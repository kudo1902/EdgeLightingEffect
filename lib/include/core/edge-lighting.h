#ifndef _EDGE_LIGHTING_EFFECT_H_
#define _EDGE_LIGHTING_EFFECT_H_

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

        bool Initialize();
        void Update(float deltaTime);
        void Render(int viewportWidth, int viewportHeight);

        void SetConfig(const Config &config);
        const Config &GetConfig() const;

        void AddRenderer(std::shared_ptr<BaseRenderer> renderer);
        Animation &GetAnimation();

    private:
        Config mConfig;
        float mTime = 0.0f;

        Animation mAnimation;
        std::vector<std::shared_ptr<BaseRenderer>> mRenderers;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EFFECT_H_
