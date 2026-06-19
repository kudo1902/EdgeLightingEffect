#ifndef _NEON_MULTI_PASS_RENDERER_H_
#define _NEON_MULTI_PASS_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/vertex-array.h"

namespace EdgeLighting
{
    class NeonMultiPassRenderer : public BaseRenderer
    {
    public:
        NeonMultiPassRenderer() = default;
        virtual ~NeonMultiPassRenderer();

        virtual bool Initialize() override;
        virtual void Update(float deltaTime, float time, const Config &config) override;
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;
        virtual void OnConfigChanged(const Config &config) override;

    private:
        bool setupShaders();
        void setupGeometry(const Config &config);
        void destroyFbo();
        void createFbo(int w, int h);

        Config mCurrentConfig;
        GLuint mGradientFbo = 0;
        GLuint mGradientTex = 0;
        int mFboW = 0, mFboH = 0;
        ShaderProgram mGradientShader;
        ShaderProgram mGlowShader;
        VertexArray mVertexArray;
    };
}

#endif
