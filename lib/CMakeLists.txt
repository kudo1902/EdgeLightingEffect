set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gradient.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow.frag"
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.vert" STROKE_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/stroke.frag" STROKE_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.vert" WIREFRAME_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/wireframe.frag" WIREFRAME_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.vert" PARTICLE_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.frag" PARTICLE_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.vert" NEON_VERT_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon.frag" NEON_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-gradient.frag" NEON_GRADIENT_FRAG_CONTENT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/neon-glow.frag" NEON_GLOW_FRAG_CONTENT)

set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.h.in"
    "${SHADER_GEN_DIR}/shaders.h"
    @ONLY
)

add_library(edge-lighting STATIC
    src/core/edge-lighting.cpp
    src/animation/animation.cpp
    src/renderer/stroke-renderer.cpp
    src/renderer/wireframe-renderer.cpp
    src/renderer/particles.cpp
    src/renderer/particle-renderer.cpp
    src/renderer/neon-renderer.cpp
    src/renderer/neon-multi-pass-renderer.cpp
    src/util/contour-tracer.cpp
    src/util/stb-image.cpp
)

target_include_directories(edge-lighting PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${SHADER_GEN_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(edge-lighting PRIVATE
    ${CMAKE_SOURCE_DIR}/external/include
)

target_link_libraries(edge-lighting PUBLIC glad)
