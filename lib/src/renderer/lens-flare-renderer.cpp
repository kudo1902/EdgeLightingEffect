#include "renderer/lens-flare-renderer.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <cmath>

namespace EdgeLighting
{
    namespace
    {
        // Half-res factor for the FBO. Fixed 0.5 for now - roughly 4x fillrate
        // reduction. Not exposed as a config knob because at other ratios the
        // ghost radii start reading noticeably fuzzy.
        constexpr float HALF_RES_SCALE = 0.5f;

        // Max slots the density fraction maps to. Higher = finer needles;
        // 80 is what the demo slider ranges to.
        constexpr int MAX_RAY_SLOTS = 80;
    }

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

        // --- Half-res sizing ---
        int halfW = std::max(1, static_cast<int>(viewportWidth * HALF_RES_SCALE));
        int halfH = std::max(1, static_cast<int>(viewportHeight * HALF_RES_SCALE));
        mHalfResBuffer.Resize(halfW, halfH);

        // ==============================================================
        // Pass 1: draw the flare into the half-res FBO.
        // ==============================================================
        mHalfResBuffer.Bind();
        glViewport(0, 0, halfW, halfH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mShaderProgram.Use();

        // Sun on the rect perimeter: sample the shared helper, apply the outward
        // normal offset from perimeterOffset, then convert local (centre-origin,
        // y-up) to app pixel (top-left origin, y-down). Same as before, then
        // scaled to half-res just before upload.
        float t = config.lensFlare.perimeterPosition;
        glm::vec2 local = GeometryUtils::GetPointOnRectangle(t, config.geometry);

        constexpr float NORMAL_EPS = 1e-3f;
        float tPrev = t - NORMAL_EPS;
        float tNext = t + NORMAL_EPS;
        tPrev -= std::floor(tPrev);
        tNext -= std::floor(tNext);
        glm::vec2 pPrev = GeometryUtils::GetPointOnRectangle(tPrev, config.geometry);
        glm::vec2 pNext = GeometryUtils::GetPointOnRectangle(tNext, config.geometry);
        glm::vec2 tangent = pNext - pPrev;
        glm::vec2 normal(tangent.y, -tangent.x);
        float normalLen = std::sqrt(normal.x * normal.x + normal.y * normal.y);
        if (normalLen > 0.0f)
        {
            normal /= normalLen;
        }
        if (glm::dot(normal, local) < 0.0f)
        {
            normal = -normal;
        }
        local += normal * config.lensFlare.perimeterOffset;

        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::vec2 sunApp(config.geometry.position.x + local.x + halfRectW,
                         config.geometry.position.y + (halfRectH - local.y));

        // Convert pixel-space uniforms to half-res. gl_FragCoord in the FBO
        // is (0..halfW, 0..halfH); dimensionless uniforms below (spread /
        // size / density / intensity / rotation / colour) don't need scaling.
        glm::vec2 resolutionHalf(static_cast<float>(halfW), static_cast<float>(halfH));
        glm::vec2 sunPosFrag(sunApp.x * HALF_RES_SCALE,
                             resolutionHalf.y - sunApp.y * HALF_RES_SCALE);

        mShaderProgram.SetUniform("uResolution", resolutionHalf);
        mShaderProgram.SetUniform("uSunPos", sunPosFrag);
        mShaderProgram.SetUniform("uSunColor", config.lensFlare.color);
        mShaderProgram.SetUniform("uIntensity", config.lensFlare.intensity);
        mShaderProgram.SetUniform("uSpread", config.lensFlare.spread);
        mShaderProgram.SetUniform("uSize", config.lensFlare.size);

        constexpr float TWO_PI_F = 6.28318530717958647692f;
        mShaderProgram.SetUniform("uRotation", time * config.lensFlare.rotationRate * TWO_PI_F);

        // Density (0..1 fraction) -> integer slot count for the shader.
        float clampedDensity = std::clamp(config.lensFlare.rayDensity, 0.0f, 1.0f);
        int slots = std::max(1, static_cast<int>(std::round(clampedDensity * MAX_RAY_SLOTS)));
        mShaderProgram.SetUniform("uRayDensity", static_cast<float>(slots));

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mShaderProgram.Unuse();

        // ==============================================================
        // Pass 2: bilinear composite the FBO onto the main framebuffer.
        // Bilinear upscale of premultiplied colour + alpha is fringe-free.
        // ==============================================================
        Framebuffer::BindDefault();
        glViewport(0, 0, viewportWidth, viewportHeight);

        // Blend state carries through from pass 1 (still premultiplied over).
        mBlitShader.Use();
        mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));

        mHalfResBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlitShader.Unuse();
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
        // Reuse the neon vert + blit frag for the pass-2 composite.
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "LensFlare.Blit");
        return mShaderProgram.IsValid() && mBlitShader.IsValid();
    }

    void LensFlareRenderer::setupGeometry()
    {
        // Fullscreen NDC quad, used by BOTH the flare pass (drawn into the
        // half-res FBO) and the blit pass (drawn into the default fb). We
        // keep two VertexArrays because their attribute bindings live inside
        // the VAO state, but their vertex data is identical.
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mVertexArray.SetVertexData(ndc, sizeof(ndc));
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        mBlitVertexArray.SetVertexData(ndc, sizeof(ndc));
        mBlitVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

} // namespace EdgeLighting

/*
 * Ghost-array regenerator (drives the const vec2 GHOSTS[10] literal in
 * lens-flare.frag). Kept as a comment so anyone tweaking the ghost recipe
 * has a one-file generator to run; no build target needed.
 *
 *   #include <cmath>
 *   #include <cstdio>
 *   int main() {
 *       auto rnd = [](float w) {
 *           float x = std::sin(w) * 1000.0f;
 *           return x - std::floor(x);
 *       };
 *       for (int i = 0; i < 10; ++i) {
 *           float rs = rnd(i * 2000.0f) * 1.8f;
 *           float size = rs * rs + 1.41f;
 *           float dist = rnd(i * 20.0f) * 3.0f + 0.2f - 0.5f;
 *           std::printf("    vec2(%.6f, %.6f)%s\n", size, dist, i == 9 ? "" : ",");
 *       }
 *   }
 */
