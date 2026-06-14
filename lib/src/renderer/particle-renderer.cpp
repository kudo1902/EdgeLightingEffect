#include "renderer/particle-renderer.h"
#include "util/log-util.h"
#include "util/geometry-utils.h"
#include <glm/gtc/matrix_transform.hpp>

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
            LOG_E("Failed to initialize particle system in ParticleRenderer.");
            return false;
        }
        return true;
    }

    void ParticleRenderer::Update(float deltaTime, float progress, float time, const Config &config)
    {
        if (!config.particles.enable)
        {
            return;
        }

        if (config.stroke.animation == StrokeAnimation::MOVING)
        {
            emitParticlesAtHead(progress, time, config);
        }

        mParticleSystem->Update(deltaTime);
    }

    void ParticleRenderer::Render(int viewportWidth, int viewportHeight, float, float, const Config &config)
    {
        if (!config.particles.enable)
        {
            return;
        }

        glm::vec2 offset(config.geometry.position.x + config.geometry.width * 0.5f - viewportWidth * 0.5f,
                         viewportHeight * 0.5f - config.geometry.position.y - config.geometry.height * 0.5f);
        mParticleSystem->Render(viewportWidth, viewportHeight, offset);
    }

    void ParticleRenderer::OnConfigChanged(const Config &config)
    {
        if (mParticleSystem)
        {
            mParticleSystem->SetMaxParticles(config.particles.maxCount);
            mParticleSystem->SetParticleSize(config.particles.size);
            mParticleSystem->SetParticleIntensity(config.particles.intensity);
        }
    }

    glm::vec3 ParticleRenderer::getRainbowColor(float p)
    {
        glm::vec3 c1(1.0f, 0.0f, 0.0f);
        glm::vec3 c2(1.0f, 1.0f, 0.0f);
        glm::vec3 c3(0.0f, 1.0f, 0.0f);
        glm::vec3 c4(0.0f, 1.0f, 1.0f);
        glm::vec3 c5(0.0f, 0.0f, 1.0f);
        glm::vec3 c6(1.0f, 0.0f, 1.0f);

        float w = 1.0f / 6.0f;
        if (p < w)
        {
            return glm::mix(c1, c2, p / w);
        }
        if (p < 2.0f * w)
        {
            return glm::mix(c2, c3, (p - w) / w);
        }
        if (p < 3.0f * w)
        {
            return glm::mix(c3, c4, (p - 2.0f * w) / w);
        }
        if (p < 4.0f * w)
        {
            return glm::mix(c4, c5, (p - 3.0f * w) / w);
        }
        if (p < 5.0f * w)
        {
            return glm::mix(c5, c6, (p - 4.0f * w) / w);
        }
        return glm::mix(c6, c1, (p - 5.0f * w) / w);
    }

    void ParticleRenderer::emitParticlesAtHead(float progress, float time, const Config &config)
    {
        for (int i = 0; i < config.stroke.lineCount; ++i)
        {
            float offset = static_cast<float>(i) / static_cast<float>(config.stroke.lineCount);
            float headProgress = glm::fract(time * config.stroke.speed + offset);
            glm::vec2 spawnPos = GeometryUtils::GetPointOnRectangle(headProgress, config.geometry);

            glm::vec4 emitterColor;
            if (config.stroke.colorMode == StrokeColorMode::STATIC)
            {
                emitterColor = config.stroke.primaryColor;
            }
            else if (config.stroke.colorMode == StrokeColorMode::GRADIENT)
            {
                float t = 1.0f - abs(2.0f * headProgress - 1.0f);
                emitterColor = glm::mix(config.stroke.primaryColor, config.stroke.secondaryColor, t);
            }
            else
            {
                float shift = glm::fract(headProgress - time * 0.25f);
                emitterColor = glm::vec4(getRainbowColor(shift), 1.0f);
            }

            glm::vec4 color = (config.particles.color.a > 0.0f) ? config.particles.color : emitterColor;
            mParticleSystem->Emit(spawnPos, color, config.stroke.speed, 2);
        }
    }

} // namespace EdgeLighting
