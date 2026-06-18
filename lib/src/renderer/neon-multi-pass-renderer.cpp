#include "renderer/neon-multi-pass-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    NeonMultiPassRenderer::~NeonMultiPassRenderer()
    {
        destroyFbo();
    }

    bool NeonMultiPassRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonMultiPassRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        return true;
    }

    void NeonMultiPassRenderer::Update(float, float, float, float, const Config &)
    {
    }

    void NeonMultiPassRenderer::Render(int viewportWidth, int viewportHeight, float, float headPos, float time, const Config &config)
    {
        if (!config.multipassNeon.enable)
            return;

        if (viewportWidth != mFboW || viewportHeight != mFboH)
            createFbo(viewportWidth, viewportHeight);

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        // --- Pass 1: Render conic gradient to FBO texture ---
        glBindFramebuffer(GL_FRAMEBUFFER, mGradientFbo);
        glViewport(0, 0, viewportWidth, viewportHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        mGradientShader.Use();
        mGradientShader.SetUniform("uMVP", mvp);
        mGradientShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mGradientShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mGradientShader.SetUniform("uTime", time);
        mGradientShader.SetUniform("uSpeed", config.multipassNeon.speed);
        mGradientShader.SetUniform("uStrokeThickness", config.multipassNeon.thickness);
        mGradientShader.SetUniform("uBlendSpace", static_cast<int>(config.multipassNeon.blendSpace));
        mGradientShader.SetUniform("uColorStopCount", static_cast<int>(config.multipassNeon.colorStops.size()));
        for (int i = 0; i < static_cast<int>(config.multipassNeon.colorStops.size()) && i < Config::MultiPassNeon::MAX_COLOR_STOPS; ++i)
        {
            std::string posName = "uColorStopPositions[" + std::to_string(i) + "]";
            mGradientShader.SetUniform(posName.c_str(), config.multipassNeon.colorStops[i].position);
            std::string colName = "uColorStopColors[" + std::to_string(i) + "]";
            mGradientShader.SetUniform(colName.c_str(), config.multipassNeon.colorStops[i].color);
        }
        glDisable(GL_BLEND);
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        // --- Pass 2: Sample gradient texture, apply SDF glow to screen ---
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, viewportWidth, viewportHeight);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mGradientTex);
        mGlowShader.Use();
        mGlowShader.SetUniform("uMVP", mvp);
        mGlowShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mGlowShader.SetUniform("uCornerRadius", config.geometry.cornerRadius);
        mGlowShader.SetUniform("uStrokeThickness", config.multipassNeon.thickness);
        mGlowShader.SetUniform("uIntensity", config.multipassNeon.intensity);
        mGlowShader.SetUniform("uTime", time);
        mGlowShader.SetUniform("uSpeed", config.multipassNeon.speed);
        mGlowShader.SetUniform("uGlowSize", config.multipassNeon.glowSize);
        mGlowShader.SetUniform("uViewportSize", glm::vec2(viewportWidth, viewportHeight));
        mGlowShader.SetUniform("uGradient", 0);
        mGlowShader.SetUniform("uShowGradient", config.multipassNeon.showGradient);
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mGlowShader.Unuse();
    }

    void NeonMultiPassRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mGradientShader.IsValid())
            setupGeometry(config);
    }

    bool NeonMultiPassRenderer::setupShaders()
    {
        mGradientShader = ShaderProgram(ShaderSource::NEON_VERT_SRC, ShaderSource::NEON_GRADIENT_FRAG_SRC);
        mGlowShader = ShaderProgram(ShaderSource::NEON_VERT_SRC, ShaderSource::NEON_GLOW_FRAG_SRC);
        return mGradientShader.IsValid() && mGlowShader.IsValid();
    }

    void NeonMultiPassRenderer::setupGeometry(const Config &config)
    {
        float margin = 600.0f;
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

    void NeonMultiPassRenderer::destroyFbo()
    {
        if (mGradientFbo)
        {
            glDeleteFramebuffers(1, &mGradientFbo);
            mGradientFbo = 0;
        }
        if (mGradientTex)
        {
            glDeleteTextures(1, &mGradientTex);
            mGradientTex = 0;
        }
        mFboW = mFboH = 0;
    }

    void NeonMultiPassRenderer::createFbo(int w, int h)
    {
        destroyFbo();

        glGenTextures(1, &mGradientTex);
        glBindTexture(GL_TEXTURE_2D, mGradientTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &mGradientFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, mGradientFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mGradientTex, 0);

        GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (s != GL_FRAMEBUFFER_COMPLETE)
            LOG_W("Gradient FBO incomplete (status=%x)", s);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        mFboW = w;
        mFboH = h;
    }
}
