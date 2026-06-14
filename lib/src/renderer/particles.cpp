#include "renderer/particles.h"
#include "util/log-util.h"
#include "shaders.h"

#include <random>
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    static std::mt19937 &rng()
    {
        static std::mt19937 instance{1337};
        return instance;
    }

    static float randomFloat(float min, float max)
    {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(rng());
    }

    bool ParticleSystem::Initialize()
    {
        if (!setupShaders())
        {
            return false;
        }
        setupBuffers();
        return true;
    }

    bool ParticleSystem::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::PARTICLE_VERT_SRC, ShaderSource::PARTICLE_FRAG_SRC);
        return mShaderProgram.IsValid();
    }

    void ParticleSystem::setupBuffers()
    {
        mVertexArray.SetVertexData(nullptr, mMaxParticles * sizeof(ParticleVertex), GL_DYNAMIC_DRAW);
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, sizeof(ParticleVertex), offsetof(ParticleVertex, position));
        mVertexArray.SetAttribPointer(1, 4, GL_FLOAT, sizeof(ParticleVertex), offsetof(ParticleVertex, color));
        mVertexArray.SetAttribPointer(2, 1, GL_FLOAT, sizeof(ParticleVertex), offsetof(ParticleVertex, size));
    }

    void ParticleSystem::SetMaxParticles(int maxParticles)
    {
        if (mMaxParticles != maxParticles)
        {
            mMaxParticles = maxParticles;
            if (mShaderProgram.IsValid())
                setupBuffers();
        }
    }

    void ParticleSystem::SetParticleSize(float size)
    {
        mGlobalSize = size;
    }

    void ParticleSystem::SetParticleIntensity(float intensity)
    {
        mGlobalIntensity = intensity;
    }

    void ParticleSystem::Emit(const glm::vec2 &position, const glm::vec4 &color, float speed, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            Particle p;
            p.position = position;

            float angle = randomFloat(0.0f, 2.0f * 3.14159265f);
            float velocityMag = randomFloat(30.0f, 120.0f) * speed;
            p.velocity = glm::vec2(cos(angle), sin(angle)) * velocityMag;

            p.color = color;
            p.life = randomFloat(0.3f, 0.8f);
            p.maxLife = p.life;
            p.size = randomFloat(0.5f, 1.5f) * mGlobalSize;

            if (OnParticleSpawned)
            {
                OnParticleSpawned(p);
            }

            bool emitted = false;
            for (auto &activeP : mParticles)
            {
                if (activeP.life <= 0.0f)
                {
                    activeP = p;
                    emitted = true;
                    break;
                }
            }

            if (!emitted && mParticles.size() < static_cast<size_t>(mMaxParticles))
            {
                mParticles.push_back(p);
            }
        }
    }

    void ParticleSystem::Update(float deltaTime)
    {
        for (auto &p : mParticles)
        {
            if (p.life > 0.0f)
            {
                p.position += p.velocity * deltaTime;
                p.velocity *= exp(-2.0f * deltaTime);
                p.life -= deltaTime;
            }
        }
    }

    void ParticleSystem::Render(int viewportWidth, int viewportHeight, glm::vec2 offset)
    {
        std::vector<ParticleVertex> vertices;
        vertices.reserve(mParticles.size());

        for (const auto &p : mParticles)
        {
            if (p.life <= 0.0f)
            {
                continue;
            }

            float lifeFactor = p.life / p.maxLife;
            glm::vec4 col = p.color;
            col.a *= lifeFactor * mGlobalIntensity;
            col.a = std::min(col.a, 1.0f);

            vertices.push_back({p.position, col, p.size});
        }

        if (vertices.empty())
        {
            return;
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uResolution", glm::vec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
        mShaderProgram.SetUniform("uOffset", offset);

        mVertexArray.Bind();
        mVertexArray.BindBuffer();
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(ParticleVertex), vertices.data());

#ifdef GL_PROGRAM_POINT_SIZE
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif

        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(vertices.size()));
        mVertexArray.Unbind();
        mShaderProgram.Unuse();

        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

} // namespace EdgeLighting
