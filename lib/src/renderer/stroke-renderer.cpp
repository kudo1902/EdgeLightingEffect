#include "renderer/stroke-renderer.h"
#include "shaders.h"
#include "util/log-util.h"
#include "util/geometry-utils.h"
#include "util/path-utils.h"
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace EdgeLighting
{
    bool StrokeRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link StrokeRenderer shaders.");
            return false;
        }
        setupGeometry(mCurrentConfig);
        uploadPathTexture(mCurrentConfig);
        return true;
    }

    void StrokeRenderer::Update(float, float, float, float, const Config &)
    {
    }

    void StrokeRenderer::Render(int viewportWidth, int viewportHeight, float, float headPos, float time, const Config &config)
    {
        if (!config.stroke.enable)
            return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        mShaderProgram.Use();

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        mShaderProgram.SetUniform("uMVP", mvp);
        mShaderProgram.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mShaderProgram.SetUniform("uBorderRadius", config.geometry.borderRadius);
        mShaderProgram.SetUniform("uStrokeThickness", config.stroke.thickness);
        mShaderProgram.SetUniform("uIntensity", config.stroke.intensity);
        mShaderProgram.SetUniform("uPrimaryColor", config.stroke.primaryColor);
        mShaderProgram.SetUniform("uStrokeAlignment", static_cast<int>(config.stroke.alignment));
        mShaderProgram.SetUniform("uFadeRange", config.stroke.fadeRange);
        mShaderProgram.SetUniform("uFadeMode", static_cast<int>(config.stroke.fadeMode));
        mShaderProgram.SetUniform("uStrokeAnimation", static_cast<int>(config.stroke.animation));
        mShaderProgram.SetUniform("uSegmentLength", config.stroke.segmentLength);
        mShaderProgram.SetUniform("uHeadPosition", headPos);
        mShaderProgram.SetUniform("uTime", time);
        mShaderProgram.SetUniform("uSpeed", config.stroke.speed);
        mShaderProgram.SetUniform("uSecondaryColor", config.stroke.secondaryColor);
        mShaderProgram.SetUniform("uColorMode", static_cast<int>(config.stroke.colorMode));
        mShaderProgram.SetUniform("uLineCount", config.stroke.lineCount);
        mShaderProgram.SetUniform("uWinding", static_cast<int>(config.geometry.winding));

        mShaderProgram.SetUniform("uPathSource", static_cast<int>(config.path.source));
        mShaderProgram.SetUniform("uPathPointCount", static_cast<int>(config.path.points.size()));
        mShaderProgram.SetUniform("uPathClosed", static_cast<int>(config.path.closed));

        {
            float totalLen = (config.path.source != PathSource::RECT && !config.path.points.empty())
                                 ? PathUtils::GetPathLength(config.path.points, config.path.closed)
                                 : 1.0f;
            mShaderProgram.SetUniform("uPathTotalLength", totalLen);

            mPathTexture.Bind(0);
            mShaderProgram.SetUniform("uPathTexture", 0);
        }

        if (config.stroke.glowEnable)
        {
            mShaderProgram.SetUniform("uGlowSize", config.stroke.glowSize);
            mShaderProgram.SetUniform("uIntensity", config.stroke.glowIntensity);
            mVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }

        mShaderProgram.SetUniform("uGlowSize", 0.0f);
        mShaderProgram.SetUniform("uIntensity", config.stroke.intensity);
        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void StrokeRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
        if (mShaderProgram.IsValid())
        {
            setupGeometry(config);
            uploadPathTexture(config);
        }
    }

    bool StrokeRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::STROKE_VERT_SRC, ShaderSource::STROKE_FRAG_SRC);
        return mShaderProgram.IsValid();
    }

    void StrokeRenderer::setupGeometry(const Config &config)
    {
        float margin = config.stroke.thickness + config.stroke.glowSize + 5.0f;
        float l, r, b, t;

        if (config.path.source != PathSource::RECT && !config.path.points.empty())
        {
            auto localPts = GeometryUtils::AppToLocal(config.path.points, config.geometry);
            glm::vec2 center, halfSize;
            PathUtils::GetPathAABB(localPts, center, halfSize);
            l = center.x - (halfSize.x + margin);
            r = center.x + (halfSize.x + margin);
            b = center.y - (halfSize.y + margin);
            t = center.y + (halfSize.y + margin);
        }
        else
        {
            float halfW = config.geometry.width * 0.5f;
            float halfH = config.geometry.height * 0.5f;
            l = -(halfW + margin);
            r = halfW + margin;
            b = -(halfH + margin);
            t = halfH + margin;
        }

        // clang-format off
        float verts[] = {
            l, t, l, b, r, b,
            l, t, r, b, r, t,
        };
        // clang-format on

        mVertexArray.SetVertexData(verts, sizeof(verts));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void StrokeRenderer::uploadPathTexture(const Config &config)
    {
        mPathTexture = Texture1D();

        if (config.path.source == PathSource::RECT || config.path.points.empty())
        {
            float zero[2] = {0.0f, 0.0f};
            mPathTexture.SetData(zero, 1);
        }
        else
        {
            auto localPts = GeometryUtils::AppToLocal(config.path.points, config.geometry);
            if (config.geometry.winding == Winding::CLOCKWISE)
            {
                std::reverse(localPts.begin(), localPts.end());
            }

            mPathTexture.SetData(reinterpret_cast<const float *>(localPts.data()),
                                 static_cast<GLsizei>(localPts.size()));
        }

        mPathTexture.SetParams();
    }

} // namespace EdgeLighting
