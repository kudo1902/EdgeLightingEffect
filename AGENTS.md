# EdgeLightingEffect - Naming Conventions

## Files
- Source/header files: `kebab-case`
  - e.g. `edge-lighting.h`, `segment-renderer.cpp`, `particle-renderer.h`

## Namespaces
- `PascalCase`
  - e.g. `EdgeLighting`, `EdgeLightingDemo`, `GeometryUtils`

## Classes
- `PascalCase`
- Examples: `EdgeLightingEffect`, `BaseRenderer`, `SegmentRenderer`, `ParticleSystem`, `Particle`, `Animation`

## Structs / Enums
- `PascalCase`
- Must have `typedef` alias (same name, C++ style)
  - `typedef struct Config { ... } Config;`
  - `typedef enum class ColorMode { ... } ColorMode;`
- Examples: `Config`, `ColorMode`

## Enum values
- `ALL_CAPS` with underscores
  - e.g. `CLOCKWISE`, `COUNTER_CLOCKWISE`, `LOOP`, `ONE_SHOT`, `SINE`

## Functions / Methods
- Public methods: `PascalCase`
  - e.g. `Initialize()`, `Update()`, `Render()`, `SetConfig()`, `GetConfig()`, `AddRenderer()`, `Play()`, `Pause()`, `Stop()`, `Emit()`
- Private methods: `camelCase`
  - e.g. `setupShaders()`, `setupQuadGeometry()`, `setupBuffers()`, `updateBuffers()`, `emitParticlesAtHead()`

## Event Callbacks
- `PascalCase` with `On` prefix
  - e.g. `OnProgress`, `OnLoopCompleted`, `OnParticleSpawned`, `OnResize`, `OnKey`

## Variables

### Member variables
- `camelCase` with `m` prefix
  - e.g. `mConfig`, `mTime`, `mAnimation`, `mRenderers`, `mShaderProgram`, `mIsPlaying`, `mSpeed`, `mProgress`, `mMaxParticles`, `mGlobalSize`

### Local variables & parameters
- `camelCase` (no prefix)
  - e.g. `deltaTime`, `displayW`, `success`, `infoLog`, `progress`, `viewportWidth`

### Global variables
- `camelCase` with `g` prefix
  - e.g. `gEffect`, `gColorThemeIndex`, `gInstance`

## Constants
- `ALL_CAPS` with underscores
  - e.g. `VERTEX_SHADER_SRC`, `FRAGMENT_SHADER_SRC`, `PARTICLE_VERTEX_SHADER_SRC`

## Header guards / Macros
- `ALL_CAPS` with `_` prefix and `_` suffix
  - e.g. `_EDGE_LIGHTING_EFFECT_H_`, `_EDGE_LIGHTING_CONFIG_H_`

## Class layout / Access specifiers
- Use separate `private:` (or `public:`) labels for methods vs member variables
  - First `private:` - methods
  - Second `private:` - member variables
  - *Correct:*
    ```
    class Foo : public BaseRenderer
    {
    public:
        // public methods...

    private:
        // private methods...

    private:
        // member variables...
    };
    ```
  - *Incorrect:*
    ```
    class Foo : public BaseRenderer
    {
    public:
        // public methods...

    private:
        // everything mixed
    };
    ```

## Comments / Text
- Do not use the em-dash character (Unicode U+2014). Use a plain hyphen `-`
  instead, in comments, doc comments, log strings, and Markdown. The whole
  repo - including this file - is kept free of the glyph, so a recursive
  grep for U+2014 over the source can enforce the rule with no false positives.
  - *Correct:* `// premultiplied output - reserved for a later pass`
  - *Incorrect:* the same line with a U+2014 dash where the hyphen is.
- Keep shader sources (`.vert`/`.frag`) ASCII-only so every GLSL compiler
  accepts the CMake-injected text (see `lib/shaders/neon-tuning.h`).

## Braces / Formatting
- Always use braces for control flow statements, even single-line bodies
  - *Correct:*
    ```
    if (count <= 0)
    {
        return glm::vec3(1.0f);
    }
    ```
  - *Incorrect:*
    ```
    if (count <= 0)
        return glm::vec3(1.0f);
    ```
- Use braces for `case` bodies inside `switch`:
  - *Correct:*
    ```
    switch (i)
    {
    case 0:
    {
        r = v; g = t; b = p;
        break;
    }
    case 1:
    {
        r = q; g = v; b = p;
        break;
    }
    }
    ```
  - *Incorrect:*
    ```
    switch (i)
    {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    }
    ```
