add_executable(edge-lighting-demo
    src/main.cpp
    src/debug-ui.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_demo.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_impl_glfw.cpp
    ${CMAKE_SOURCE_DIR}/external/include/imgui/imgui_impl_opengl3.cpp
)

target_compile_definitions(edge-lighting-demo PRIVATE
    RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../res"
)

target_include_directories(edge-lighting-demo PRIVATE
    ${CMAKE_SOURCE_DIR}/external/include/imgui
)

target_link_libraries(edge-lighting-demo PRIVATE
    edge-lighting
    glfw
)
