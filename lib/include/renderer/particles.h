#ifndef _EDGE_LIGHTING_PARTICLES_H_
#define _EDGE_LIGHTING_PARTICLES_H_

#include <glm/glm.hpp>
#include <vector>
#include <functional>
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    typedef struct Particle
    {
        glm::vec2 position = glm::vec2(0.0f);
        glm::vec2 velocity = glm::vec2(0.0f);
        glm::vec4 color = glm::vec4(1.0f);
        float life = 0.0f;
        float maxLife = 1.0f;
        float size = 5.0f;
    } Particle;

    struct ParticleVertex
    {
        glm::vec2 position;
        glm::vec4 color;
        float size;
    };

    class ParticleSystem
    {
    public:
        ParticleSystem() = default;

        bool Initialize();
        void Update(float deltaTime);
        void Render(int viewportWidth, int viewportHeight, glm::vec2 offset = glm::vec2(0.0f));
        void Emit(const glm::vec2 &position, const glm::vec4 &color, float speed, int count);

        void SetMaxParticles(int maxParticles);
        void SetParticleSize(float size);
        void SetParticleIntensity(float intensity);

        std::function<void(const Particle &)> OnParticleSpawned;

    private:
        bool setupShaders();
        void setupBuffers();

        int mMaxParticles = 200;
        float mGlobalSize = 6.0f;
        float mGlobalIntensity = 1.0f;

        std::vector<Particle> mParticles;

        ShaderProgram mShaderProgram;
        VertexArray mVertexArray;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLES_H_
