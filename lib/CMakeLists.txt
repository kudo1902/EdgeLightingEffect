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
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gradient.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blur.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-optimized.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert" WIREFRAME_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag" WIREFRAME_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert" NEON_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag" NEON_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gradient.frag" NEON_GRADIENT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blur.frag" NEON_BLUR_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow.frag" NEON_GLOW_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-optimized.frag" NEON_OPTIMIZED_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-blit.frag" NEON_BLIT_FRAG_CONTENT)

set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.h.in"
    "${SHADER_GEN_DIR}/shaders.h"
    @ONLY
)

add_library(edge-lighting STATIC
    src/animation/easing-function.cpp
    src/core/edge-lighting.cpp
    src/renderer/wireframe-renderer.cpp
    src/renderer/neon-renderer.cpp
    src/renderer/neon-multi-pass-renderer.cpp
    src/renderer/neon-optimized-renderer.cpp
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
