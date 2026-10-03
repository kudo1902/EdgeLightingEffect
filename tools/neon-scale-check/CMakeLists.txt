# neon-scale-check - the harness behind docs/neon-resolution-scale-comparison.html.
#
# Two ways in:
#
#   In-tree (the usual one): configure the repo with
#       cmake -S . -B build -G Ninja -DEDGE_LIGHTING_BUILD_TOOLS=ON
#   and this links the library being built.
#
#   Standalone, against ANOTHER checkout's library - how the page measures a
#   commit from before this tool existed:
#       cmake -S tools/neon-scale-check -B build/neon-scale-check-other -G Ninja -DEL_ROOT=/path/to/checkout
#   EL_ROOT must already be built in EL_ROOT/build (libedge-lighting.a and
#   libglad.a). The tool only uses the public headers, which it takes from
#   EL_ROOT so they match that library.
#
# See README.md.

cmake_minimum_required(VERSION 3.15)

if(NOT TARGET edge-lighting)
    project(neon-scale-check CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()

    set(EL_ROOT "" CACHE PATH "Checkout whose built library to link (standalone mode)")
    set(EL_BUILD "${EL_ROOT}/build" CACHE PATH "That checkout's build directory")
    if(NOT EL_ROOT OR NOT EXISTS "${EL_BUILD}/lib/libedge-lighting.a")
        message(FATAL_ERROR "neon-scale-check standalone: set EL_ROOT to a checkout built in "
                            "EL_ROOT/build (expected ${EL_BUILD}/lib/libedge-lighting.a)")
    endif()

    if(CMAKE_OSX_ARCHITECTURES)
        set(_el_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_el_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    add_library(glad STATIC IMPORTED)
    set_target_properties(glad PROPERTIES IMPORTED_LOCATION "${EL_BUILD}/libglad.a")
    add_library(edge-lighting STATIC IMPORTED)
    set_target_properties(edge-lighting PROPERTIES
        IMPORTED_LOCATION "${EL_BUILD}/lib/libedge-lighting.a"
        INTERFACE_INCLUDE_DIRECTORIES "${EL_ROOT}/lib/include;${EL_BUILD}/lib/generated;${EL_ROOT}/external/include"
        INTERFACE_LINK_LIBRARIES glad
        INTERFACE_COMPILE_DEFINITIONS PLATFORM_MACOS
    )
    add_library(glfw SHARED IMPORTED)
    set_target_properties(glfw PROPERTIES
        IMPORTED_LOCATION "${EL_ROOT}/external/lib/${_el_arch}/libglfw.3.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${EL_ROOT}/external/include"
    )
endif()

add_executable(neon-scale-check
    src/main.cpp
    src/harness.cpp
    src/partition.cpp
    ../common/pass-recorder.cpp
)

# `partition` watches the renderer's draws through the pass recorder shared
# with neon-guide-figures. It needs the generated shaders.h (the recorder
# replays draws through the library's own neon.vert), which is why the
# standalone target above carries EL_BUILD/lib/generated.
target_include_directories(neon-scale-check PRIVATE ../common)

# Where `check` finds the page's committed images by default: this checkout's
# docs, whichever library the tool is linked against.
target_compile_definitions(neon-scale-check PRIVATE
    DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../docs"
)

target_link_libraries(neon-scale-check PRIVATE
    edge-lighting
    glfw
)
