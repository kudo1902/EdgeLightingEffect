#ifndef _EDGE_LIGHTING_BASE_RENDERER_H_
#define _EDGE_LIGHTING_BASE_RENDERER_H_

#include "core/config.h"

namespace EdgeLighting
{

    class BaseRenderer
    {
    public:
        BaseRenderer() = default;
        virtual ~BaseRenderer() = default;

        virtual bool Initialize() = 0;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) = 0;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) = 0;
        virtual void OnConfigChanged(const Config &config) = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_BASE_RENDERER_H_
