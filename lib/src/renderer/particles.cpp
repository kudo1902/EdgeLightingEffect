#include "renderer/particles.h"
#include "util/log-util.h"
#include "util/gl-header.h"
#include <random>
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    static const char *const PARTICLE_VERTEX_SHADER_SRC = R"(
    #version 330 core
    precision highp float;

    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec4 aColor;
    layout(location = 2) in float aSize;

    out vec4 vColor;

    uniform vec2 uResolution;
    uniform vec2 uOffset;

    void main() {
        vColor = aColor;
        vec2 worldPos = aPos + uOffset;
        vec2 ndcPos = worldPos / (uResolution * 0.5);
        gl_Position = vec4(ndcPos, 0.0, 1.0);
        gl_PointSize = aSize;
    }
    )";

    static const char *const PARTICLE_FRAGMENT_SHADER_SRC = R"(
    #version 330 core
    precision highp float;

    in vec4 vColor;
    out vec4 fragColor;

    void main() {
        vec2 coord = gl_PointCoord - vec2(0.5);
        float dist = length(coord);
        if (dist > 0.5) {
            discard;
        }
        float alpha = smoothstep(0.5, 0.2, dist) * vColor.a;
        fragColor = vec4(vColor.rgb, alpha);
    }
    )";

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

    ParticleSystem::ParticleSystem()
    {
    }

    ParticleSystem::~ParticleSystem()
    {
        if (mVao != 0)
            glDeleteVertexArrays(1, &mVao);
        if (mVboPos != 0)
            glDeleteBuffers(1, &mVboPos);
        if (mVboCol != 0)
            glDeleteBuffers(1, &mVboCol);
        if (mVboSize != 0)
            glDeleteBuffers(1, &mVboSize);
        if (mShaderProgram != 0)
            glDeleteProgram(mShaderProgram);
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
        unsigned int vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &PARTICLE_VERTEX_SHADER_SRC, nullptr);
        glCompileShader(vertexShader);

        int success;
        char infoLog[512];
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
            LOG_E("Particle Vertex Shader Compile Error:\n%s", infoLog);
            glDeleteShader(vertexShader);
            return false;
        }

        unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &PARTICLE_FRAGMENT_SHADER_SRC, nullptr);
        glCompileShader(fragmentShader);

        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
            LOG_E("Particle Fragment Shader Compile Error:\n%s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return false;
        }

        mShaderProgram = glCreateProgram();
        glAttachShader(mShaderProgram, vertexShader);
        glAttachShader(mShaderProgram, fragmentShader);
        glLinkProgram(mShaderProgram);

        glGetProgramiv(mShaderProgram, GL_LINK_STATUS, &success);
        if (!success)
        {
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(mShaderProgram);
            return false;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return true;
    }

    void ParticleSystem::setupBuffers()
    {
        glGenVertexArrays(1, &mVao);
        glGenBuffers(1, &mVboPos);
        glGenBuffers(1, &mVboCol);
        glGenBuffers(1, &mVboSize);

        glBindVertexArray(mVao);

        glBindBuffer(GL_ARRAY_BUFFER, mVboPos);
        glBufferData(GL_ARRAY_BUFFER, mMaxParticles * sizeof(glm::vec2), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), (void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, mVboCol);
        glBufferData(GL_ARRAY_BUFFER, mMaxParticles * sizeof(glm::vec4), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, mVboSize);
        glBufferData(GL_ARRAY_BUFFER, mMaxParticles * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void *)0);

        glBindVertexArray(0);
    }

    void ParticleSystem::SetMaxParticles(int maxParticles)
    {
        if (mMaxParticles != maxParticles)
        {
            mMaxParticles = maxParticles;
            if (mVao != 0)
            {
                glDeleteVertexArrays(1, &mVao);
                glDeleteBuffers(1, &mVboPos);
                glDeleteBuffers(1, &mVboCol);
                glDeleteBuffers(1, &mVboSize);
                mVao = 0;
                mVboPos = 0;
                mVboCol = 0;
                mVboSize = 0;
                setupBuffers();
            }
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
            bool emitted = false;
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
        std::vector<glm::vec2> activePositions;
        std::vector<glm::vec4> activeColors;
        std::vector<float> activeSizes;

        activePositions.reserve(mParticles.size());
        activeColors.reserve(mParticles.size());
        activeSizes.reserve(mParticles.size());

        for (const auto &p : mParticles)
        {
            if (p.life > 0.0f)
            {
                activePositions.push_back(p.position);

                float lifeFactor = p.life / p.maxLife;
                glm::vec4 col = p.color;
                col.a *= lifeFactor * mGlobalIntensity;
                col.a = std::min(col.a, 1.0f);
                activeColors.push_back(col);

                activeSizes.push_back(p.size);
            }
        }

        if (activePositions.empty())
        {
            return;
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDepthMask(GL_FALSE);

        glUseProgram(mShaderProgram);

        int resLoc = glGetUniformLocation(mShaderProgram, "uResolution");
        glUniform2f(resLoc, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        int offLoc = glGetUniformLocation(mShaderProgram, "uOffset");
        glUniform2f(offLoc, offset.x, offset.y);

        glBindVertexArray(mVao);

        glBindBuffer(GL_ARRAY_BUFFER, mVboPos);
        glBufferSubData(GL_ARRAY_BUFFER, 0, activePositions.size() * sizeof(glm::vec2), activePositions.data());

        glBindBuffer(GL_ARRAY_BUFFER, mVboCol);
        glBufferSubData(GL_ARRAY_BUFFER, 0, activeColors.size() * sizeof(glm::vec4), activeColors.data());

        glBindBuffer(GL_ARRAY_BUFFER, mVboSize);
        glBufferSubData(GL_ARRAY_BUFFER, 0, activeSizes.size() * sizeof(float), activeSizes.data());

#ifdef GL_PROGRAM_POINT_SIZE
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif

        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(activePositions.size()));

        glBindVertexArray(0);
        glUseProgram(0);

        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

} // namespace EdgeLighting
