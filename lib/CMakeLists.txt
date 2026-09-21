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
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/spotlight-tuning.h"
)

# ---------------------------------------------------------------------------
# Shader embedding: read, strip comments, substitute into shaders.h.in
# ---------------------------------------------------------------------------
# The sources under shaders/ are heavily commented on purpose - neon.frag is
# 1645 lines and the docs send people to read it - but every one of those bytes
# would otherwise be embedded verbatim in shaders.h, shipped in .rodata of both
# libraries and both demos, and handed to the driver's GLSL compiler at
# Initialize. Measured before this strip: the four neon shaders plus the two
# injections of neon-tuning.h came to 188,379 bytes of string data carrying
# 26.7 KB of actual GLSL - 85% comment text. neon-renderer.cpp.o alone had a
# 190,693-byte __cstring section, 70% of the whole library's string data.
#
# So the ON-DISK files keep every comment and the EMBEDDED copies get none.
option(EDGE_LIGHTING_STRIP_SHADER_COMMENTS
       "Strip // comments from the embedded shader sources (not from the files on disk)." ON)

# Read a GLSL source (or an injected tuning header) and strip its comments.
#
# LINE COUNT IS PRESERVED - a stripped line becomes empty, it does not
# disappear. That is the whole reason blank lines are not also collapsed, and
# it costs 3,065 bytes across all eighteen files against a 179 KB saving:
# every GLSL compile error the driver reports keeps pointing at the same line
# it pointed at before this strip, so a shader error log still reads against
# the commented file on disk. Collapse the blanks as well and every line
# number in every error message shifts by an amount that depends on where in
# the file the error is.
#
# Only `//` comments are stripped. Nothing here uses `/* */` - checked, all
# eighteen files - and a block comment that did appear would simply survive
# into the embedded copy, which is valid GLSL and costs bytes but breaks
# nothing. That is the safe direction to be wrong in; a multi-line handler
# that got its state wrong would eat code instead. The negated class `[^\n]*`
# is what confines each match to its own line, since CMake's `.` does not.
#
# Safe on GLSL specifically because the language has no string literals and no
# line continuations, so `//` can never be anything but the start of a comment
# (verified: no backslash appears in any shader source).
function(read_shader_source PATH OUT_VAR)
    file(READ "${PATH}" _src)
    if(EDGE_LIGHTING_STRIP_SHADER_COMMENTS)
        string(REGEX REPLACE "//[^\n]*" "" _src "${_src}")
        # Drop the whitespace a stripped trailing comment leaves behind, plus
        # any that was already there. The newline itself is what stays.
        string(REGEX REPLACE "[ \t]+\n" "\n" _src "${_src}")
        string(REGEX REPLACE "[ \t]+$" "" _src "${_src}")
    endif()
    set(${OUT_VAR} "${_src}" PARENT_SCOPE)
endfunction()

read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert" WIREFRAME_VERT_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag" WIREFRAME_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert" NEON_VERT_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag" NEON_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-emission.frag" NEON_EMISSION_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag" NEON_BLIT_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/black-rect.frag" BLACK_RECT_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-lut-debug.frag" NEON_LUT_DEBUG_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-stop-marker.frag" NEON_STOP_MARKER_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/droplets.frag" DROPLETS_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.vert" LENS_FLARE_VERT_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/lens-flare.frag" LENS_FLARE_FRAG_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.vert" SPOTLIGHT_VERT_CONTENT)
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight.frag" SPOTLIGHT_FRAG_CONTENT)

# The four tuning headers go through the same strip, and they are where most of
# it is won: neon-tuning.h is 32,393 bytes carrying 1,199 bytes of #define, and
# it is injected TWICE (neon.frag and neon-emission.frag). Stripping is also
# what keeps the shader body's line numbers where they were - the header sits
# above it, so its line count is the offset every line below inherits.
#
# The C++ side is untouched: the renderers #include these headers as normal
# commented C++, and only the copy embedded in shaders.h is stripped.

# Shared neon tuning #defines, injected into the single-pass neon shaders via
# @NEON_TUNING@ in shaders.h.in (also #included by the C++ renderers).
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h" NEON_TUNING)

# Shared droplet band #defines, injected into droplets.frag via
# @DROPLETS_TUNING@ in shaders.h.in (also #included by the C++ renderer).
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/droplets-tuning.h" DROPLETS_TUNING)

# Shared ghost-term #defines, injected into lens-flare.frag via
# @LENS_FLARE_TUNING@ in shaders.h.in (also #included by the C++ renderer,
# which derives the ghost support bounds from them).
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/lens-flare-tuning.h" LENS_FLARE_TUNING)

# Shared spotlight falloff #defines, injected into spotlight.frag via
# @SPOTLIGHT_TUNING@ in shaders.h.in (also #included by the C++ renderer,
# which solves each lamp's support from the same falloff).
read_shader_source("${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/spotlight-tuning.h" SPOTLIGHT_TUNING)

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
