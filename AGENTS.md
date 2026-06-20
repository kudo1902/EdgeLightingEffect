# EdgeLightingEffect — Naming Conventions

## Files
- Source/header files: `kebab-case`
  - e.g. `edge-lighting.h`, `segment-renderer.cpp`, `particle-renderer.h`

## Namespaces
- `PascalCase`
  - e.g. `EdgeLighting`, `EdgeLightingDemo`, `GeometryUtils`

## Classes / Structs / Enums
- `PascalCase`
- Must have `typedef` alias (same name, C++ style)
  - `typedef struct Config { ... } Config;`
  - `typedef enum class ColorMode { ... } ColorMode;`
- Examples: `EdgeLightingEffect`, `Config`, `ColorMode`, `BaseRenderer`, `SegmentRenderer`, `ParticleSystem`, `Particle`, `Animation`

## Enum values
- `ALL_CAPS` with underscores
  - e.g. `STATIC`, `GRADIENT`, `AMBIENT_RAINBOW`

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
