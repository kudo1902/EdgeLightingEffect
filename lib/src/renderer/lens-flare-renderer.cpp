#include "renderer/lens-flare-renderer.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    bool LensFlareRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link LensFlareRenderer shaders.");
            return false;
        }
        setupGeometry();
        return true;
    }

    void LensFlareRenderer::Update(float, float, const Config &)
    {
    }

    void LensFlareRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.lensFlare.enable)
        {
            return;
        }

        // Premultiplied "over": alpha = brightest channel, so the flare
        // occludes cleanly under its cores and blends additively in the
        // low-brightness ghost wings.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();

        // The sun rides the rect perimeter: sample the shared helper (same
        // parameter space as segments/arcs), convert local -> app pixel space
        // (rect top-left origin, y-down), then flip Y for gl_FragCoord.
        float t = config.lensFlare.perimeterPosition;
        glm::vec2 local = GeometryUtils::GetPointOnRectangle(t, config.geometry);

        // Apply signed offset along the edge outward normal. Estimate the
        // tangent via a central finite difference in local space (y-up), then
        // pick the perpendicular that points away from the centre - the rect
        // is convex, so the outward normal has a positive dot with the local
        // point position.
        constexpr float NORMAL_EPS = 1e-3f;
        float tPrev = t - NORMAL_EPS;
        float tNext = t + NORMAL_EPS;
        tPrev -= floorf(tPrev);
        tNext -= floorf(tNext);
        glm::vec2 pPrev = GeometryUtils::GetPointOnRectangle(tPrev, config.geometry);
        glm::vec2 pNext = GeometryUtils::GetPointOnRectangle(tNext, config.geometry);
        glm::vec2 tangent = pNext - pPrev;
        glm::vec2 normal(tangent.y, -tangent.x);
        float normalLen = sqrtf(normal.x * normal.x + normal.y * normal.y);
        if (normalLen > 0.0f)
        {
            normal /= normalLen;
        }
        if (glm::dot(normal, local) < 0.0f)
        {
            normal = -normal;
        }
        local += normal * config.lensFlare.perimeterOffset;

        float halfW = config.geometry.width * 0.5f;
        float halfH = config.geometry.height * 0.5f;
        glm::vec2 sunApp(config.geometry.position.x + local.x + halfW,
                         config.geometry.position.y + (halfH - local.y));

        glm::vec2 resolution(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        glm::vec2 sunPosFrag(sunApp.x, resolution.y - sunApp.y);

        mShaderProgram.SetUniform("uResolution", resolution);
        mShaderProgram.SetUniform("uSunPos", sunPosFrag);
        mShaderProgram.SetUniform("uSunColor", config.lensFlare.color);
        mShaderProgram.SetUniform("uIntensity", config.lensFlare.intensity);
        mShaderProgram.SetUniform("uSpread", config.lensFlare.spread);
        mShaderProgram.SetUniform("uGhostSpacing", config.lensFlare.ghostSpacing);
        mShaderProgram.SetUniform("uGhostSize", config.lensFlare.ghostSize);
        mShaderProgram.SetUniform("uGhostOffset", config.lensFlare.ghostOffset);
        mShaderProgram.SetUniform("uGhostColor", config.lensFlare.ghostColor);
        mShaderProgram.SetUniform("uGhostTint", config.lensFlare.ghostTint);
        mShaderProgram.SetUniform("uSize", config.lensFlare.size);

        constexpr float TWO_PI = 6.28318530717958647692f;
        mShaderProgram.SetUniform("uRotation", time * config.lensFlare.rotationRate * TWO_PI);
        // Quantise the [0, 1] density into an integer slot count for the
        // shader; the shader needs an integer so `abs(sin(a * N/2))` closes
        // cleanly at 2 PI. MAX_RAY_SLOTS caps the top of the range so
        // extreme densities stay artist-friendly (rays too thin to see).
        constexpr int MAX_RAY_SLOTS = 80;
        float clampedDensity = std::clamp(config.lensFlare.rayDensity, 0.0f, 1.0f);
        int slots = std::max(1, static_cast<int>(std::round(clampedDensity * MAX_RAY_SLOTS)));
        mShaderProgram.SetUniform("uRayDensity", static_cast<float>(slots));

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mShaderProgram.Unuse();
    }

    void LensFlareRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
    }

    bool LensFlareRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::LENS_FLARE_VERT_SRC,
                                       ShaderSource::LENS_FLARE_FRAG_SRC,
                                       "LensFlareRenderer");
        return mShaderProgram.IsValid();
    }

    void LensFlareRenderer::setupGeometry()
    {
        // Static fullscreen NDC quad; shader shapes everything from
        // gl_FragCoord + uniforms, so the geometry never needs a rebuild.
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mVertexArray.SetVertexData(ndc, sizeof(ndc));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

} // namespace EdgeLighting
