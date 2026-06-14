#ifndef _EDGE_LIGHTING_PARTICLE_RENDERER_H_
#define _EDGE_LIGHTING_PARTICLE_RENDERER_H_

#include "renderer/base-renderer.h"
#include "renderer/particles.h"
#include <memory>

namespace EdgeLighting
{
    class ParticleRenderer : public BaseRenderer
    {
    public:
        ParticleRenderer();
        virtual ~ParticleRenderer() = default;

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        static glm::vec3 getRainbowColor(float p);
        void emitParticlesAtHead(float progress, float time, const Config &config);

    private:
        std::unique_ptr<ParticleSystem> mParticleSystem;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_PARTICLE_RENDERER_H_
