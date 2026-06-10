#ifndef EDGE_LIGHTING_PARTICLE_RENDERER_H
#define EDGE_LIGHTING_PARTICLE_RENDERER_H

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

        // BaseRenderer overrides
        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        void emitParticlesAtHead(float progress, float time, const Config &config);

        std::unique_ptr<ParticleSystem> particleSystem_;
    };

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_PARTICLE_RENDERER_H
