# ---------------------------------------------------------------------------
# Make this file buildable standalone or as a subdirectory of the root project.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.15)
project(edge-lighting C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Platform / GL profile auto-detection
# ---------------------------------------------------------------------------
# macOS  → desktop OpenGL Core 3.3 (GLAD loader)
# Windows → OpenGL ES 3.0 (GLAD-GLES2 loader, typically against ANGLE)
# Linux  → OpenGL ES 3.0 (native <GLES3/gl3.h>, link-time symbols)
#
# Each platform contributes:
#  - one compile-def consumed by gl-header.h and main.cpp
#  - the GLSL version line injected into shaders.h.in via @GLSL_VERSION@
if(APPLE)
    set(PLATFORM_DEFINE "PLATFORM_MACOS")
    set(GLSL_VERSION "#version 330 core")
elseif(WIN32)
    set(PLATFORM_DEFINE "PLATFORM_WINDOWS")
    set(GLSL_VERSION "#version 300 es")
elseif(UNIX)
    set(PLATFORM_DEFINE "PLATFORM_LINUX")
    set(GLSL_VERSION "#version 300 es")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

add_compile_definitions(${PLATFORM_DEFINE})
message(STATUS "EdgeLightingEffect: ${PLATFORM_DEFINE} (${GLSL_VERSION})")

# Every text shaders.h.in injects - each shader, each shared .glsl chunk, the
# four tuning headers - as VARIABLE file pairs. Each is read here with its //
# comments stripped, newlines kept (so a compile error's line number still
# points into the file), handed to the template as @VARIABLE@, and made a
# configure dependency, so editing it re-runs the configure. shaders.h.in says
# which text goes into which program. A new shader is one line here and its
# placeholder there.
#
# Stripping is plain text: none of these files has a string literal or a /* */
# comment, so every // starts a comment. Keep it that way.
set(EDGE_LIGHTING_SHADER_TEXTS
    WIREFRAME_VERT_CONTENT             shaders/wireframe.vert
    WIREFRAME_FRAG_CONTENT             shaders/wireframe.frag
    NEON_VERT_CONTENT                  shaders/neon.vert
    NEON_FRAG_CONTENT                  shaders/neon.frag
    NEON_FIELD_BAKE_FRAG_CONTENT       shaders/neon-field-bake.frag
    NEON_GATHER_FRAG_CONTENT           shaders/neon-gather.frag
    NEON_EMISSION_FRAG_CONTENT         shaders/neon-emission.frag
    NEON_GLOW_COVER_FRAG_CONTENT       shaders/neon-glow-cover.frag
    NEON_GLOW_COVER_FILL_FRAG_CONTENT  shaders/neon-glow-cover-fill.frag
    NEON_BLIT_FRAG_CONTENT             shaders/neon-blit.frag
    NEON_FIELD_FRAG_CONTENT            shaders/neon-field.frag
    NEON_RING_FIELD_FRAG_CONTENT       shaders/neon-ring-field.frag
    BLACK_RECT_FRAG_CONTENT            shaders/black-rect.frag
    NEON_LUT_DEBUG_FRAG_CONTENT        shaders/neon-lut-debug.frag
    NEON_STOP_MARKER_FRAG_CONTENT      shaders/neon-stop-marker.frag
    SHARED_BUFFER_DEBUG_FRAG_CONTENT   shaders/shared-buffer-debug.frag
    DROPLETS_FRAG_CONTENT              shaders/droplets.frag
    LENS_FLARE_VERT_CONTENT            shaders/lens-flare.vert
    LENS_FLARE_FRAG_CONTENT            shaders/lens-flare.frag
    LENS_FLARE_BLIT_FRAG_CONTENT       shaders/lens-flare-blit.frag
    SPOTLIGHT_VERT_CONTENT             shaders/spotlight.vert
    SPOTLIGHT_FRAG_CONTENT             shaders/spotlight.frag
    SPOTLIGHT_BLIT_FRAG_CONTENT        shaders/spotlight-blit.frag
    NEON_TUNING                        include/renderer/neon-tuning.h
    NEON_COMMON                        shaders/neon-common.glsl
    NEON_PIECES                        shaders/neon-pieces.glsl
    NEON_SDF                           shaders/neon-sdf.glsl
    NEON_GRADE                         shaders/neon-grade.glsl
    NEON_SHADE                         shaders/neon-shade.glsl
    NEON_FIELD_COMMON                  shaders/neon-field-common.glsl
    DROPLETS_TUNING                    include/renderer/droplets-tuning.h
    LENS_FLARE_TUNING                  include/renderer/lens-flare-tuning.h
    LENS_FLARE_DITHER                  shaders/lens-flare-dither.glsl
    SPOTLIGHT_TUNING                   include/renderer/spotlight-tuning.h
    SPOTLIGHT_DITHER                   shaders/spotlight-dither.glsl
)
set(_shaderTexts ${EDGE_LIGHTING_SHADER_TEXTS})
while(_shaderTexts)
    list(POP_FRONT _shaderTexts _shaderVar _shaderFile)
    set(_shaderPath "${CMAKE_CURRENT_SOURCE_DIR}/${_shaderFile}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_shaderPath}")
    file(READ "${_shaderPath}" _shaderText)
    string(REGEX REPLACE "[ \t]*//[^\n]*" "" _shaderText "${_shaderText}")
    set(${_shaderVar} "${_shaderText}")
endwhile()

set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.h.in"
    "${SHADER_GEN_DIR}/shaders.h"
    @ONLY
)

add_library(edge-lighting STATIC
    src/animation/animation-manager.cpp
    src/animation/easing-function.cpp
    src/animation/field-bound-animation.cpp
    src/core/edge-lighting.cpp
    src/core/config-validation.cpp
    src/core/adaptive-format-target.cpp
    src/core/format-availability.cpp
    src/core/shared-buffer-registry.cpp
    src/core/shared-buffer.cpp
    src/renderer/base-renderer.cpp
    src/renderer/neon-renderer.cpp
    src/renderer/debug-renderer.cpp
    src/renderer/droplets-renderer.cpp
    src/renderer/lens-flare-renderer.cpp
    src/renderer/spotlight-renderer.cpp
    src/util/capture-util.cpp
    src/util/stb-image.cpp
)

target_include_directories(edge-lighting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${SHADER_GEN_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(edge-lighting PRIVATE
    ${EXTERNAL_DIR}/include
)

target_link_libraries(edge-lighting PUBLIC glad)

# ---------------------------------------------------------------------------
# C ABI shared library for FFI / C# interop.
# ---------------------------------------------------------------------------
# Wraps the static edge-lighting lib in a flat extern "C" surface and links it
# whole into a shared object (libedge-lighting-c.{dylib,so} / edge-lighting-c.dll)
# that .NET P/Invoke (or any FFI) can load. Only the el_* symbols are exported;
# everything else is hidden so the C++ ABI never leaks.
add_library(edge-lighting-c SHARED
    capi/el-effect.cpp
    capi/el-animation.cpp
    capi/el-modulator.cpp
    capi/el-deprecated.cpp
)

target_include_directories(edge-lighting-c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(edge-lighting-c PRIVATE edge-lighting)

set_target_properties(edge-lighting-c PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME edge-lighting-c
)
