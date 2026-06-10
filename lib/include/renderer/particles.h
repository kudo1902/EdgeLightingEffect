#ifndef EDGE_LIGHTING_PARTICLES_H
#define EDGE_LIGHTING_PARTICLES_H

#include <glm/glm.hpp>
#include <vector>
#include <functional>

namespace EdgeLighting
{
    struct Particle
    {
        glm::vec2 position = glm::vec2(0.0f);
        glm::vec2 velocity = glm::vec2(0.0f);
        glm::vec4 color = glm::vec4(1.0f);
        float life = 0.0f; // 1.0 to 0.0
        float maxLife = 1.0f;
        float size = 5.0f;
    };

    class ParticleSystem
    {
    public:
        ParticleSystem();
        ~ParticleSystem();

        // Public API (PascalCase)
        bool Initialize();
        void Update(float deltaTime);
        void Render(int viewportWidth, int viewportHeight);
        void Emit(const glm::vec2 &position, const glm::vec4 &color, float speed, int count);

        void SetMaxParticles(int maxParticles);
        void SetParticleSize(float size);
        void SetParticleIntensity(float intensity);

        // Callback event (prefixed with 'On')
        std::function<void(const Particle &)> OnParticleSpawned;

    private:
        // Private methods (camelCase)
        bool setupShaders();
        void setupBuffers();
        void updateBuffers();

        int maxParticles_ = 200;
        float globalSize_ = 6.0f;
        float globalIntensity_ = 1.0f;

        std::vector<Particle> particles_;

        // OpenGL Objects
        unsigned int shaderProgram_ = 0;
        unsigned int vao_ = 0;
        unsigned int vboPos_ = 0;
        unsigned int vboCol_ = 0;
        unsigned int vboSize_ = 0;
    };

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_PARTICLES_H
