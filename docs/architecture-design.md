# Architecture Design

## 1. Project Overview

**EdgeLightingEffect** is a modular OpenGL 3.3 Core renderer that draws animated edge strokes along a rounded-rectangle perimeter. It produces a moving light segment with glow and a particle trail. Supports multiple lighting path sources (RECT, CUSTOM, MASK) with configurable start/end points and traversal direction (CW/CCW). Built for macOS arm64 with CMake, GLFW, GLAD, and GLM.

### 1.1 Design Goals

- **Composable renderers** — Each visual layer is a `BaseRenderer` subclass; add/remove/reorder independently.
- **CPU-driven head position** — The moving head position is computed once per frame by `EdgeLightingEffect` (with start/end mapping and winding) and passed as a uniform, keeping the shader stateless.
- **Polyline SDF branching** — CUSTOM/MASK paths use a 1D texture-based polyline SDF (`sdPolyline`) in the shader, while RECT uses the analytic `sdRoundedBox`.
- **Single-pass glow** — Fake double-pass glow (wider + fainter behind core stroke) avoids expensive FBO/blur.
- **No runtime file I/O** — Shaders are embedded as C++ string literals at build time via CMake `configure_file()`.

---

## 2. Directory Structure

```
/
├── CMakeLists.txt                  # Root build: GLAD static lib, GLFW imported, subdirs
├── AGENTS.md                       # Naming conventions
│
├── lib/                            # Library — builds edge-lighting.a
│   ├── CMakeLists.txt              # Reads shaders, generates shaders.h, builds static lib
│   ├── include/
│   │   ├── core/
│   │   │   ├── config.h                # Config + all sub-structs + enums
│   │   │   └── edge-lighting.h         # EdgeLightingEffect orchestrator
│   │   ├── renderer/
│   │   │   ├── base-renderer.h         # Abstract BaseRenderer interface
│   │   │   ├── stroke-renderer.h       # StrokeRenderer
│   │   │   ├── wireframe-renderer.h    # WireframeRenderer
│   │   │   ├── particle-renderer.h     # ParticleRenderer (owns ParticleSystem)
│   │   │   └── particles.h             # Particle struct + ParticleSystem
│   │   ├── animation/
│   │   │   └── animation.h             # Animation looping timer
│   │   ├── gl/
│   │   │   ├── gl-header.h             # #include <glad/glad.h>
│   │   │   ├── texture.h               # Base Texture RAII class (gen/delete/move)
│   │   │   ├── texture-1d.h            # Texture1D : Texture (1D RG32F for path points)
│   │   │   ├── texture-2d.h            # Texture2D : Texture (2D with SetDataFromFile)
│   │   │   ├── shader-program.h        # RAII ShaderProgram
│   │   │   └── vertex-array.h          # RAII VertexArray
│   │   └── util/
│   │       ├── geometry-utils.h        # GetPointOnRectangle (CW/CCW, rounded corners), AppToLocal
│   │       ├── path-utils.h            # GetPointOnPath, GetPathAABB, GetPathLength
│   │       ├── contour-tracer.h        # TraceOutermostContour (Marching Squares)
│   │       ├── stb-image.h             # Wrapper for stb/stb_image.h
│   │       └── log-util.h              # LOG macros
│   ├── src/
│   │   ├── core/edge-lighting.cpp
│   │   ├── animation/animation.cpp
│   │   ├── renderer/
│   │   │   ├── stroke-renderer.cpp
│   │   │   ├── wireframe-renderer.cpp
│   │   │   ├── particles.cpp
│   │   │   └── particle-renderer.cpp
│   │   └── util/
│   │       └── stb-image.cpp            # STB_IMAGE_IMPLEMENTATION
│   └── shaders/
│       ├── shaders.h.in                # CMake template → generated/shaders.h
│       ├── stroke.vert / stroke.frag   # Main stroke shader (SDF + animation + glow + polyline)
│       ├── wireframe.vert / .frag      # Bounding-box overlay
│       └── particle.vert / .frag       # Soft-point sprite particles
│
├── demo/                           # Demo application
│   ├── CMakeLists.txt              # Links edge-lighting + glfw + OpenGL.framework, defines RES_DIR
│   └── src/
│       ├── main.cpp                # GLFW loop, renderer registration, keyboard controls
│       └── ui-controls.h           # Console HUD output
│
├── res/
│   └── mask.png                    # Test mask image (800×800 anti-aliased circle)
│
├── external/
│   ├── include/ (glad/, GLFW/, glm/, stb/)
│   ├── src/glad.c
│   └── lib/{arm64,x86_64,universal}/libglfw.3.dylib
│
└── docs/
    ├── architecture-design.md      # This file
    ├── implementation-plan.md      # Phase-completed plan
    └── coordinate-system.md        # Coordinate space conversions
```

---

## 3. Class Hierarchy & Relationships

```
EdgeLightingEffect  (orchestrator)
 │
 ├── mConfig: Config
 ├── mTime: float                         # Accumulated wall-clock (seconds)
 ├── mAnimation: Animation
 │    ├── mIsPlaying: bool
 │    ├── mSpeed: float                   # loops/second (synced to config.stroke.speed)
 │    ├── mProgress: float [0, 1]
 │    ├── OnProgress: callback(float)
 │    └── OnLoopCompleted: callback()
 │
 └── mRenderers: vector<shared_ptr<BaseRenderer>>
      │
      ├── StrokeRenderer                 # SDF-based stroke (RECT + CUSTOM/MASK via polyline)
      │    ├── mShaderProgram: ShaderProgram
      │    ├── mVertexArray: VertexArray  # 6-vertex quad (AABB-sized for CUSTOM/MASK)
      │    └── mPathTexture: Texture1D    # RG32F path points (reversed for CW winding)
      │
      ├── WireframeRenderer              # GL_LINE_LOOP bounding box
      │    ├── mShaderProgram: ShaderProgram
      │    └── mVertexArray: VertexArray  # 4 vertices
      │
      └── ParticleRenderer               # Emits + renders particle trail
           └── mParticleSystem: unique_ptr<ParticleSystem>
                ├── mParticles: vector<Particle>
                ├── mShaderProgram: ShaderProgram
                └── mVertexArray: VertexArray
```

### 3.1 Lifetime & Ownership

- `EdgeLightingEffect` owns `shared_ptr<BaseRenderer>` — shared ownership allows external code to query or reconfigure individual renderers.
- `ParticleRenderer` uniquely owns `ParticleSystem` via `unique_ptr`.
- `ShaderProgram`, `VertexArray`, `Texture`, `Texture1D`, `Texture2D` are move-only RAII wrappers; they delete OpenGL resources on destruction.

---

## 4. Config System

### 4.1 Config Struct Hierarchy

```
Config
 ├── Geometry
 │    ├── width: float = 800.0f
 │    ├── height: float = 600.0f
 │    ├── position: vec2 = (0, 0)       # Top-left corner in app space
 │    ├── borderRadius: float = 40.0f
 │    └── winding: Winding = COUNTER_CLOCKWISE   # Traversal direction (CW/CCW)
 │
 ├── Stroke
 │    ├── enable: bool = true
 │    ├── thickness: float = 3.0f
 │    ├── intensity: float = 1.0f
 │    ├── alignment: StrokeAlignment
 │    ├── primaryColor: vec4 = (1,0,0,1)
 │    ├── secondaryColor: vec4 = (0,0,1,1)
 │    ├── colorMode: StrokeColorMode
 │    ├── fadeRange: float = 0.0f
 │    ├── fadeMode: FadeMode
 │    ├── animation: StrokeAnimation
 │    ├── segmentLength: float = 0.25f
 │    ├── speed: float = 0.5f
 │    ├── lineCount: int = 1          # [1, 16]
 │    ├── glowEnable: bool = false
 │    ├── glowSize: float = 5.0f
 │    └── glowIntensity: float = 0.25f
 │
 ├── Wireframe
 │    ├── enable: bool = true
 │    └── color: vec4 = (0,1,0,1)
 │
 ├── Path
 │    ├── source: PathSource = RECT       # RECT, CUSTOM, MASK
 │    ├── closed: bool = true             # Closed loop or open path
 │    ├── startPos: float = 0.0           # Head start [0, 1]
 │    ├── endPos: float = 1.0             # Head end [0, 1]
 │    └── points: vec2[]                  # Polyline (for CUSTOM/MASK)
 │
 └── Particles
      ├── enable: bool = true
      ├── maxCount: int = 200
      ├── size: float = 6.0f
      ├── intensity: float = 1.0f
      └── color: vec4 = (0,0,0,0)      # Zero alpha = auto from stroke mode
```

### 4.2 Enumerations

| Enum | Values | Integer |
|---|---|---|
| `PathSource` | `RECT`, `CUSTOM`, `MASK` | 0, 1, 2 |
| `StrokeAlignment` | `CENTER`, `INNER`, `OUTER` | 0, 1, 2 |
| `StrokeAnimation` | `STATIC`, `MOVING` | 0, 1 |
| `FadeMode` | `SINGLE`, `DOUBLE` | 0, 1 |
| `StrokeColorMode` | `STATIC`, `GRADIENT`, `RAINBOW`, `RAINBOW_TIME`, `PULSE` | 0–4 |
| `Winding` | `CLOCKWISE`, `COUNTER_CLOCKWISE` | 0, 1 |

---

## 5. Rendering Pipeline

### 5.1 Frame Loop

```
main.cpp loop:
  1. glClear()
  2. effect->Update(deltaTime)
       ├── mAnimation.Update(deltaTime)       // advances progress, fires callbacks
       ├── mTime += deltaTime (if playing)
       ├── computeHeadPos()                   // maps progress → headPos via startPos/endPos
       └── for each renderer in order:
             renderer->Update(deltaTime, progress, headPos, mTime, mConfig)

  3. effect->Render(fbW, fbH)
       └── for each renderer in order:
             renderer->Render(vpW, vpH, progress, headPos, mTime, mConfig)

  4. glfwSwapBuffers()
```

### 5.2 Registration Order (in `main.cpp`)

1. **StrokeRenderer** — SDF-based edge stroke (bottommost layer)
2. **WireframeRenderer** — Green bounding box overlay
3. **ParticleRenderer** — Particle trail on top

### 5.3 MVP Matrix (shared convention)

```cpp
proj = ortho(0, viewportWidth, 0, viewportHeight, -1, 1)
center_ogl = (pos.x + halfW, viewportH - pos.y - halfH)   // Y flip
model = translate(center_ogl)
mvp = proj * model
```

All renderers use this same formula, so local-space quad vertices (origin at center, +Y up) render correctly on screen (origin at top-left, +Y down).

---

## 6. StrokeRenderer — Core Edge Stroke

### 6.1 Vertex Geometry

- A 6-vertex quad (2 triangles) covering `±(halfSize + margin)` in local space.
- `margin = thickness + glowSize + 5.0f` — enough room for glow falloff and all alignment modes.
- For CUSTOM/MASK modes: quad sized to path AABB (via `PathUtils::GetPathAABB`) + margin.
- For RECT mode: quad sized to rectangle half-extents + margin.
- Rebuilt on `OnConfigChanged()`.

### 6.2 Uniforms (stroke.frag)

| Uniform | Type | Section |
|---|---|---|
| `uMVP` | `mat4` | Transform |
| `uRectSize` | `vec2` | Geometry |
| `uBorderRadius` | `float` | Geometry |
| `uStrokeThickness` | `float` | Stroke shape |
| `uIntensity` | `float` | Global opacity |
| `uPrimaryColor` | `vec4` | Color |
| `uSecondaryColor` | `vec4` | Color |
| `uStrokeAlignment` | `int` | Stroke shape |
| `uFadeRange` | `float` | Feathering |
| `uFadeMode` | `int` | Feathering |
| `uStrokeAnimation` | `int` | Animation |
| `uSegmentLength` | `float` | Animation |
| `uHeadPosition` | `float` | Animation — CPU-mapped head pos [0, 1] |
| `uTime` | `float` | Animation |
| `uColorMode` | `int` | Color |
| `uLineCount` | `int` | Animation |
| `uGlowSize` | `float` | Glow |
| `uWinding` | `int` | 0=CW, 1=CCW — controls getPerimeterPos direction |
| `uPathSource` | `int` | Path — 0=RECT, 1=CUSTOM, 2=MASK |
| `uPathTexture` | `sampler1D` | Path — polyline points as RG32F (CUSTOM/MASK) |
| `uPathPointCount` | `int` | Path — number of polyline points |
| `uPathClosed` | `bool` | Path — is the path closed? |
| `uPathTotalLength` | `float` | Path — total polyline length (CPU-computed) |

### 6.3 SDF — `sdRoundedBox(p, b, r)`

```glsl
vec2 q = abs(p) - b + r;
return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
```

- Returns signed distance from point `p` to the nearest edge of a rounded rectangle.
- `d < 0` = inside, `d > 0` = outside, `d = 0` = exactly on the edge.
- Used only for RECT mode. CUSTOM/MASK modes use `sdPolyline`.

### 6.4 Perimeter Mapping — `getPerimeterPos(p, b, r)`

Branches on `uWinding`:

- **CW** (`uWinding == 0`): `getPerimeterPosCW` — top→right→bottom→left from top-left.
- **CCW** (`uWinding == 1`): `getPerimeterPosCCW` — left→bottom→right→top from top-left.

Both handle sharp corners (linear edge fractions) and rounded corners (quarter-circle arcs via `atan()`).

### 6.5 Polyline SDF — `sdPolyline(p)`

Used for CUSTOM/MASK paths. Iterates over segments stored in `uPathTexture` (1D, `GL_RG32F`):

```glsl
for each segment i:
    vec2 a = texelFetch(uPathTexture, i, 0).xy;
    vec2 b = texelFetch(uPathTexture, (i+1) % n, 0).xy;

    // closest point on segment, signed distance via cross product
    // perimPos = cumulative length + t * segLen, normalized by uPathTotalLength
    // returns vec2(signedDist, perimPos)
```

Returns both signed distance and perimeter position in a single O(N) loop. All downstream rendering (thickness, alignment, fade, glow, animation, color) is **path-agnostic**.

### 6.6 Moving Segment Animation

```glsl
float headBase = uHeadPosition;   // CPU-computed, maps through startPos/endPos

for each line i in [0, lineCount):
    float head = fract(headBase + i / lineCount);
    float diffPix = (head - perimPos) wrapped forward (in pixels);

    if (diffPix <= segmentLengthPixels):
        // Body: visible with soft tail fade over last 25%
        alpha = smoothstep(tailStart, segmentEnd, diffPix);
    else:
        // Head cap: semicircle forward of the head
        float forwardPix = totalPerimeter - diffPix;
        float capDist = sqrt(forwardPix² + edgeDist²);
        if (capDist <= limit):
            alpha = smoothstep(limit, 0, capDist);

    finalAlpha = max(finalAlpha, alpha);
```

- `segmentLengthPixels = segmentLength * totalPerimeter`.
- The head cap extends forward as a Euclidean-distance semicircle, preventing a hard cutoff.
- Maximum of 16 lines (hard-coded loop bound, `lineCount` caps at 16).
- Uses `uHeadPosition` directly (not `fract(uTime * uSpeed)`), enabling start/end mapping.

### 6.7 Stroke Alignment & Fade

| Alignment | Limit | EdgeDist | Discard condition |
|---|---|---|---|
| `CENTER` (0) | `thickness/2 + glowSize` | `abs(d)` | — |
| `INNER` (1) | `thickness + glowSize` | `abs(d)` | `d > 0` (outside) |
| `OUTER` (2) | `thickness + glowSize` | `d` | `d < 0` (inside) |

**Fade** (SINGLE mode):
```glsl
lineAlpha = 1.0 - smoothstep(limit - fadeRange, limit, edgeDist);
```
**DOUBLE mode**: Also fades the inner edge:
```glsl
lineAlpha *= smoothstep(0, fadeRange, edgeDist);
```

### 6.8 Glow Pass

When `glowEnable` is true, the quad is drawn **twice**:

1. **Glow pass**: `uGlowSize = glowSize`, `uIntensity = glowIntensity` — wider, fainter glow.
2. **Core pass**: `uGlowSize = 0`, `uIntensity = stroke.intensity` — core stroke on top.

The shader extends the `limit` by `glowSize`, making `smoothstep` produce a wider visible region at lower opacity. No FBO or blur needed.

### 6.9 Blend Mode

Both passes use **additive blending** (`GL_SRC_ALPHA, GL_ONE`) so glow and core accumulate naturally, and overlapping segments brighten correctly.

### 6.10 Texture Upload — `uploadPathTexture()`

- RECT mode: uploads a 1-pixel fallback texture (avoids macOS GL warning about unbound 1D sampler).
- CUSTOM/MASK modes:
  1. Converts points from app-space to local-space via `GeometryUtils::AppToLocal`.
  2. If `winding == CLOCKWISE`, reverses the point order with `std::reverse`.
  3. Uploads as `Texture1D` with `GL_RG32F` format.
- Called from `Initialize()` and `OnConfigChanged()`.

### 6.11 Head Position

`EdgeLightingEffect::computeHeadPos()` determines the effective head position:

- **Full loop** (`closed && startPos == endPos`): Uses `fract(time * speed)`.
- **Start→End** (all other cases): Uses `mix(startPos, endPos, animationProgress)`.

For open paths, `OnLoopCompleted` pauses the animation to prevent wrapping. The mapped `headPos` is passed as `uHeadPosition` uniform.

For CUSTOM/MASK with CW winding, the uploaded texture has reversed points, so the shader's `sdPolyline` traversal direction matches the head movement. Particles also use reversed progress (`1.0 - headProgress`) when calling `GetPointOnPath`.

### 6.12 Color Modes (CPU-side available for particles)

| Mode | Value | Algorithm |
|---|---|---|
| `STATIC` | 0 | `primaryColor` (uniform across stroke) |
| `GRADIENT` | 1 | `mix(primary, secondary, 1.0 - abs(2*t - 1))` |
| `RAINBOW` | 2 | `hsv2rgb(vec3(t, 0.8, 1.0))` — hue = perimPos |
| `RAINBOW_TIME` | 3 | `hsv2rgb(vec3(fract(time*0.15), 0.8, 1.0))` |
| `PULSE` | 4 | `mix(primary, secondary, sin(time*2)*0.5+0.5)` |

---

## 7. WireframeRenderer

### 7.1 Geometry

- 4 vertices at rectangle corners in local space.
- Drawn with `GL_LINE_LOOP` (1px wide, no custom line width control).

### 7.2 Rendering

- Temporarily **disables blending** to get crisp 1px lines.
- Draws after stroke layer so it's visible on top.
- Re-enables blend afterward.

---

## 8. ParticleRenderer + ParticleSystem

### 8.1 Architecture

- `ParticleRenderer` is a `BaseRenderer` that composites a `ParticleSystem`.
- `ParticleRenderer::Update()` calls `emitParticlesAtHead()` (when animation is MOVING) then delegates to `mParticleSystem->Update()`.
- `ParticleRenderer::Render()` computes the local→NDC offset and delegates to `mParticleSystem->Render()`.

### 8.2 Head Position Sync

Particles spawn at the moving head position:

```cpp
float headProgress = glm::fract(headPos + offset);

// RECT mode:
spawnPos = GeometryUtils::GetPointOnRectangle(headProgress, config.geometry);

// CUSTOM/MASK mode:
float pathProgress = (winding == CLOCKWISE) ? 1.0f - headProgress : headProgress;
glm::vec2 appPos = PathUtils::GetPointOnPath(pathProgress, points, closed);
spawnPos = GeometryUtils::AppToLocal(appPos, config.geometry);
```

This matches the shader's traversal, accounting for winding direction.

### 8.3 Particle Color

Color is derived from the stroke's color mode at the spawn point:

| Stroke color mode | Particle color |
|---|---|
| `STATIC` | `stroke.primaryColor` |
| `GRADIENT` | `mix(primary, secondary, 1.0 - abs(2*t - 1))` |
| `RAINBOW` / `RAINBOW_TIME` / `PULSE` | `getRainbowColor(fract(headProgress - time*0.25))` |

If `config.particles.color.a > 0`, the user-override color is used instead.

### 8.4 Particle System Internals

```
Particle {
    vec2 position;     // Local coords
    vec2 velocity;
    vec4 color;
    float life;        // Remaining seconds
    float maxLife;     // For fade ratio (life/maxLife)
    float size;        // Point size in pixels
}

Each frame:
  position += velocity * deltaTime
  velocity *= exp(-2.0 * deltaTime)   // Exponential drag
  life -= deltaTime

Emission:
  Random angle 0-2π
  Velocity magnitude: random(30, 120) * speed
  Life: random(0.3, 0.8) sec
  Size: random(0.5, 1.5) * mGlobalSize
  Recycles dead particles first; appends if under max

Rendering:
  - Collect alive particles into ParticleVertex[] (position + color + size)
  - Upload via glBufferSubData (DYNAMIC_DRAW)
  - Draw as GL_POINTS with additive blending
  - Fragment shader: smoothstep(0.5, 0.2, dist) circular soft sprite
```

---

## 9. Animation System

```cpp
class Animation {
    bool mIsPlaying = true;
    float mSpeed = 1.0;       // Loops per second (synced to config.stroke.speed)
    float mProgress = 0.0;    // [0, 1]

    void Update(deltaTime):
        if !playing: return
        progress += speed * deltaTime
        if progress >= 1.0:
            progress = fract(progress)
            OnLoopCompleted()
        OnProgress(progress)
};
```

- **Play/Pause/Stop**: Control `mIsPlaying`.
- **Speed**: Synced from `config.stroke.speed` via `mAnimation.SetSpeed()` in `EdgeLightingEffect::SetConfig()`.
- `EdgeLightingEffect` accumulates `mTime` separately (only when playing), providing a continuous clock for the shader.

---

## 10. Path Source Branching

### 10.1 RECT

- Uses analytic `sdRoundedBox` SDF + `getPerimeterPos` (CW/CCW based on `uWinding`).
- Head position via `GeometryUtils::GetPointOnRectangle` with `geom.winding`.

### 10.2 CUSTOM

- Polyline points provided via `Config::Path::points` in app space.
- Uploaded to `Texture1D` (local-space), reversed for CW winding.
- Shader uses `sdPolyline` to compute signed distance and perimPos.
- Particles use `PathUtils::GetPointOnPath` with reversed progress for CW.

### 10.3 MASK

- Loaded via `EdgeLightingEffect::SetMaskFromFile()` / `SetMaskFromPixels()`.
- Contour extracted with `TraceOutermostContour()` (Marching Squares with sub-pixel interpolation).
- Contour points stored in `Config::Path::points`, source set to `MASK`, closed = true.
- Rendered identically to CUSTOM mode (same `sdPolyline` path).
- Mask points stored in `gStoredMaskPoints` in the demo to persist across path source cycling.

### 10.4 Winding

- `Winding::COUNTER_CLOCKWISE` (default): vertex order defines the traversal direction.
- `Winding::CLOCKWISE`: reverses the vertex order for CUSTOM/MASK (texture upload + particle lookup).
- For RECT, the `getPerimeterPosCW`/`getPerimeterPosCCW` shader functions are selected accordingly.
- No automatic winding detection — the vertex order is taken as-is.

---

## 11. Geometry Utility Functions

### 11.1 `GeometryUtils::GetPointOnRectangle(t, geom)`

Branches on `geom.winding` to call `Detail::GetPointOnRectCW` (top→right→bottom→left) or `Detail::GetPointOnRectCCW` (left→bottom→right→top), both starting from top-left. Supports rounded corners via `borderRadius`.

### 11.2 `GeometryUtils::AppToLocal(point, geom)`

Converts app-space points (rect top-left = 0,0, +Y down) to local space (rect center = 0,0, +Y up): `vec2(pt.x - halfW, halfH - pt.y)`.

### 11.3 `PathUtils::GetPointOnPath(t, points, closed)`

Returns a point on a polyline at progress `t ∈ [0, 1]`. For closed paths, `t = 1.0` correctly returns the first point (via the closing segment).

### 11.4 `PathUtils::GetPathLength(points, closed)`

Returns the total length of a polyline, including the closing segment for closed paths.

### 11.5 `PathUtils::GetPathAABB(points, center, halfSize)`

Returns the axis-aligned bounding box center and half-extents of a set of points.

### 11.6 `TraceOutermostContour(pixels, w, h, rectW, rectH, ...)`

Marching Squares with sub-pixel linear interpolation. Handles saddle cases (patterns 6, 9) via average-corner-alpha vs threshold. Chains segments into closed polyline, filters zero-length segments, resamples to max 512 points. Returns points in app space.

---

## 12. RAII OpenGL Wrappers

### 12.1 Texture (base)

- `Texture()` — `glGenTextures(1, &mId)`
- `~Texture()` — `glDeleteTextures(1, &mId)`
- Move-only. Provides `GetId()`, `IsValid()`.

### 12.2 Texture1D : Texture

- `Bind(unit)`, `Unbind(unit)` — `glActiveTexture + glBindTexture(GL_TEXTURE_1D)`.
- `SetData(data, count, ...)` — `glTexImage1D(GL_TEXTURE_1D, ..., GL_RG32F, count, ..., GL_RG, GL_FLOAT, data)`.
- `SetParams(minFilter, magFilter, wrapS)` — nearest filtering, clamp-to-edge by default.

### 12.3 Texture2D : Texture

- `SetData(data, w, h, ...)` — `glTexImage2D`.
- `SetDataFromFile(path, ...)` — loads via stb_image, uploads, frees.
- `SetParams(minFilter, magFilter, wrapS, wrapT)` — linear filtering, clamp-to-edge by default.

### 12.4 ShaderProgram

- Constructor compiles and links a vertex + fragment shader pair.
- `SetUniform(name, value)` — typed wrappers for `int`, `float`, `vec2`, `vec4`, `mat4`.
- Move-only (deleted copy). Destructor calls `glDeleteProgram()`.
- Logs compile/link errors through `LOG_E`; `mId = 0` on failure.

### 12.5 VertexArray

- Constructor creates VAO + VBO via `glGenVertexArrays` + `glGenBuffers`.
- `SetVertexData(data, size, usage)` — uploads vertices to VBO.
- `SetAttribPointer(location, size, type, stride, offset)` — configures vertex attribute.
- `DrawArrays(mode, count)` — binds VAO, draws, unbinds.
- `BindBuffer()` — required before `glBufferSubData` (VAO doesn't restore `GL_ARRAY_BUFFER`).
- Move-only. Destructor deletes VBO then VAO.

---

## 13. Build System

### 13.1 Structure

```
CMakeLists.txt (root)
 ├── add_library(glad STATIC external/src/glad.c)
 ├── add_library(glfw SHARED IMPORTED)          # external/lib/arm64/libglfw.3.dylib
 ├── add_subdirectory(lib)
 │    ├── file(READ ...) shader files
 │    ├── configure_file(shaders.h.in → generated/shaders.h)
 │    ├── CMAKE_CONFIGURE_DEPENDS on shader files
 │    └── add_library(edge-lighting STATIC)
 │         ├── source files (including util/stb-image.cpp, util/contour-tracer.cpp)
 │         ├── target_include: lib/include/, build/lib/generated/
 │         └── target_link: glad
 └── add_subdirectory(demo)
      ├── target_compile_definitions: RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../res"
      └── add_executable(edge-lighting-demo)
           └── target_link: edge-lighting, glfw, OpenGL.framework
```

### 13.2 Shader Embedding

At configure time, CMake reads each `.vert`/`.frag` file content into CMake variables (`@STROKE_VERT_CONTENT@`, etc.), then substitutes them into `shaders.h.in` via `configure_file()`. The generated `shaders.h` exposes `ShaderSource::STROKE_VERT_SRC` etc. as `const char*` raw string literals.

`CMAKE_CONFIGURE_DEPENDS` triggers re-configure when shader files are edited.

### 13.3 RES_DIR

The demo binary receives `RES_DIR` as a compile definition set to the absolute path of the `res/` directory. The mask file path is `RES_DIR "/mask.png"`, making it resolve correctly regardless of working directory.

---

## 14. Demo Application

### 14.1 Key Controls

| Key | Action |
|---|---|
| R / F | Inc / Dec Stroke Thickness |
| K / J | Inc / Dec Corner Radius |
| I / O | Inc / Dec Stroke Intensity |
| T | Cycle Alignment (CENTER / INNER / OUTER) |
| H / Shift+H | Inc / Dec Fade Range |
| B | Cycle Fade Mode (SINGLE / DOUBLE) |
| C | Cycle Color Mode |
| , / . | Dec / Inc Line Count |
| M | Toggle Animation (STATIC / MOVING) |
| U / Y | Inc / Dec Segment Length |
| P / L | Inc / Dec Speed |
| SPACE | Pause / Resume Animation |
| N | Toggle Particle Trail |
| V | Toggle Glow |
| [ / ] | Inc / Dec Glow Size |
| ; / ' | Inc / Dec Glow Intensity |
| G | Toggle Wireframe |
| X | Cycle Path Source (RECT / CUSTOM / MASK) |
| W | Toggle Winding (CW / CCW) |
| 1 / 2 | Dec / Inc Start Pos |
| 3 / 4 | Dec / Inc End Pos |
| 5 | Toggle Path Closed / Open |
| 6 | Switch to Diamond path (CUSTOM) |
| Z | Load mask from res/mask.png, switch to MASK |
| ESC | Exit |

### 14.2 HUD

The console HUD (via `PrintCurrentConfig`) shows:
- Stroke thickness, corner radius, intensity, alignment
- Fade range and mode
- Animation mode, line count, segment length, speed
- Color mode and fade mode
- Glow status
- Particle trail status
- Path source (RECT/CUST/MASK), closed/open, winding (CW/CCW), start/end positions
- Animation play/pause status
- Wireframe on/off

### 14.3 Preset Paths

- `MakeTrianglePath()` — 3-point triangle near rectangle edges.
- `MakeDiamondPath()` — 4-point diamond near rectangle edges.
- Both return hardcoded app-space pixel coordinates.

---

## 15. Coordinate Spaces

| Space | Origin | +X | +Y | Used by |
|---|---|---|---|---|
| **App** | viewport top-left | right | down | `Config::Geometry::position`, preset paths, mask contour output |
| **Local** | rectangle center | right | up | SDF, GetPointOnRectangle, particles, uploaded path texture |
| **OpenGL** | viewport bottom-left | right | up | MVP model `translate` |
| **NDC** | screen center | right | up | `gl_Position` |

**Key conversion** (app-to-OpenGL Y flip):
```cpp
center_ogl.y = viewportH - position.y - halfH;
```

---

## 16. Design Patterns

| Pattern | Where |
|---|---|
| **Strategy** | `BaseRenderer` interface with 3 concrete implementations |
| **Observer** | `Animation::OnProgress`, `Animation::OnLoopCompleted`, `BaseRenderer::OnConfigChanged` |
| **RAII** | `ShaderProgram`, `VertexArray`, `Texture`, `Texture1D`, `Texture2D` |
| **Composition** | `ParticleRenderer` owns `ParticleSystem` via `unique_ptr` |
| **Template Method** | `BaseRenderer` defines skeleton; subclasses fill in virtual methods |
| **Singleton (local static)** | RNG in `particles.cpp` (`static std::mt19937 instance{1337}`) |
| **Command** | Demo `OnKey` maps key presses to config mutations |
| **Data-Oriented** | Particles stored in flat `vector<Particle>`, transferred as `ParticleVertex[]` to GPU |
| **Shader-as-Data** | Shaders embedded as C++ string literals at build time |
