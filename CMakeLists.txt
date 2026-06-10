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
add_library(glfw SHARED IMPORTED)
set_target_properties(glfw PROPERTIES
    IMPORTED_LOCATION "${EXTERNAL_DIR}/lib/arm64/libglfw.3.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${EXTERNAL_INC_DIR}"
)

add_subdirectory(lib)
add_subdirectory(demo)
