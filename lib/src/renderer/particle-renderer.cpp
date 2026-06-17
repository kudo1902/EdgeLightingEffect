#include "renderer/particle-renderer.h"
#include "util/log-util.h"
#include "util/geometry-utils.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

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

    void ParticleRenderer::Update(float deltaTime, float, float headPos, float time, const Config &config)
    {
        if (!config.particles.enable)
        {
            return;
        }

        if (config.stroke.animation == StrokeAnimation::MOVING)
        {
            emitParticlesAtHead(headPos, time, config);
        }

        mParticleSystem->Update(deltaTime);
    }

    void ParticleRenderer::Render(int viewportWidth, int viewportHeight, float, float, float, const Config &config)
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

    static glm::vec3 rgb2hsv(glm::vec3 c)
    {
        float r = c.r, g = c.g, b = c.b;
        float mx = glm::max(glm::max(r, g), b);
        float mn = glm::min(glm::min(r, g), b);
        float d = mx - mn;
        float h = 0.0f;
        if (d > 1e-10f)
        {
            if (mx == r) h = glm::mod((g - b) / d, 6.0f);
            else if (mx == g) h = (b - r) / d + 2.0f;
            else h = (r - g) / d + 4.0f;
            h /= 6.0f;
        }
        return glm::vec3(h, (mx > 1e-10f) ? d / mx : 0.0f, mx);
    }

    static glm::vec3 hsv2rgb(glm::vec3 c)
    {
        float h = c.x, s = c.y, v = c.z;
        float r = v, g = v, b = v;
        if (s > 0.0f && v > 0.0f)
        {
            h = glm::fract(h) * 6.0f;
            int i = (int)h;
            float f = h - (float)i;
            float p = v * (1.0f - s);
            float q = v * (1.0f - s * f);
            float t = v * (1.0f - s * (1.0f - f));
            switch (i)
            {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
            }
        }
        return glm::vec3(r, g, b);
    }

    static glm::vec3 blendHsv(glm::vec3 a, glm::vec3 b, float t)
    {
        glm::vec3 ha = rgb2hsv(a);
        glm::vec3 hb = rgb2hsv(b);
        float dh = hb.x - ha.x;
        if (dh > 0.5f) dh -= 1.0f;
        if (dh < -0.5f) dh += 1.0f;
        return hsv2rgb(glm::vec3(ha.x + dh * t, glm::mix(ha.y, hb.y, t), glm::mix(ha.z, hb.z, t)));
    }

    static glm::vec3 blendColors(glm::vec3 a, glm::vec3 b, float t, BlendSpace space)
    {
        if (space == BlendSpace::HSV) return blendHsv(a, b, t);
        return glm::mix(a, b, t);
    }

    static glm::vec4 sampleColorStops(float pos, const std::vector<ColorStop> &stops, BlendSpace blendSpace)
    {
        int count = (int)stops.size();
        if (count <= 0) return glm::vec4(1.0f);
        if (count == 1) return stops[0].color;
        auto bl = [&](glm::vec3 a, glm::vec3 b, float t) { return blendColors(a, b, t, blendSpace); };
        if (count == 2)
        {
            float t = 1.0f - std::abs(2.0f * pos - 1.0f);
            return glm::vec4(bl(glm::vec3(stops[0].color), glm::vec3(stops[1].color), t), 1.0f);
        }
        for (int i = 0; i < count; i++)
        {
            int next = (i + 1 < count) ? i + 1 : 0;
            float a = stops[i].position;
            float b = stops[next].position;
            if (next != 0)
            {
                if (pos >= a && pos < b)
                {
                    float t = (pos - a) / std::max(b - a, 0.0001f);
                    return glm::vec4(bl(glm::vec3(stops[i].color), glm::vec3(stops[next].color), t), 1.0f);
                }
            }
            else
            {
                float wrapLen = (1.0f - a) + b;
                if (pos >= a)
                {
                    float t = (pos - a) / std::max(wrapLen, 0.0001f);
                    return glm::vec4(bl(glm::vec3(stops[i].color), glm::vec3(stops[next].color), t), 1.0f);
                }
                if (pos < b)
                {
                    float t = ((1.0f - a) + pos) / std::max(wrapLen, 0.0001f);
                    return glm::vec4(bl(glm::vec3(stops[i].color), glm::vec3(stops[next].color), t), 1.0f);
                }
            }
        }
        return stops[0].color;
    }

    void ParticleRenderer::emitParticlesAtHead(float headPos, float time, const Config &config)
    {
        for (int i = 0; i < config.stroke.lineCount; ++i)
        {
            float offset = static_cast<float>(i) / static_cast<float>(config.stroke.lineCount);
            float headProgress = glm::fract(headPos + offset);
            glm::vec2 spawnPos = GeometryUtils::GetPointOnRectangle(headProgress, config.geometry);

            glm::vec4 emitterColor = sampleColorStops(headProgress, config.stroke.colorStops, config.stroke.blendSpace);
            glm::vec4 color = (config.particles.color.a > 0.0f) ? config.particles.color : emitterColor;
            mParticleSystem->Emit(spawnPos, color, config.stroke.speed, 2);
        }
    }

} // namespace EdgeLighting
