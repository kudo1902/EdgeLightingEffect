add_executable(edge-lighting-capi-demo
    src/main.cpp
    src/debug-ui.cpp
    src/border-color-picker.cpp
    src/stb-image-impl.c
    ${EXTERNAL_INC_DIR}/imgui/imgui.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_draw.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_tables.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_widgets.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_demo.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_impl_glfw.cpp
    ${EXTERNAL_INC_DIR}/imgui/imgui_impl_opengl3.cpp
)

target_compile_definitions(edge-lighting-capi-demo PRIVATE
    RES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../res"
)

# Only the flat C ABI header is on the include path. The include tree under
# lib/include is deliberately absent so any accidental "#include "core/..."" or
# "renderer/..." fails at compile time - the guarantee that this demo drives
# the effect exclusively through el_*.
target_include_directories(edge-lighting-capi-demo PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/capi
    ${EXTERNAL_INC_DIR}
    ${EXTERNAL_INC_DIR}/imgui
)

target_link_libraries(edge-lighting-capi-demo PRIVATE
    edge-lighting-c
    glfw
    glad
)
