#ifndef _EDGE_LIGHTING_CAPI_DEMO_IMAGE_QUAD_H_
#define _EDGE_LIGHTING_CAPI_DEMO_IMAGE_QUAD_H_

#include "gl-mini.h"

namespace EdgeLightingCapiDemo
{
    // Textured quad at a pixel-space rect. Backdrop under the neon so the
    // sampled border colours can be verified against the source image.
    // Same coord system as the effect: origin top-left, y grows downward.
    class ImageQuad
    {
    public:
        ~ImageQuad()
        {
            if (mProg) glDeleteProgram(mProg);
            if (mVbo) glDeleteBuffers(1, &mVbo);
            if (mVao) glDeleteVertexArrays(1, &mVao);
        }

        bool Init()
        {
            const char *vs = EL_CAPI_GLSL_VERSION R"(
                layout(location = 0) in vec2 aPos;
                out vec2 vTexCoord;
                uniform vec2 uViewport;
                uniform vec4 uRect;
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
            const char *fs = EL_CAPI_GLSL_VERSION R"(
                precision mediump float;
                in vec2 vTexCoord;
                out vec4 fragColor;
                uniform sampler2D uTex;
                void main() { fragColor = texture(uTex, vTexCoord); }
            )";

            mProg = LinkProgram(vs, fs, "ImageQuad");
            if (!mProg) return false;
            uViewport = glGetUniformLocation(mProg, "uViewport");
            uRect = glGetUniformLocation(mProg, "uRect");
            uTex = glGetUniformLocation(mProg, "uTex");

            const float verts[] = {
                0.f, 0.f,   0.f, 1.f,   1.f, 1.f,
                0.f, 0.f,   1.f, 1.f,   1.f, 0.f,
            };
            glGenVertexArrays(1, &mVao);
            glGenBuffers(1, &mVbo);
            glBindVertexArray(mVao);
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
            glBindVertexArray(0);
            return true;
        }

        void Draw(int fbW, int fbH, float x, float y, float w, float h, GLuint texture)
        {
            glDisable(GL_BLEND);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glUseProgram(mProg);
            glUniform2f(uViewport, static_cast<float>(fbW), static_cast<float>(fbH));
            glUniform4f(uRect, x, y, w, h);
            glUniform1i(uTex, 0);
            glBindVertexArray(mVao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glUseProgram(0);
            glEnable(GL_BLEND);
        }

    private:
        GLuint mProg = 0;
        GLuint mVao = 0;
        GLuint mVbo = 0;
        GLint uViewport = -1, uRect = -1, uTex = -1;
    };
} // namespace EdgeLightingCapiDemo

#endif
