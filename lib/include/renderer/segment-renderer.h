#ifndef EDGE_LIGHTING_SEGMENT_RENDERER_H
#define EDGE_LIGHTING_SEGMENT_RENDERER_H

#include "renderer/base-renderer.h"

namespace EdgeLighting
{
    class SegmentRenderer : public BaseRenderer
    {
    public:
        SegmentRenderer();
        virtual ~SegmentRenderer();

        // BaseRenderer overrides
        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupQuadGeometry();

        unsigned int shaderProgram_ = 0;
        unsigned int quadVAO_ = 0;
        unsigned int quadVBO_ = 0;
    };

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_SEGMENT_RENDERER_H
