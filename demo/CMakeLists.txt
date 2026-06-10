add_executable(edge-lighting-demo
    src/main.cpp
)

target_link_libraries(edge-lighting-demo PRIVATE
    edge-lighting
    glfw
)

if(APPLE)
    target_link_libraries(edge-lighting-demo PRIVATE
        "-framework OpenGL"
    )
else()
    target_link_libraries(edge-lighting-demo PRIVATE
        OpenGL::GL
    )
endif()
