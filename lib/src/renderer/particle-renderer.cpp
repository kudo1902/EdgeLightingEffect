#include "renderer/particle-renderer.h"
#include <iostream>

namespace EdgeLighting
{

    ParticleRenderer::ParticleRenderer()
    {
        mParticleSystem = std::make_unique<ParticleSystem>();
    }

    bool ParticleRenderer::Initialize()
    {
        if (!mParticleSystem->Initialize())
        {
            std::cerr << "Failed to initialize particle system in ParticleRenderer." << std::endl;
            return false;
        }
        return true;
    }

    void ParticleRenderer::Update(float deltaTime, float progress, float time, const Config &config)
    {
        if (!config.enableParticles)
            return;

        emitParticlesAtHead(progress, time, config);
        mParticleSystem->Update(deltaTime);
    }

    void ParticleRenderer::Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config)
    {
        if (!config.enableParticles)
            return;
        mParticleSystem->Render(viewportWidth, viewportHeight);
    }

    void ParticleRenderer::OnConfigChanged(const Config &config)
    {
        if (mParticleSystem)
        {
            mParticleSystem->SetMaxParticles(config.maxParticles);
            mParticleSystem->SetParticleSize(config.particleSize);
            mParticleSystem->SetParticleIntensity(config.particleIntensity);
        }
    }

    void ParticleRenderer::emitParticlesAtHead(float progress, float time, const Config &config)
    {
        for (int i = 0; i < config.lightCount; ++i)
        {
            float offset = static_cast<float>(i) / static_cast<float>(config.lightCount);
            float headProgress = glm::fract(progress + offset);
            glm::vec2 spawnPos = GeometryUtils::GetPointOnRectangle(headProgress, config);

            // Determine color
            glm::vec4 emitterColor;
            if (config.colorMode == ColorMode::STATIC)
            {
                emitterColor = config.primaryColor;
            }
            else if (config.colorMode == ColorMode::GRADIENT)
            {
                emitterColor = glm::mix(config.primaryColor, config.secondaryColor, headProgress);
            }
            else
            {
                // Rainbow shift
                float shift = glm::fract(headProgress - time * 0.25f);
                auto getRainbow = [](float p)
                {
                    glm::vec3 c1(1.0f, 0.0f, 0.0f);
                    glm::vec3 c2(1.0f, 1.0f, 0.0f);
                    glm::vec3 c3(0.0f, 1.0f, 0.0f);
                    glm::vec3 c4(0.0f, 1.0f, 1.0f);
                    glm::vec3 c5(0.0f, 0.0f, 1.0f);
                    glm::vec3 c6(1.0f, 0.0f, 1.0f);

                    float w = 1.0f / 6.0f;
                    if (p < w)
                        return glm::mix(c1, c2, p / w);
                    if (p < 2.0f * w)
                        return glm::mix(c2, c3, (p - w) / w);
                    if (p < 3.0f * w)
                        return glm::mix(c3, c4, (p - 2.0f * w) / w);
                    if (p < 4.0f * w)
                        return glm::mix(c4, c5, (p - 3.0f * w) / w);
                    if (p < 5.0f * w)
                        return glm::mix(c5, c6, (p - 4.0f * w) / w);
                    return glm::mix(c6, c1, (p - 5.0f * w) / w);
                };
                emitterColor = glm::vec4(getRainbow(shift), 1.0f);
            }

            glm::vec4 color = (config.particleColor.a > 0.0f) ? config.particleColor : emitterColor;
            mParticleSystem->Emit(spawnPos, color, config.speed, 2);
        }
    }

} // namespace EdgeLighting
