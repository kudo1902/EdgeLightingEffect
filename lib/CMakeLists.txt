add_library(edge-lighting STATIC
    src/core/edge-lighting.cpp
    src/animation/animation.cpp
    src/renderer/segment-renderer.cpp
    src/renderer/particle-renderer.cpp
    src/renderer/particles.cpp
)

target_include_directories(edge-lighting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(edge-lighting PUBLIC glad)
