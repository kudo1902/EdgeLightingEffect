#ifndef _EDGE_LIGHTING_CAPI_DEMO_BACKGROUND_QUAD_H_
#define _EDGE_LIGHTING_CAPI_DEMO_BACKGROUND_QUAD_H_

#include "gl-mini.h"

namespace EdgeLightingCapiDemo
{
    // Fullscreen debug checker drawn BEHIND the effect. Same purpose as the
    // C++ demo's BackgroundQuad: verifies neon blend vs opaque compositing.
    class BackgroundQuad
    {
    public:
        ~BackgroundQuad()
        {
            if (mProg) glDeleteProgram(mProg);
            if (mVbo) glDeleteBuffers(1, &mVbo);
            if (mVao) glDeleteVertexArrays(1, &mVao);
        }

        bool Init()
        {
            const char *vs = EL_CAPI_GLSL_VERSION R"(
                layout(location = 0) in vec2 aPos;
                void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
            )";
            const char *fs = EL_CAPI_GLSL_VERSION R"(
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
            mProg = LinkProgram(vs, fs, "BackgroundQuad");
            if (!mProg) return false;

            uCheckerSize = glGetUniformLocation(mProg, "uCheckerSize");
            uColorA = glGetUniformLocation(mProg, "uColorA");
            uColorB = glGetUniformLocation(mProg, "uColorB");

            const float verts[] = {
                -1.f,  1.f,  -1.f, -1.f,   1.f, -1.f,
                -1.f,  1.f,   1.f, -1.f,   1.f,  1.f,
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

        void Draw(float checkerSize, const float colorA[3], const float colorB[3])
        {
            glDisable(GL_BLEND);
            glUseProgram(mProg);
            glUniform1f(uCheckerSize, checkerSize);
            glUniform3f(uColorA, colorA[0], colorA[1], colorA[2]);
            glUniform3f(uColorB, colorB[0], colorB[1], colorB[2]);
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
        GLint uCheckerSize = -1, uColorA = -1, uColorB = -1;
    };
} // namespace EdgeLightingCapiDemo

#endif
