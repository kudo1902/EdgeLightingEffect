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
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-emission.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/black-rect.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-lut-debug.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-stop-marker.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/droplets.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert" WIREFRAME_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag" WIREFRAME_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert" NEON_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag" NEON_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-emission.frag" NEON_EMISSION_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag" NEON_BLIT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/black-rect.frag" BLACK_RECT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-lut-debug.frag" NEON_LUT_DEBUG_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-stop-marker.frag" NEON_STOP_MARKER_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/droplets.frag" DROPLETS_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.vert" LENS_FLARE_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.frag" LENS_FLARE_FRAG_CONTENT)

# Shared neon tuning #defines, injected into the single-pass neon shaders via
# @NEON_TUNING@ in shaders.h.in (also #included by the C++ renderers).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h" NEON_TUNING)

# Shared droplet band #defines, injected into droplets.frag via
# @DROPLETS_TUNING@ in shaders.h.in (also #included by the C++ renderer).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h" DROPLETS_TUNING)

# Shared ghost-term #defines, injected into lens-flare.frag via
# @LENS_FLARE_TUNING@ in shaders.h.in (also #included by the C++ renderer,
# which derives the ghost support bounds from them).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h" LENS_FLARE_TUNING)

set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.h.in"
    "${SHADER_GEN_DIR}/shaders.h"
    @ONLY
)

add_library(edge-lighting STATIC
    src/animation/animation-manager.cpp
    src/animation/easing-function.cpp
    src/animation/field-access.cpp
    src/animation/field-bound-animation.cpp
    src/core/edge-lighting.cpp
    src/renderer/neon-renderer.cpp
    src/renderer/debug-renderer.cpp
    src/renderer/droplets-renderer.cpp
    src/renderer/lens-flare-renderer.cpp
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
# Narrowing guard: double -> float must be deliberate.
# ---------------------------------------------------------------------------
# The renderer interface carries `time` as a double specifically so each
# renderer can reduce it at full precision before it becomes a float uniform
# (TimeUtils::WrapHueTime, review-findings I33b). That contract is invisible at
# a call site: a private pass method declared with a float parameter silently
# undoes it, and the frame it breaks is one at days of uptime that nobody runs.
# NeonRenderer::renderNeonPass did exactly that and shipped - see I35.
#
# One flag catches the whole class, and after I35 it fires on zero lines of
# this library. Every conversion that IS intended already goes through an
# explicit static_cast, which the warning does not touch.
#
# The -Wno is not a workaround, it is the scope. In Clang
# -Wimplicit-float-conversion is a GROUP, and it pulls in the INT -> float
# variant with it - six hits in contour-tracer.cpp, all of them small integer
# pixel coordinates converted to float, where the "precision loss" the warning
# names cannot happen below 2^24 and none of these come close. That is a
# separate judgement about a separate file, and silently rewriting it under a
# double -> float fix would be the wrong place to make it. Narrowing the group
# to the half that has a live defect behind it keeps the build at zero warnings,
# which is the only state in which a new one gets noticed.
#
# PRIVATE and scoped to the two library targets deliberately. It is a rule about
# THIS code, not about the code that links it, and it must not reach the two
# third-party sources compiled elsewhere in the tree (glad has its own target,
# ImGui is compiled into the demos) - neither is ours to keep clean.
#
# Guarded on the compiler because the spelling is not portable: GCC has no
# -Wimplicit-float-conversion at all and folds this into -Wfloat-conversion,
# which in Clang means float -> int instead and would NOT catch I35. Only the
# Clang/AppleClang name is asserted here because that is the only toolchain the
# tree currently builds on; a GCC branch belongs with the rest of the
# PLATFORM_LINUX scaffolding when that is finished.
#
# Applied to edge-lighting-c at the bottom of this file, where that target
# exists.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(EL_NARROWING_FLAGS -Wimplicit-float-conversion -Wno-implicit-int-float-conversion)
    target_compile_options(edge-lighting PRIVATE ${EL_NARROWING_FLAGS})
endif()

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

# The ABI sources get the same narrowing guard as the static library - see the
# block above it for why. Empty on a non-Clang toolchain, which is a no-op.
target_compile_options(edge-lighting-c PRIVATE ${EL_NARROWING_FLAGS})
