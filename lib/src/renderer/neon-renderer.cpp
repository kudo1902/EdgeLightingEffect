#include "renderer/neon-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    bool NeonRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        return true;
    }

    void NeonRenderer::Update(float, float, float, float, const Config &)
    {
    }

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float, float headPos, float time, const Config &config)
    {
        if (!config.neon.enable)
            return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        mShaderProgram.Use();

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mShaderProgram.SetUniform("uStrokeThickness", config.neon.thickness);
        mShaderProgram.SetUniform("uIntensity", config.neon.intensity);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uSpeed", config.neon.speed);
        mShaderProgram.SetUniform("uGlowSize", config.neon.glowSize);
        mShaderProgram.SetUniform("uBlendSpace", static_cast<int>(config.neon.blendSpace));
        mShaderProgram.SetUniform("uColorStopCount", static_cast<int>(config.neon.colorStops.size()));
        for (int i = 0; i < static_cast<int>(config.neon.colorStops.size()) && i < Config::Neon::MAX_COLOR_STOPS; ++i)
        {
            std::string posName = "uColorStopPositions[" + std::to_string(i) + "]";
            mShaderProgram.SetUniform(posName.c_str(), config.neon.colorStops[i].position);
            std::string colName = "uColorStopColors[" + std::to_string(i) + "]";
            mShaderProgram.SetUniform(colName.c_str(), config.neon.colorStops[i].color);
        }

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mShaderProgram.IsValid())
            setupGeometry(config);
    }

    bool NeonRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::NEON_VERT_SRC, ShaderSource::NEON_FRAG_SRC);
        return mShaderProgram.IsValid();
    }

    void NeonRenderer::setupGeometry(const Config &config)
    {
        float margin = 600.0f; // Large margin to avoid hard clipping the glow
        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;
        float l = -(halfW + margin);
        float r = halfW + margin;
        float b = -(halfH + margin);
        float t = halfH + margin;

        float verts[] = {
            l, t, l, b, r, b,
            l, t, r, b, r, t,
        };

        mVertexArray.SetVertexData(verts, sizeof(verts));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }
}
