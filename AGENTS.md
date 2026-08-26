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
- Protected methods: `PascalCase` - they follow the **public** rule, not the
  private one. A protected virtual is API for subclasses to override or call,
  not an implementation detail of this class.
  - e.g. `RestoreBaseline()`, `ApplyAt()`, `CaptureBaseline()`, `OnDurationChanged()`

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

## C API (`lib/capi/`)

- Files: `kebab-case` with `-capi` suffix
  - e.g. `edge-lighting-capi.h`, `edge-lighting-capi.cpp`
- Functions: `snake_case` with `el_` prefix
  - e.g. `el_config_create`, `el_effect_render`, `el_animation_play`
- Enum types: `snake_case` with `_e` suffix
  - e.g. `el_result_e`, `el_winding_e`, `el_animation_state_e`
- Handle types: `snake_case` with `_handle_t` suffix
  - e.g. `el_config_handle_t`, `el_effect_handle_t`, `el_animation_handle_t`, `el_modulator_handle_t`
- Struct / typedef types: `snake_case` with `_t` suffix
  - e.g. `el_bool_t`, `el_gl_get_proc_address_t`
- Enum values / macros: `ALL_CAPS` with underscores (unchanged)
  - e.g. `EL_OK`, `EL_ERR_NULL_ARG`, `EL_API`, `EL_ABI_VERSION`
- Function-pointer typedef: `snake_case` with `_t` suffix
  - e.g. `el_gl_get_proc_address_t`

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
