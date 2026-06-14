add_executable(edge-lighting-demo
    src/main.cpp
)

target_compile_definitions(edge-lighting-demo PRIVATE
    RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../res"
)

target_link_libraries(edge-lighting-demo PRIVATE
    edge-lighting
    glfw
)
