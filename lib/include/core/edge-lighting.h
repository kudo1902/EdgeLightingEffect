#ifndef EDGE_LIGHTING_EFFECT_H
#define EDGE_LIGHTING_EFFECT_H

#include "core/config.h"
#include "animation/animation.h"
#include "renderer/base-renderer.h"
#include <vector>
#include <memory>

namespace EdgeLighting
{

    class EdgeLightingEffect
    {
    public:
        EdgeLightingEffect();
        ~EdgeLightingEffect() = default;

        // Public API (PascalCase)
        bool Initialize();
        void Update(float deltaTime);
        void Render(int viewportWidth, int viewportHeight);

        void SetConfig(const Config &config);
        const Config &GetConfig() const;

        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);
        Animation &GetAnimation();

    private:
        Config config_;
        float time_ = 0.0f;

        Animation animation_;
        std::vector<std::shared_ptr<BaseRenderer>> renderers_;
    };

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_EFFECT_H
