# Architecture Design

## 1. Project Overview

**EdgeLightingEffect** is a modular OpenGL 3.3 Core renderer that draws animated edge strokes along a rounded-rectangle perimeter. It produces a moving light segment with glow and a particle trail. Built for macOS arm64 with CMake, GLFW, GLAD, and GLM.

### 1.1 Design Goals

- **Composable renderers** — Each visual layer is a `BaseRenderer` subclass; add/remove/reorder independently.
- **GPU-driven animation** — The moving segment head position is computed entirely in the fragment shader (`fract(uTime * uSpeed)`), keeping CPU work minimal.
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
│   │   │   ├── shader-program.h        # RAII ShaderProgram
│   │   │   └── vertex-array.h          # RAII VertexArray
│   │   └── util/
│   │       ├── geometry-utils.h        # GetPointOnRectangle (rounded corners)
│   │       └── log-util.h              # LOG macros
│   ├── src/
│   │   ├── core/edge-lighting.cpp
│   │   ├── animation/animation.cpp
│   │   └── renderer/
│   │       ├── stroke-renderer.cpp
│   │       ├── wireframe-renderer.cpp
│   │       ├── particles.cpp
│   │       └── particle-renderer.cpp
│   └── shaders/
│       ├── shaders.h.in                # CMake template → generated/shaders.h
│       ├── stroke.vert / stroke.frag   # Main stroke shader (SDF + animation + glow)
│       ├── wireframe.vert / .frag      # Bounding-box overlay
│       ├── particle.vert / .frag       # Soft-point sprite particles
│       ├── debug.vert / .frag          # 🔴 Not in build (orphaned on disk)
│       └── edge-glow.vert / .frag      # 🔴 Not in build (orphaned on disk)
│
├── demo/                           # Demo application
│   ├── CMakeLists.txt              # Links edge-lighting + glfw + OpenGL.framework
│   └── src/
│       ├── main.cpp                # GLFW loop, renderer registration, keyboard controls
│       └── ui-controls.h           # Console HUD output
│
├── external/
│   ├── include/ (glad/, GLFW/, glm/, stb/)
│   ├── src/glad.c
│   └── lib/{arm64,x86_64,universal}/libglfw.3.dylib
│
└── docs/
    ├── architecture-design.md      # This file
    ├── implementation-plan.md      # Future-phase plans
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
 │    ├── mSpeed: float                   # loops/second
 │    ├── mProgress: float [0, 1]
 │    ├── OnProgress: callback(float)
 │    └── OnLoopCompleted: callback()
 │
 └── mRenderers: vector<shared_ptr<BaseRenderer>>
      │
      ├── StrokeRenderer                 # SDF-based rounded-rect stroke
      │    ├── mShaderProgram: ShaderProgram
      │    └── mVertexArray: VertexArray  # 6-vertex quad
      │
      ├── WireframeRenderer              # GL_LINE_LOOP bounding box
      │    ├── mShaderProgram: ShaderProgram
      │    └── mVertexArray: VertexArray  # 4 vertices
      │
      └── ParticleRenderer               # Emits + renders particle trail
           ├── mParticleSystem: unique_ptr<ParticleSystem>
           │    ├── mParticles: vector<Particle>
           │    ├── mShaderProgram: ShaderProgram
           │    └── mVertexArray: VertexArray
           │    └── OnParticleSpawned: callback
           └── getRainbowColor(p)        # static helper
```

### 3.1 Lifetime & Ownership

- `EdgeLightingEffect` owns `shared_ptr<BaseRenderer>` — shared ownership allows external code to query or reconfigure individual renderers.
- `ParticleRenderer` uniquely owns `ParticleSystem` via `unique_ptr`.
- `ShaderProgram` and `VertexArray` are move-only RAII wrappers; they delete OpenGL resources on destruction.

---

## 4. Config System

### 4.1 Config Struct Hierarchy

```
Config
 ├── Geometry
 │    ├── width: float = 800.0f
 │    ├── height: float = 600.0f
 │    ├── position: vec2 = (0, 0)       # Top-left corner in app space
 │    └── borderRadius: float = 40.0f
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
| `StrokeAlignment` | `CENTER`, `INNER`, `OUTER` | 0, 1, 2 |
| `StrokeAnimation` | `STATIC`, `MOVING` | 0, 1 |
| `FadeMode` | `SINGLE`, `DOUBLE` | 0, 1 |
| `StrokeColorMode` | `STATIC`, `GRADIENT`, `RAINBOW`, `RAINBOW_TIME`, `PULSE` | 0–4 |

---

## 5. Rendering Pipeline

### 5.1 Frame Loop

```
main.cpp loop:
  1. glClear()
  2. effect->Update(deltaTime)
       ├── mAnimation.Update(deltaTime)       // advances progress, fires callbacks
       ├── mTime += deltaTime (if playing)
       └── for each renderer in order:
             renderer->Update(deltaTime, progress, mTime, mConfig)

  3. effect->Render(fbW, fbH)
       └── for each renderer in order:
             renderer->Render(vpW, vpH, progress, mTime, mConfig)

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
- Rebuilt on `OnConfigChanged()` if dimensions or margin change.

### 6.2 Uniforms (stroke.frag)

| Uniform | Type | Section |
|---|---|---|
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
| `uSpeed` | `float` | Animation |
| `uTime` | `float` | Animation |
| `uColorMode` | `int` | Color |
| `uLineCount` | `int` | Animation |
| `uGlowSize` | `float` | Glow |

### 6.3 SDF — `sdRoundedBox(p, b, r)`

```glsl
vec2 q = abs(p) - b + r;
return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
```

- Returns signed distance from point `p` to the nearest edge of a rounded rectangle.
- `d < 0` = inside, `d > 0` = outside, `d = 0` = exactly on the edge.
- Used as the foundation for all stroke rendering (thickness, alignment, fade, glow).

### 6.4 Perimeter Mapping — `getPerimeterPos(p, b, r)`

Maps a fragment's local-space position to a normalized perimeter position `[0, 1)` clockwise from the top-center.

**Sharp corners** (`r <= 0.01`): Walks top → right → bottom → left edge, computing linear fraction on each side.

**Rounded corners**: Four straight segments of length `ws = width - 2r` and `hs = height - 2r`, connected by quarter-circle arcs of length `πr/2`. Uses `atan()` to determine angular position within each arc. Nearest-side logic handles corner regions.

### 6.5 Moving Segment Animation

```glsl
float headBase = fract(uTime * uSpeed);

for each line i in [0, lineCount):
    float head = fract(headBase + i / lineCount);
    float diffPix = clockwise distance from fragment to head (in pixels);

    if (diffPix <= segmentLengthPixels):
        // Body: visible with soft tail fade over last 25%
        alpha = smoothstep(tailStart, segmentEnd, diffPix);
    else:
        // Head cap: semicircle forward of the head
        float forwardPix = totalPerimeter - diffPix;
        float capDist = sqrt(forwardPix² + edgeDist²);
        if (capDist <= limit):
            alpha = smoothstep(limit, 0, capDist);  // radial falloff

    finalAlpha = max(finalAlpha, alpha);
```

- `segmentLengthPixels = segmentLength * totalPerimeter` — segment body length.
- The head cap extends forward as a Euclidean-distance semicircle, preventing a hard cutoff at the head.
- Maximum of 16 lines (hard-coded loop bound, `lineCount` caps at 16).

### 6.6 Stroke Alignment & Fade

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

### 6.7 Glow Pass

When `glowEnable` is true, the quad is drawn **twice**:

1. **Glow pass**: `uGlowSize = glowSize`, `uIntensity = glowIntensity` — wider, fainter glow.
2. **Core pass**: `uGlowSize = 0`, `uIntensity = stroke.intensity` — core stroke on top.

The shader extends the `limit` by `glowSize`, making `smoothstep` produce a wider visible region at lower opacity. No FBO or blur needed.

### 6.8 Blend Mode

Both passes use **additive blending** (`GL_SRC_ALPHA, GL_ONE`) so glow and core accumulate naturally, and overlapping segments brighten correctly.

### 6.9 Color Modes (CPU-side available for particles)

| Mode | Value | Algorithm |
|---|---|---|
| `STATIC` | 0 | `primaryColor` (uniform across stroke) |
| `GRADIENT` | 1 | `mix(primary, secondary, 1.0 - abs(2*t - 1))` — triangle wave perimeter |
| `RAINBOW` | 2 | `hsv2rgb(vec3(t, 0.8, 1.0))` — hue = perimeter position |
| `RAINBOW_TIME` | 3 | `hsv2rgb(vec3(fract(time*0.15), 0.8, 1.0))` — uniform hue over time |
| `PULSE` | 4 | `mix(primary, secondary, sin(time*2)*0.5+0.5)` — oscillation |

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

- `ParticleRenderer` is a `BaseRenderer` that composites a `ParticleSystem` (non-renderer helper class).
- `ParticleRenderer::Update()` calls `emitParticlesAtHead()` (when animation is MOVING) then delegates to `mParticleSystem->Update()`.
- `ParticleRenderer::Render()` computes the local→NDC offset and delegates to `mParticleSystem->Render()`.

### 8.2 Head Position Sync

Particles spawn at the *same* position as the stroke shader's moving head:

```cpp
float headProgress = glm::fract(time * config.stroke.speed + offset);
glm::vec2 spawnPos = GeometryUtils::GetPointOnRectangle(headProgress, config.geometry);
```

This matches the shader's `fract(uTime * uSpeed)` formula exactly, keeping particles and stroke in sync.

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
    float mSpeed = 1.0;       // Loops per second
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
- **Speed**: Independent from `config.stroke.speed` (animation speed ≠ stroke segment speed).
- `EdgeLightingEffect` accumulates `mTime` separately (only when playing), providing a continuous clock for the shader independent of progress wrapping.

---

## 10. Coordinate Spaces

| Space | Origin | +X | +Y | Used by |
|---|---|---|---|---|
| **App** | viewport top-left | right | down | `Config::Geometry::position` |
| **Local** | rectangle center | right | up | SDF, GetPointOnRectangle, particles |
| **OpenGL** | viewport bottom-left | right | up | MVP model `translate` |
| **NDC** | screen center | right | up | `gl_Position` |

**Key conversion** (app-to-OpenGL Y flip):

```cpp
center_ogl.y = viewportH - position.y - halfH;
```

---

## 11. RAII OpenGL Wrappers

### 11.1 ShaderProgram

- Constructor compiles and links a vertex + fragment shader pair.
- `SetUniform(name, value)` — typed wrappers for `int`, `float`, `vec2`, `vec4`, `mat4`.
- Move-only (deleted copy). Destructor calls `glDeleteProgram()`.
- Logs compile/link errors through `LOG_E`; `mId = 0` on failure.

### 11.2 VertexArray

- Constructor creates VAO + VBO via `glGenVertexArrays` + `glGenBuffers`.
- `SetVertexData(data, size, usage)` — uploads vertices to VBO.
- `SetAttribPointer(location, size, type, stride, offset)` — configures vertex attribute.
- `DrawArrays(mode, count)` — binds VAO, draws, unbinds.
- `BindBuffer()` — required before `glBufferSubData` (VAO doesn't restore `GL_ARRAY_BUFFER`).
- Move-only. Destructor deletes VBO then VAO.

---

## 12. Build System

### 12.1 Structure

```
CMakeLists.txt (root)
 ├── add_library(glad STATIC external/src/glad.c)
 ├── add_library(glfw SHARED IMPORTED)          # external/lib/arm64/libglfw.3.dylib
 ├── add_subdirectory(lib)
 │    ├── file(READ ...) 6 shader files
 │    ├── configure_file(shaders.h.in → generated/shaders.h)
 │    ├── CMAKE_CONFIGURE_DEPENDS on all 6 shader files
 │    └── add_library(edge-lighting STATIC)
 │         ├── 6 source files
 │         ├── target_include: lib/include/, build/lib/generated/
 │         └── target_link: glad
 └── add_subdirectory(demo)
      └── add_executable(edge-lighting-demo)
           └── target_link: edge-lighting, glfw, OpenGL.framework
```

### 12.2 Shader Embedding

At configure time, CMake reads each `.vert`/`.frag` file content into CMake variables (`@STROKE_VERT_CONTENT@`, etc.), then substitutes them into `shaders.h.in` via `configure_file()`. The generated `shaders.h` exposes `ShaderSource::STROKE_VERT_SRC` etc. as `const char*` raw string literals.

When a shader file is edited, `CMAKE_CONFIGURE_DEPENDS` triggers a re-configure, which regenerates the header and recompiles all translation units that include it.

---

## 13. Orphaned / Removed Files

| Path | Status | Reason |
|---|---|---|
| `lib/shaders/edge-glow.vert` | On disk, not in build | Folded into stroke shader as two-pass glow |
| `lib/shaders/edge-glow.frag` | On disk, not in build | Same |
| `lib/shaders/debug.vert` | On disk, not in build | Replaced by WireframeRenderer |
| `lib/shaders/debug.frag` | On disk, not in build | Same |
| `lib/include/renderer/edge-glow-renderer.h` | Source removed | Folded into StrokeRenderer glow pass |
| `lib/src/renderer/edge-glow-renderer.cpp` | Source removed | Same |
| `lib/include/renderer/debug-renderer.h` | Source removed | Replaced by WireframeRenderer |
| `lib/src/renderer/debug-renderer.cpp` | Source removed | Same |
| `lib/include/renderer/segment-renderer.h` | Source removed | Renamed to StrokeRenderer |
| `lib/src/renderer/segment-renderer.cpp` | Source removed | Same |
| Config `Light` sub-struct / `LightMode` / `ColorMode` | Removed | Folded into `Stroke` sub-struct |

---

## 14. Design Patterns

| Pattern | Where |
|---|---|
| **Strategy** | `BaseRenderer` interface with 3 concrete implementations |
| **Observer** | `Animation::OnProgress`, `Animation::OnLoopCompleted`, `ParticleSystem::OnParticleSpawned`, `BaseRenderer::OnConfigChanged` |
| **RAII** | `ShaderProgram`, `VertexArray` — acquire in ctor, release in dtor, move-only |
| **Composition** | `ParticleRenderer` owns `ParticleSystem` via `unique_ptr` |
| **Template Method** | `BaseRenderer` defines skeleton; subclasses fill in virtual methods |
| **Singleton (local static)** | RNG in `particles.cpp` (`static std::mt19937 instance{1337}`) |
| **Command** | Demo `OnKey` maps key presses to config mutations |
| **Data-Oriented** | Particles stored in flat `vector<Particle>`, transferred as `ParticleVertex[]` to GPU |
| **Shader-as-Data** | Shaders embedded as C++ string literals at build time |
