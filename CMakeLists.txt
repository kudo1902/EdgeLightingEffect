cmake_minimum_required(VERSION 3.15)
project(EdgeLightingEffect C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Default build type
# ---------------------------------------------------------------------------
# Without this, a bare `cmake -S . -B build` - the command in README.md and
# CLAUDE.md, and so the one everybody actually runs - leaves CMAKE_BUILD_TYPE
# empty, and an empty build type contributes NO optimisation flag at all. The
# compile line for the library was literally:
#
#     clang++ -DPLATFORM_MACOS -I... -g -std=gnu++17 -arch arm64 -fPIC -c ...
#
# so every host linking libedge-lighting.a got an unoptimised build of the LUT
# bakes, the colour conversions, the contour tracer and the whole C ABI.
# Measured against -O3: the per-frame config path ran 1.5x slower and the
# static library was 11.6 MB instead of 844 KB.
#
# Only a default. An explicit -DCMAKE_BUILD_TYPE=Debug (or a multi-config
# generator, which drives the choice per-build and leaves this variable empty
# by design) is left exactly as it was found.
get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _isMultiConfig AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    message(STATUS "EdgeLightingEffect: no CMAKE_BUILD_TYPE set - defaulting to Release")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

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

# ---------------------------------------------------------------------------
# macOS GLFW architecture selection
# ---------------------------------------------------------------------------
# Prebuilt GLFW dylibs live under external/lib/{arm64,x86_64,universal}. Pick
# the slice that matches the target arch so the binary links and runs on both
# Apple Silicon and Intel:
#   - honour an explicit CMAKE_OSX_ARCHITECTURES (a multi-arch value like
#     "arm64;x86_64" selects the universal slice)
#   - otherwise fall back to the host processor (CMAKE_HOST_SYSTEM_PROCESSOR
#     reports arm64 on Apple Silicon, x86_64 on Intel).
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
        if(_osx_arch_count GREATER 1)
            set(GLFW_ARCH "universal")
        else()
            set(GLFW_ARCH "${CMAKE_OSX_ARCHITECTURES}")
        endif()
    else()
        set(GLFW_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    if(NOT GLFW_ARCH MATCHES "^(arm64|x86_64|universal)$")
        message(FATAL_ERROR "Unsupported macOS arch for GLFW: '${GLFW_ARCH}' "
                            "(expected arm64, x86_64, or a universal build)")
    endif()

    set(EXTERNAL_LIB_DIR ${EXTERNAL_DIR}/lib/${GLFW_ARCH})
    message(STATUS "EdgeLightingEffect: GLFW arch = ${GLFW_ARCH}")
else()
    set(EXTERNAL_LIB_DIR ${EXTERNAL_DIR}/lib/universal)
endif()

# Include directories
include_directories(${EXTERNAL_INC_DIR})

# GLAD library
add_library(glad STATIC ${EXTERNAL_SRC_DIR}/glad.c)
target_include_directories(glad PUBLIC ${EXTERNAL_INC_DIR})
target_link_libraries(glad PRIVATE ${CMAKE_DL_LIBS})

# GLFW library
add_library(glfw SHARED IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${EXTERNAL_LIB_DIR}/libglfw.3.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${EXTERNAL_INC_DIR}"
)

add_subdirectory(lib)
add_subdirectory(demo)
add_subdirectory(demo-capi)
