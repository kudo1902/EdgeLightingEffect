cmake_minimum_required(VERSION 3.15)
project(EdgeLightingEffect C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
add_library(glfw STATIC IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${EXTERNAL_LIB_DIR}/libglfw3.a"
    INTERFACE_INCLUDE_DIRECTORIES "${EXTERNAL_INC_DIR}"
)

# On macOS, GLFW needs system frameworks.
if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(APPKIT_LIBRARY AppKit)
    set_target_properties(glfw PROPERTIES
        INTERFACE_LINK_LIBRARIES "${COCOA_LIBRARY};${IOKIT_LIBRARY};${COREVIDEO_LIBRARY};${QUARTZCORE_LIBRARY};${APPKIT_LIBRARY};-pthread"
    )
endif()

add_subdirectory(lib)
add_subdirectory(demo)
