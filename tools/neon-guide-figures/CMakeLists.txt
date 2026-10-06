# neon-guide-figures - renders every image in docs/neon-onboarding-guide.md.
#
# In-tree only: configure the repo with
#     cmake -S . -B build -G Ninja -DEDGE_LIGHTING_BUILD_TOOLS=ON
# and run build/tools/neon-guide-figures/neon-guide-figures. It links the
# STATIC library on purpose: the pass figures watch the renderer through the
# glad function pointers that library calls, which the C ABI dylib keeps
# private. See README.md.

add_executable(neon-guide-figures
    src/main.cpp
    src/figures.cpp
    src/canvas.cpp
    ../common/pass-recorder.cpp
)

# The pass recorder is shared with neon-scale-check.
target_include_directories(neon-guide-figures PRIVATE ../common)

# Where the figures go by default: this checkout's docs.
target_compile_definitions(neon-guide-figures PRIVATE
    DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../docs"
)

target_link_libraries(neon-guide-figures PRIVATE
    edge-lighting
    glfw
)
