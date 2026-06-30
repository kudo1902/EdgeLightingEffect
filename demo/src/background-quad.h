#ifndef _EDGE_LIGHTING_DEMO_BACKGROUND_QUAD_H_
#define _EDGE_LIGHTING_DEMO_BACKGROUND_QUAD_H_

#include "gl/gl-header.h" // GL symbols + GLSL_VERSION
#include "gl/shader-program.h"
#include "gl/vertex-array.h"
#include <glm/glm.hpp>
#include <string>

namespace EdgeLightingDemo
{
    /// Demo-only debug aid: a fullscreen checkerboard drawn BEHIND the effect.
    ///
    /// It exists to verify the neon renderer's compositing: with `neon.opaque`
    /// off you should see the checker through the dark surround (and tinted
    /// under the halo); with `neon.opaque` on the effect's draw region writes
    /// opaquely and occludes the checker. A checkerboard makes partial-alpha
    /// blending obvious in a way a flat colour can't.
    class BackgroundQuad
    {
    public:
        bool Init()
        {
            const std::string vs = std::string(GLSL_VERSION) + "\n" + R"(
                layout(location = 0) in vec2 aPos;
                void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
            )";
            const std::string fs = std::string(GLSL_VERSION) + "\n" + R"(
                precision mediump float;
                out vec4 fragColor;
                uniform float uCheckerSize;
                uniform vec3  uColorA;
                uniform vec3  uColorB;
                void main() {
                    vec2 cell = floor(gl_FragCoord.xy / max(uCheckerSize, 1.0));
                    float c = mod(cell.x + cell.y, 2.0);
                    fragColor = vec4(mix(uColorA, uColorB, c), 1.0);
                }
            )";

            mShader = EdgeLighting::ShaderProgram(vs.c_str(), fs.c_str(), "BackgroundQuad");
            if (!mShader.IsValid())
            {
                return false;
            }

            // Fullscreen quad in NDC.
            // clang-format off
            float verts[] = {
                -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
                -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
            };
            // clang-format on
            mVao.SetVertexData(verts, sizeof(verts));
            mVao.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
            return true;
        }

        void Draw(float checkerSize, const glm::vec3 &colorA, const glm::vec3 &colorB)
        {
            glDisable(GL_BLEND); // opaque fill
            mShader.Use();
            mShader.SetUniform("uCheckerSize", checkerSize);
            mShader.SetUniform("uColorA", colorA);
            mShader.SetUniform("uColorB", colorB);
            mVao.DrawArrays(GL_TRIANGLES, 6);
            mShader.Unuse();
            glEnable(GL_BLEND);
        }

    private:
        EdgeLighting::ShaderProgram mShader;
        EdgeLighting::VertexArray mVao;
    };

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_BACKGROUND_QUAD_H_
