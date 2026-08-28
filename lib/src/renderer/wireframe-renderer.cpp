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

    void WireframeRenderer::Update(float, float, const Config &)
    {
    }

    void WireframeRenderer::Render(int viewportWidth, int viewportHeight, float, const Config &config)
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

        // Hand back the same blend state every other renderer leaves behind,
        // not just "blending on". This one happens to be registered first in
        // the demo, so whatever func was live before it was the caller's - but
        // relying on that makes the registration order load-bearing.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void WireframeRenderer::OnConfigChanged(const Config &config)
    {
        // Gate the VBO upload on the geometry alone, like the neon renderers
        // do. OnConfigChanged fires on ANY change to the composited config, so
        // with an animation attached it runs every frame - and the box only
        // depends on width and height.
        const bool geometryDirty = config.geometry != mCurrentConfig.geometry;
        mCurrentConfig = config;
        if (geometryDirty && mShaderProgram.IsValid())
        {
            buildGeometry(config);
        }
    }

    bool WireframeRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::WIREFRAME_VERT_SRC,
                                       ShaderSource::WIREFRAME_FRAG_SRC,
                                       "WireframeRenderer");
        return mShaderProgram.IsValid();
    }

    void WireframeRenderer::buildGeometry(const Config &config)
    {
        // Sharp box on purpose, even when geometry.cornerRadius is set: this is
        // a debug bounding box, so it shows the extent the config asked for
        // rather than tracing the rounded outline the neon actually draws.
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
