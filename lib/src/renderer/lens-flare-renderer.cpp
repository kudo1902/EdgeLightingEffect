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
        /// Smallest scale that still yields a sane buffer. Mirrors the neon
        /// renderer's own floor.
        constexpr float MIN_FLARE_RESOLUTION_SCALE = 1.0e-3f;

        /// @c LensFlareConfig::resolutionScale clamped to (0, 1].
        ///
        /// Above 1.0 is refused rather than supersampled: the whole point of
        /// the knob is to shade FEWER fragments, and honouring 2.0 would
        /// quietly allocate a buffer four times the viewport. The retired
        /// half-res renderer read the field raw and did exactly that.
        inline float GetClampedFlareScale(const Config &config)
        {
            return std::clamp(config.lensFlare.resolutionScale, MIN_FLARE_RESOLUTION_SCALE, 1.0f);
        }
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

        // ONE schedule serves both resolution paths. `scaled` changes only
        // where the flare lands and whether the blit runs - not the order, not
        // the blend timeline, and not the uniforms, bar the two that say which
        // resolution is being drawn into.
        const float scale = GetClampedFlareScale(config);
        const bool scaled = (scale < 1.0f);
        const int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        const int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // The framebuffer this renderer was handed. Usually the window's
        // default one, but an offscreen frame capture (@ref OffscreenCapture)
        // binds a real FBO, so the blit has to come back to whatever was bound
        // rather than assuming 0. Read BEFORE the resize below.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        if (scaled)
        {
            // Resize destroys the attachment on its failure path, so a failure
            // leaves mScaledBuffer holding id 0 - and Bind() would then bind
            // the CALLER'S framebuffer, whereupon the glClear below erases
            // everything already drawn this frame (glClear is not clipped by
            // the viewport). Under an OffscreenCapture that target is the
            // capture. Nothing has been drawn or any state changed at this
            // point, so returning leaves the frame exactly as it was found.
            if (!mScaledBuffer.Resize(bufW, bufH))
            {
                return;
            }
            mScaledBuffer.Bind();

            // Clear colour is global GL state, so put it back: a host that
            // sets its own once at startup would otherwise find it silently
            // replaced with transparent black by whichever frame ran this.
            GLfloat prevClear[4];
            glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
        }

        // Premultiplied "over": alpha = brightest channel, so the flare
        // occludes cleanly under its cores and blends additively in the
        // low-brightness ghost wings. The same blend serves the draw into the
        // scaled buffer and the blit that composites it back.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        mFlareShader.Use();

        // The only two scale-dependent uniforms in the whole program. The
        // shader normalises everything by uResolution, so handing it the
        // buffer size reproduces the identical flare at lower resolution; the
        // sun rides the perimeter in full-res gl_FragCoord space, so it scales
        // into the buffer by the same factor. Both are identities at 1.0,
        // which is what keeps the direct path bit-identical to the
        // single-pass renderer this replaced.
        const glm::vec2 resolution(static_cast<float>(bufW), static_cast<float>(bufH));
        glm::vec2 sunPosFrag = GeometryUtils::GetSunFragPosition(config.lensFlare, config.geometry,
                                                                 viewportWidth, viewportHeight);
        sunPosFrag *= scale;

        mFlareShader.SetUniform("uResolution", resolution);
        mFlareShader.SetUniform("uSunPos", sunPosFrag);
        mFlareShader.SetUniform("uSunColor", config.lensFlare.color);
        mFlareShader.SetUniform("uIntensity", config.lensFlare.intensity);
        mFlareShader.SetUniform("uSpread", config.lensFlare.spread);
        mFlareShader.SetUniform("uGhostSpacing", config.lensFlare.ghostSpacing);
        mFlareShader.SetUniform("uGhostSize", config.lensFlare.ghostSize);
        mFlareShader.SetUniform("uGhostOffset", config.lensFlare.ghostOffset);
        mFlareShader.SetUniform("uGhostColor", config.lensFlare.ghostColor);
        mFlareShader.SetUniform("uGhostTint", config.lensFlare.ghostTint);
        mFlareShader.SetUniform("uFlareCenter", config.lensFlare.flareCenter);
        mFlareShader.SetUniform("uSize", config.lensFlare.size);

        constexpr float TWO_PI = 6.28318530717958647692f;
        mFlareShader.SetUniform("uRotation", time * config.lensFlare.rotationRate * TWO_PI);
        // Quantise the [0, 1] density into an integer slot count for the
        // shader; the shader needs an integer so `abs(sin(a * N/2))` closes
        // cleanly at 2 PI. MAX_RAY_SLOTS caps the top of the range so
        // extreme densities stay artist-friendly (rays too thin to see).
        constexpr int MAX_RAY_SLOTS = 80;
        float clampedDensity = std::clamp(config.lensFlare.rayDensity, 0.0f, 1.0f);
        int slots = std::max(1, static_cast<int>(std::round(clampedDensity * MAX_RAY_SLOTS)));
        mFlareShader.SetUniform("uRayDensity", static_cast<float>(slots));

        mVertexArray.DrawArrays(GL_TRIANGLES, 6);

        mFlareShader.Unuse();

        if (scaled)
        {
            // Back to the caller's target and its full-resolution viewport,
            // then composite. Bilinear upscaling of premultiplied alpha is
            // fringe-free; the blit shader is a plain texture read over
            // whatever is on the target already.
            Framebuffer::BindId(targetFbo);
            glViewport(0, 0, viewportWidth, viewportHeight);

            mBlitShader.Use();
            mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));
            mScaledBuffer.BindTexture(0);
            mBlitShader.SetUniform("uSource", 0);
            mVertexArray.DrawArrays(GL_TRIANGLES, 6);
            mBlitShader.Unuse();
        }

        // Restore the default alpha blend state for any following renderers -
        // the same hand-back every other renderer does. This one is registered
        // last in the demo, so the premultiplied mode it sets above used to
        // leak out of the effect entirely and land on whatever the host drew
        // next.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void LensFlareRenderer::OnConfigChanged(const Config &config)
    {
        mCurrentConfig = config;
    }

    bool LensFlareRenderer::setupShaders()
    {
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "LensFlareRenderer.Blit");
        mFlareShader = ShaderProgram(ShaderSource::LENS_FLARE_VERT_SRC,
                                     ShaderSource::LENS_FLARE_FRAG_SRC,
                                     "LensFlareRenderer.Flare");
        return mFlareShader.IsValid() && mBlitShader.IsValid();
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
