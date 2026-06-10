#include "renderer/particles.h"
#include <glad/glad.h>
#include <cstdlib>
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

    void main() {
        vColor = aColor;
        vec2 ndcPos = aPos / (uResolution * 0.5);
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

    static float randomFloat(float min, float max)
    {
        return min + static_cast<float>(rand()) / (static_cast<float>(RAND_MAX) / (max - min));
    }

    ParticleSystem::ParticleSystem()
    {
        srand(static_cast<unsigned int>(1337));
    }

    ParticleSystem::~ParticleSystem()
    {
        if (vao_ != 0)
            glDeleteVertexArrays(1, &vao_);
        if (vboPos_ != 0)
            glDeleteBuffers(1, &vboPos_);
        if (vboCol_ != 0)
            glDeleteBuffers(1, &vboCol_);
        if (vboSize_ != 0)
            glDeleteBuffers(1, &vboSize_);
        if (shaderProgram_ != 0)
            glDeleteProgram(shaderProgram_);
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
            glDeleteShader(vertexShader);
            return false;
        }

        unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &PARTICLE_FRAGMENT_SHADER_SRC, nullptr);
        glCompileShader(fragmentShader);

        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return false;
        }

        shaderProgram_ = glCreateProgram();
        glAttachShader(shaderProgram_, vertexShader);
        glAttachShader(shaderProgram_, fragmentShader);
        glLinkProgram(shaderProgram_);

        glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &success);
        if (!success)
        {
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(shaderProgram_);
            return false;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return true;
    }

    void ParticleSystem::setupBuffers()
    {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vboPos_);
        glGenBuffers(1, &vboCol_);
        glGenBuffers(1, &vboSize_);

        glBindVertexArray(vao_);

        glBindBuffer(GL_ARRAY_BUFFER, vboPos_);
        glBufferData(GL_ARRAY_BUFFER, maxParticles_ * sizeof(glm::vec2), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), (void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, vboCol_);
        glBufferData(GL_ARRAY_BUFFER, maxParticles_ * sizeof(glm::vec4), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, vboSize_);
        glBufferData(GL_ARRAY_BUFFER, maxParticles_ * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void *)0);

        glBindVertexArray(0);
    }

    void ParticleSystem::SetMaxParticles(int maxParticles)
    {
        if (maxParticles_ != maxParticles)
        {
            maxParticles_ = maxParticles;
            if (vao_ != 0)
            {
                setupBuffers();
            }
        }
    }

    void ParticleSystem::SetParticleSize(float size)
    {
        globalSize_ = size;
    }

    void ParticleSystem::SetParticleIntensity(float intensity)
    {
        globalIntensity_ = intensity;
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
            p.size = randomFloat(0.5f, 1.5f) * globalSize_;

            if (OnParticleSpawned)
            {
                OnParticleSpawned(p);
            }

            for (auto &activeP : particles_)
            {
                if (activeP.life <= 0.0f)
                {
                    activeP = p;
                    emitted = true;
                    break;
                }
            }

            if (!emitted && particles_.size() < static_cast<size_t>(maxParticles_))
            {
                particles_.push_back(p);
            }
        }
    }

    void ParticleSystem::Update(float deltaTime)
    {
        for (auto &p : particles_)
        {
            if (p.life > 0.0f)
            {
                p.position += p.velocity * deltaTime;
                p.velocity *= exp(-2.0f * deltaTime);
                p.life -= deltaTime;
            }
        }
    }

    void ParticleSystem::Render(int viewportWidth, int viewportHeight)
    {
        std::vector<glm::vec2> activePositions;
        std::vector<glm::vec4> activeColors;
        std::vector<float> activeSizes;

        activePositions.reserve(particles_.size());
        activeColors.reserve(particles_.size());
        activeSizes.reserve(particles_.size());

        for (const auto &p : particles_)
        {
            if (p.life > 0.0f)
            {
                activePositions.push_back(p.position);

                float lifeFactor = p.life / p.maxLife;
                glm::vec4 col = p.color;
                col.a *= lifeFactor * globalIntensity_;
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

        glUseProgram(shaderProgram_);

        int resLoc = glGetUniformLocation(shaderProgram_, "uResolution");
        glUniform2f(resLoc, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));

        glBindVertexArray(vao_);

        glBindBuffer(GL_ARRAY_BUFFER, vboPos_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, activePositions.size() * sizeof(glm::vec2), activePositions.data());

        glBindBuffer(GL_ARRAY_BUFFER, vboCol_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, activeColors.size() * sizeof(glm::vec4), activeColors.data());

        glBindBuffer(GL_ARRAY_BUFFER, vboSize_);
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
