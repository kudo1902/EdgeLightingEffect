#ifndef _EDGE_LIGHTING_SEGMENT_RENDERER_H_
#define _EDGE_LIGHTING_SEGMENT_RENDERER_H_

#include "renderer/base-renderer.h"

namespace EdgeLighting
{
    class SegmentRenderer : public BaseRenderer
    {
    public:
        SegmentRenderer();
        virtual ~SegmentRenderer();

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float progress, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupQuadGeometry(const Config &config);

        Config mCurrentConfig;
        unsigned int mShaderProgram = 0;
        unsigned int mQuadVAO = 0;
        unsigned int mQuadVBO = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SEGMENT_RENDERER_H_
