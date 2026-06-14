#include "renderer/wireframe-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    bool WireframeRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link WireframeRenderer shaders.");
            return false;
        }
        buildGeometry(mCurrentConfig);
        return true;
    }

    void WireframeRenderer::Update(float, float, float, float, const Config &)
    {
    }

    void WireframeRenderer::Render(int viewportWidth, int viewportHeight, float, float, float, const Config &config)
    {
        if (!config.wireframe.enable)
        {
            return;
        }

        glDisable(GL_BLEND);
        mShaderProgram.Use();

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uColor", config.wireframe.color);

        mVertexArray.DrawArrays(GL_LINE_LOOP, 4);

        mShaderProgram.Unuse();
        glEnable(GL_BLEND);
    }

    void WireframeRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mShaderProgram.IsValid())
        {
            buildGeometry(config);
        }
    }

    bool WireframeRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::WIREFRAME_VERT_SRC, ShaderSource::WIREFRAME_FRAG_SRC);
        return mShaderProgram.IsValid();
    }

    void WireframeRenderer::buildGeometry(const Config &config)
    {
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;

        // clang-format off
        float verts[] = {
            -halfW, halfH, halfW, halfH,
            halfW, -halfH, -halfW, -halfH,
        };
        // clang-format on

        mVertexArray.SetVertexData(verts, sizeof(verts));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

} // namespace EdgeLighting
