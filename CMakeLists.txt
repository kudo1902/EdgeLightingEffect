cmake_minimum_required(VERSION 3.15)
project(EdgeLightingEffect C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build everything position-independent so the static lib + glad can be linked
# into the edge-lighting-c shared library (required on Linux; harmless on macOS).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

message(STATUS "EdgeLightingEffect: ${PLATFORM_DEFINE} (${GLSL_VERSION})")
add_compile_definitions(${PLATFORM_DEFINE})

# Paths
set(EXTERNAL_DIR ${CMAKE_SOURCE_DIR}/external)
set(EXTERNAL_INC_DIR ${EXTERNAL_DIR}/include)
set(EXTERNAL_SRC_DIR ${EXTERNAL_DIR}/src)
set(EXTERNAL_LIB_DIR ${EXTERNAL_DIR}/lib/universal)

# Include directories
include_directories(${EXTERNAL_INC_DIR})

# GLAD library
add_library(glad STATIC ${EXTERNAL_SRC_DIR}/glad.c)
target_include_directories(glad PUBLIC ${EXTERNAL_INC_DIR})
target_link_libraries(glad PRIVATE ${CMAKE_DL_LIBS})

# GLFW library
add_library(glfw SHARED IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${EXTERNAL_DIR}/lib/arm64/libglfw.3.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${EXTERNAL_INC_DIR}"
)

add_subdirectory(lib)
add_subdirectory(demo)
