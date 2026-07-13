#ifndef _EDGE_LIGHTING_DEMO_IMAGE_QUAD_H_
#define _EDGE_LIGHTING_DEMO_IMAGE_QUAD_H_

#include "gl/gl-header.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

#include <string>

namespace EdgeLightingDemo
{
    /// Textured quad drawn at a pixel-space rectangle. Used as a demo backdrop
    /// under the neon effect so the sampled border colours can be verified
    /// against the source image visually.
    ///
    /// The rect uses the same coordinate system as EdgeLighting::RectGeometry:
    /// origin at the top-left of the framebuffer, y grows downward. The shader
    /// flips y into GL clip space; texture coordinates are passed through
    /// directly (aPos.y = 0 → texcoord.y = 0), which samples row 0 of the
    /// uploaded pixel buffer - stb_image loads with row 0 = image top, so this
    /// renders the image upright.
    class ImageQuad
    {
    public:
        bool Init()
        {
            const std::string vs = std::string(GLSL_VERSION) + "\n" + R"(
                layout(location = 0) in vec2 aPos;   // [0,1] unit-quad corner
                out vec2 vTexCoord;
                uniform vec2 uViewport;              // fbW, fbH
                uniform vec4 uRect;                  // x, y, w, h in pixels
                void main() {
                    vec2 px = uRect.xy + aPos * uRect.zw;
                    vec2 ndc = vec2(
                        2.0 * px.x / uViewport.x - 1.0,
                        1.0 - 2.0 * px.y / uViewport.y
                    );
                    gl_Position = vec4(ndc, 0.0, 1.0);
                    vTexCoord = aPos;
                }
            )";
            const std::string fs = std::string(GLSL_VERSION) + "\n" + R"(
                precision mediump float;
                in vec2 vTexCoord;
                out vec4 fragColor;
                uniform sampler2D uTex;
                void main() {
                    fragColor = texture(uTex, vTexCoord);
                }
            )";

            mShader = EdgeLighting::ShaderProgram(vs.c_str(), fs.c_str(), "ImageQuad");
            if (!mShader.IsValid())
            {
                return false;
            }

            // Unit quad (top-left, bottom-left, bottom-right / top-left, bottom-right, top-right).
            // clang-format off
            float verts[] = {
                0.0f, 0.0f,   0.0f, 1.0f,   1.0f, 1.0f,
                0.0f, 0.0f,   1.0f, 1.0f,   1.0f, 0.0f,
            };
            // clang-format on
            mVao.SetVertexData(verts, sizeof(verts));
            mVao.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
            return true;
        }

        /// Draws the texture bound to unit 0 into the rectangle (@p x, @p y,
        /// @p w, @p h) in framebuffer pixels (top-left origin). Blending is
        /// disabled for the draw so the image writes opaquely, then restored.
        void Draw(int fbW, int fbH, float x, float y, float w, float h, GLuint texture)
        {
            glDisable(GL_BLEND);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);

            mShader.Use();
            mShader.SetUniform("uViewport",
                               glm::vec2(static_cast<float>(fbW), static_cast<float>(fbH)));
            mShader.SetUniform("uRect", glm::vec4(x, y, w, h));
            mShader.SetUniform("uTex", 0);
            mVao.DrawArrays(GL_TRIANGLES, 6);
            mShader.Unuse();

            glEnable(GL_BLEND);
        }

    private:
        EdgeLighting::ShaderProgram mShader;
        EdgeLighting::VertexArray mVao;
    };

} // namespace EdgeLightingDemo

#endif // _EDGE_LIGHTING_DEMO_IMAGE_QUAD_H_
