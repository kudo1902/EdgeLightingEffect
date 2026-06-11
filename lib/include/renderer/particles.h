#ifndef _EDGE_LIGHTING_PARTICLES_H_
#define _EDGE_LIGHTING_PARTICLES_H_

#include <glm/glm.hpp>
#include <vector>
#include <functional>

namespace EdgeLighting
{
    typedef struct Particle
    {
        glm::vec2 position = glm::vec2(0.0f);
        glm::vec2 velocity = glm::vec2(0.0f);
        glm::vec4 color = glm::vec4(1.0f);
        float life = 0.0f; // 1.0 to 0.0
        float maxLife = 1.0f;
        float size = 5.0f;
    } Particle;

    class ParticleSystem
    {
    public:
        ParticleSystem();
        ~ParticleSystem();

        bool Initialize();
        void Update(float deltaTime);
        void Render(int viewportWidth, int viewportHeight, glm::vec2 offset = glm::vec2(0.0f));
        void Emit(const glm::vec2 &position, const glm::vec4 &color, float speed, int count);

        void SetMaxParticles(int maxParticles);
        void SetParticleSize(float size);
        void SetParticleIntensity(float intensity);

        std::function<void(const Particle &)> OnParticleSpawned;

    private:
        // Private methods (camelCase)
        bool setupShaders();
        void setupBuffers();
        void updateBuffers();

        int mMaxParticles = 200;
        float mGlobalSize = 6.0f;
        float mGlobalIntensity = 1.0f;

        std::vector<Particle> mParticles;

        // OpenGL Objects
        unsigned int mShaderProgram = 0;
        unsigned int mVao = 0;
        unsigned int mVboPos = 0;
        unsigned int mVboCol = 0;
        unsigned int mVboSize = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLES_H_
