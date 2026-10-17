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

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-common.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-pieces.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-sdf.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-mask.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-grade.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-ring.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-light.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gather.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-emission.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow-cover.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow-cover-fill.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-field.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-assemble.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/blit.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/black-rect.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-lut-debug.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-stop-marker.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug-buffer-view.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/droplets.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-blit.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-dither.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/spotlight-tuning.h"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert" WIREFRAME_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag" WIREFRAME_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert" NEON_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag" NEON_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gather.frag" NEON_GATHER_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-emission.frag" NEON_EMISSION_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow-cover.frag" NEON_GLOW_COVER_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow-cover-fill.frag" NEON_GLOW_COVER_FILL_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag" NEON_BLIT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-field.frag" NEON_FIELD_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-assemble.frag" NEON_ASSEMBLE_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/blit.frag" BLIT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/black-rect.frag" BLACK_RECT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-lut-debug.frag" NEON_LUT_DEBUG_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-stop-marker.frag" NEON_STOP_MARKER_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debug-buffer-view.frag" DEBUG_BUFFER_VIEW_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/droplets.frag" DROPLETS_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.vert" LENS_FLARE_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.frag" LENS_FLARE_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.vert" SPOTLIGHT_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.frag" SPOTLIGHT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-blit.frag" SPOTLIGHT_BLIT_FRAG_CONTENT)

# Shared neon tuning #defines, injected into neon.frag, neon-gather.frag,
# neon-emission.frag, neon-glow-cover.frag, neon-glow-cover-fill.frag, neon-blit.frag and neon-field.frag via @NEON_TUNING@
# in shaders.h.in (also #included by the C++ renderers).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h" NEON_TUNING)

# The perimeter gather and what it reads, injected into neon.frag and
# neon-gather.frag via @NEON_COMMON@ in shaders.h.in, after @NEON_TUNING@ - the
# one GLSL chunk two neon programs share, so the loop has a single copy. Not a
# shader on its own: it has no main().
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-common.glsl" NEON_COMMON)

# The corner arcs' development onto a straight line and the glow coverage
# table's layout, injected into neon.frag, neon-glow-cover.frag and
# neon-glow-cover-fill.frag via @NEON_PIECES@ in shaders.h.in, after
# @NEON_TUNING@ (and in neon.frag after @NEON_COMMON@): the read, the bake and
# the segment table's fill have to agree on both to the texel. Not a shader on
# its own either.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-pieces.glsl" NEON_PIECES)

# The rounded box's signed distance, its antialiasing width and the cutoff
# band's boundaries, injected via @NEON_SDF@ into neon.frag, neon-blit.frag,
# neon-field.frag and black-rect.frag - the four programs that draw the glow's
# edge and have to agree on it to the pixel - and
# the tone map, via @NEON_GRADE@ into neon.frag and neon-field.frag, after
# @NEON_TUNING@. One copy each; neither is a shader on its own.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-sdf.glsl" NEON_SDF)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-grade.glsl" NEON_GRADE)

# The glow's edge mask - the one-sided cut and the cutoffs, as coverage after
# the tone map - via @NEON_MASK@ into the three programs that apply it
# (neon.frag's edge ring, neon-blit.frag, neon-field.frag's ring composite),
# after @NEON_SDF@, whose distances it reads. One copy; not a shader on its own.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-mask.glsl" NEON_MASK)

# The edge ring's packing - the strip lookup of RingFieldLayout - via
# @NEON_RING@ into neon-field.frag (the ring field's composite and the ring
# atlas's copy) and the neon's light, after @NEON_TUNING@. And the neon's
# light itself, read from its two shared parts (the glow and the ring atlas),
# via @NEON_LIGHT@ after @NEON_MASK@ and @NEON_RING@ - also exported whole as
# ShaderSource::NEON_LIGHT_GLSL for a host. Neither is a shader on its own.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-ring.glsl" NEON_RING)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-light.glsl" NEON_LIGHT)

# Shared droplet band #defines, injected into droplets.frag via
# @DROPLETS_TUNING@ in shaders.h.in (also #included by the C++ renderer).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h" DROPLETS_TUNING)

# Shared ghost-term #defines, injected into lens-flare.frag via
# @LENS_FLARE_TUNING@ in shaders.h.in (also #included by the C++ renderer,
# which derives the ghost support bounds from them).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h" LENS_FLARE_TUNING)

# Shared spotlight falloff #defines, injected into spotlight.frag via
# @SPOTLIGHT_TUNING@ in shaders.h.in (also #included by the C++ renderer,
# which solves each lamp's support from the same falloff).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/spotlight-tuning.h" SPOTLIGHT_TUNING)

# The spotlight's output dither, injected via @SPOTLIGHT_DITHER@ into
# spotlight.frag and spotlight-blit.frag, after @SPOTLIGHT_TUNING@ - the two
# programs that write the layer to the caller's framebuffer (at scale 1.0 and
# below it) and have to draw the same noise. Not a shader on its own.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-dither.glsl" SPOTLIGHT_DITHER)

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
    src/core/buffer-registry.cpp
    src/core/read-list.cpp
    src/core/resource-context.cpp
    src/core/edge-lighting.cpp
    src/renderer/base-renderer.cpp
    src/renderer/neon-renderer.cpp
    src/renderer/debug-renderer.cpp
    src/renderer/droplets-renderer.cpp
    src/renderer/lens-flare-renderer.cpp
    src/renderer/spotlight-renderer.cpp
    src/util/capture-util.cpp
    src/util/contour-tracer.cpp
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
