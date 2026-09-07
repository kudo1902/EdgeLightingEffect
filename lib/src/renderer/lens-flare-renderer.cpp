#include "renderer/lens-flare-renderer.h"
#include "renderer/lens-flare-tuning.h"
#include "shaders.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace EdgeLighting
{
    namespace
    {
        /// Smallest scale that still yields a sane buffer. Mirrors the neon
        /// renderer's own floor.
        constexpr float MIN_FLARE_RESOLUTION_SCALE = 1.0e-3f;

        /// std140 binding point for GhostBlock. The neon renderer holds 0, 1
        /// and 2; binding points are context state rather than per-program, so
        /// this takes its own rather than sharing one with a renderer that may
        /// be enabled alongside it.
        constexpr GLuint GHOST_BLOCK_BINDING = 3;

        /// CPU mirror of the shader's GhostBlock. std140 gives a vec4 array a
        /// 16-byte stride, which is glm::vec4's own size, so the array maps
        /// one-to-one with no padding member needed.
        typedef struct GhostBlockData
        {
            glm::vec4 ghosts[FLARE_GHOST_COUNT];
        } GhostBlockData;

        static_assert(sizeof(GhostBlockData) == FLARE_GHOST_COUNT * 16,
                      "GhostBlockData must match the shader's std140 layout");

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

        /// Whether this config gives @c mScaledBuffer anything to do.
        ///
        /// ONE predicate for two questions that have to agree: @ref Render asks
        /// it to pick the path, and @ref OnConfigChanged asks it to decide
        /// whether the buffer may be freed. Answer them separately and they
        /// drift - the failure being a release of the buffer the very pass that
        /// needs it is about to bind, which Resize would then quietly rebuild
        /// once per frame. Same shape, same reasoning, as NeonRenderer's.
        ///
        /// Only @c enable is a genuine gate on the PASS - @ref Render returns
        /// on it before the scale is even clamped - so inside Render, past that
        /// return, this is exactly @c scale < 1.0.
        inline bool UsesScaledBuffer(const Config &config)
        {
            return config.lensFlare.enable && GetClampedFlareScale(config) < 1.0f;
        }

        /// Radius outside which a ghost's bloom term is exactly zero, for
        /// @c uBloomRadius. Derivation and shared constants live in
        /// lens-flare-tuning.h.
        ///
        /// A non-positive @p ghostSize leaves the shader's own pow exponent
        /// non-positive, where the term stops being decreasing in the radius
        /// and no bound exists. Returning the largest finite float keeps the
        /// gate permanently open there, so that degenerate case shades exactly
        /// as it did before the gate existed.
        inline float GetGhostBloomRadius(float ghostSize)
        {
            if (ghostSize <= 0.0f)
            {
                return std::numeric_limits<float>::max();
            }

            return std::pow(static_cast<float>(FLARE_BLOOM_CUT),
                            1.0f / (ghostSize * static_cast<float>(FLARE_BLOOM_EXP)));
        }

        /// Lower bound on what @c sin(l * 30) must clear before a ghost's ring
        /// term can be non-zero, for @c uRingFloor. Derivation in
        /// lens-flare-tuning.h.
        ///
        /// The max() matters twice. It keeps the base of this pow non-negative,
        /// and it covers the ghostSize below 2 * FLARE_RING_SHIFT where the
        /// SHADER's `l - FLARE_RING_SHIFT` can itself go negative - pow of a
        /// negative base is undefined in GLSL, and the demos' sliders start at
        /// 1.0 but the C ABI does not clamp the field. Clamped, the floor
        /// collapses to -FLARE_RING_BIAS, which every sin above that clears, so
        /// the gate simply stays open and nothing new is skipped.
        inline float GetGhostRingFloor(float ghostSize)
        {
            const float lMin = std::max(ghostSize * static_cast<float>(FLARE_RING_L_BIAS) -
                                            static_cast<float>(FLARE_RING_SHIFT),
                                        0.0f);

            return std::pow(lMin, static_cast<float>(FLARE_RING_EXP)) -
                   static_cast<float>(FLARE_RING_BIAS);
        }

        /// Bakes the table the shader reads as @c uGhosts: xyz is a ghost's
        /// final colour, w its distance along the sun axis.
        ///
        /// Both are pure functions of the ghost index and the config, so the
        /// shader was deriving the same ten values in every one of the
        /// viewport's fragments. Hoisting them is the same split the neon
        /// emission pre-pass is built on, one tier cheaper: one 160-byte
        /// std140 block, no texture and no pass. It also consumes ghostOffset,
        /// ghostColor and ghostTint entirely, which is why the shader no longer
        /// declares uniforms for them.
        ///
        /// WHY THIS SHIFTS THE LOOK SLIGHTLY, AND WHY THAT IS AN IMPROVEMENT
        ///   The distances come from the reference's `fract(sin(w) * 1000.0)`
        ///   hash, which is precision-chaotic. GLSL guarantees @c sin to only
        ///   about 2^-11, and multiplying by 1000 before taking the fractional
        ///   part turns that slack into a different value - so the hash never
        ///   had one answer, it had a per-driver answer. Computed here in
        ///   double the ten distances are the same on every GPU, which they
        ///   were not before; the price is that they no longer match what any
        ///   one GPU used to produce.
        ///
        ///   Measured against this machine's GPU they agree to about three
        ///   decimals, a sub-percent shift in ghost placement. Across the
        ///   demos' ghostSize range that lands as a max delta of 7 to 10 / 255
        ///   with under 0.02% of the frame past 8, on smooth gradients where it
        ///   is not visible. It is larger where a ghost's hex edge is sharpest:
        ///   at ghostSize 0 (degenerate, below what the sliders reach) the max
        ///   is 24 and 1.5% of the frame is past 8, because there the shift
        ///   moves a hard edge by a pixel rather than sliding a gradient.
        inline void BakeGhostTable(const LensFlareConfig &lensFlare, glm::vec4 *out)
        {
            /// Phase constants of the reference's procedural per-ghost palette,
            /// distance-modulated so no two ghosts read the same colour.
            constexpr float PALETTE_PHASE[3] = {0.44f, 0.24f, 0.2f};

            for (int i = 0; i < FLARE_GHOST_COUNT; i++)
            {
                const double scaled = std::sin(static_cast<double>(i) * 20.0) * 1000.0;
                const float hash = static_cast<float>(scaled - std::floor(scaled));
                const float dist = hash * 3.0f + 0.2f - 0.5f + lensFlare.ghostOffset;

                glm::vec3 color(0.0f);
                for (int k = 0; k < 3; k++)
                {
                    // GLSL mix(x, y, a) = x * (1 - a) + y * a, spelled out so
                    // the tint blend stays the shader's rather than glm's.
                    const float base = std::cos(PALETTE_PHASE[k] * 8.0f + dist * 4.0f) * 0.5f + 0.5f;
                    color[k] = base * (1.0f - lensFlare.ghostTint) +
                               lensFlare.ghostColor[k] * lensFlare.ghostTint;
                }

                out[i] = glm::vec4(color, dist);
            }
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
        // mCurrentFlare is whatever the last OnConfigChanged left - the effect
        // calls it on registration, so by here it is usually the host's real
        // config rather than the defaults. Either way the block is filled from
        // this point on, and OnConfigChanged re-bakes it on every change to
        // the three fields it reads.
        bakeGhostBlock(mCurrentFlare);
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
        // Past the enable return above, this is `scale < 1.0` - asked through
        // the shared predicate so it cannot disagree with the release gate in
        // OnConfigChanged about which configs want the buffer.
        const bool scaled = UsesScaledBuffer(config);
        const int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        const int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // The render target this renderer was handed - framebuffer AND
        // viewport, saved as a pair because the blit has to put both back. The
        // framebuffer is not always the window's: an offscreen frame capture
        // (@ref OffscreenCapture) binds a real FBO, so returning to 0 would
        // redirect the composite to the window. Read BEFORE the resize below.
        //
        // SCALED PATH ONLY, because it is the only one that retargets: on the
        // direct path the flare draws straight onto the caller's framebuffer
        // and there is nothing to come back to. Same shape, same reasoning, as
        // NeonRenderer::Render.
        RenderTargetState prevTarget;

        // SCALED PATH ONLY: mScaledBuffer is a reduced-size copy of the
        // viewport, so a host scissor box - in the CALLER's window coordinates
        // - lands on the wrong texels of it. The clear and the flare draw
        // would skip everything outside the box, and the blit would then read
        // the region the box maps DOWN to, which is a different region again
        // and one nothing wrote this frame. See GLUtils::NoScissorScope.
        //
        // The direct path takes none of this - `scaled` false short-circuits
        // even the query - because there the flare draws straight onto the
        // caller's framebuffer and the host's clip is exactly what it asked
        // for. Ended explicitly below rather than at the end of this function,
        // because unlike the neon renderer's the composite that has to see
        // that clip restored lives in this same scope.
        GLUtils::NoScissorScope noScissor(scaled);

        if (scaled)
        {
            prevTarget = RenderTargetState::Capture();

            // Resize destroys the attachment on its failure path, so a failure
            // leaves mScaledBuffer holding id 0 - and Bind would then bind the
            // CALLER'S framebuffer, with only Framebuffer::ClearBuffer's own
            // no-attachment guard standing between that and erasing everything
            // already drawn this frame (a clear is not clipped by the
            // viewport). Under an OffscreenCapture that target is the capture.
            // Do not lean on that guard - bail here. Nothing has been
            // drawn here, and the only state touched so far is the scissor
            // enable, which noScissor puts back as this return unwinds - so
            // returning leaves the frame exactly as it was found.
            if (!mScaledBuffer.Resize(bufW, bufH))
            {
                return;
            }
            // Bind, then clear to transparent black. Keep the two adjacent:
            // ClearBuffer acts on whatever is BOUND, so the bind is its
            // precondition rather than a nicety - see Framebuffer::ClearBuffer,
            // which also carries the reason the clear touches no context state
            // (it used to save, overwrite and restore GL_COLOR_CLEAR_VALUE
            // every frame on this path). The scissor guard above is the other
            // half of making this clear land where it is meant to.
            mScaledBuffer.Bind();
            mScaledBuffer.ClearBuffer();
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

        // ghostOffset / ghostColor / ghostTint reach the shader only through
        // this block, so there are no uniforms of their own to set. The bake
        // itself has moved to OnConfigChanged - it is a pure function of those
        // three fields, so running it here meant re-deriving the same table
        // every frame of every flare. Only the bind is per-frame, because the
        // binding point is global context state rather than something this
        // renderer can assume it still owns.
        mGhostBlock.BindBase(GHOST_BLOCK_BINDING);

        // Support bounds for the two compactly-supported ghost terms, so the
        // shader can skip each where it is provably zero. Both are pure
        // functions of ghostSize, which makes them exactly the kind of work
        // that belongs here rather than in every one of the viewport's
        // fragments - the same split the neon emission pre-pass is built on.
        mFlareShader.SetUniform("uBloomRadius", GetGhostBloomRadius(config.lensFlare.ghostSize));
        mFlareShader.SetUniform("uRingFloor", GetGhostRingFloor(config.lensFlare.ghostSize));
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
            // The host's clip comes back BEFORE the composite, not after: this
            // is the one draw here that lands on the caller's framebuffer in
            // the caller's coordinates, so it is the one draw the scissor box
            // describes correctly. Everything above it went into a buffer the
            // box does not address.
            noScissor.Restore();

            // Back to the caller's target and viewport, both at once, then
            // composite. Bilinear upscaling of premultiplied alpha is
            // fringe-free; the blit shader is a plain texture read over
            // whatever is on the target already.
            prevTarget.Restore();

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
        // Almost nothing to cache. Every value this renderer draws from is
        // read out of the Config that Render is handed, and the two support
        // bounds (uBloomRadius / uRingFloor) are genuinely cheap enough to
        // rebuild there - they are uniform setters, whose own last-value
        // caching skips the upload when they have not moved.
        //
        // The ghost table is the exception, and the difference is the
        // mechanism rather than the size: it reaches the shader through a UBO,
        // not a uniform, so no setter cache stood between a per-frame bake and
        // the buffer. (UniformBuffer::SetData compares bytes and does skip the
        // GL call, but only after the table has been rebuilt to compare.)
        // Baking it here instead reduces the per-frame cost to a bind.
        const bool ghostsDirty = !mGhostsBaked ||
                                 config.lensFlare.ghostOffset != mCurrentFlare.ghostOffset ||
                                 config.lensFlare.ghostColor != mCurrentFlare.ghostColor ||
                                 config.lensFlare.ghostTint != mCurrentFlare.ghostTint;

        mCurrentFlare = config.lensFlare;

        // Gated on the three fields BakeGhostTable reads, not on the whole
        // sub-config: rotationRate, spread and the rest move constantly under
        // an animation and change nothing in this table.
        if (ghostsDirty)
        {
            bakeGhostBlock(mCurrentFlare);
        }

        // Give the scaled buffer back the moment this config stops wanting it -
        // the layer switched off, or the scale returned to 1.0. It is by far
        // the largest thing this renderer owns: at 1920x1080 and scale 0.5 it
        // is 2.1 MB of colour attachment, against a 160-byte ghost block and
        // one fullscreen quad for everything else. Nothing freed it before, so
        // a host that ran the flare at a reduced scale and then turned it off
        // held that for the life of the effect.
        //
        // Cheap to get wrong in only one direction, and this is the safe one:
        // @ref Render re-Resizes before it binds, so a release of a buffer that
        // turns out to be wanted again costs one allocation on the next drawn
        // frame and nothing else. Resize's own early-out then keeps it
        // allocated for as long as the size and format hold.
        //
        // Here rather than in Render because Render must not be the thing that
        // deletes a framebuffer - see Framebuffer::Release on why the deletion
        // wants to be outside a pass. Pre-Initialize (AddRenderer calls this)
        // Release no-ops on the buffer it finds unallocated.
        if (!UsesScaledBuffer(config))
        {
            mScaledBuffer.Release();
        }
    }

    bool LensFlareRenderer::setupShaders()
    {
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "LensFlareRenderer.Blit");
        mFlareShader = ShaderProgram(ShaderSource::LENS_FLARE_VERT_SRC,
                                     ShaderSource::LENS_FLARE_FRAG_SRC,
                                     "LensFlareRenderer.Flare");
        if (!mFlareShader.IsValid() || !mBlitShader.IsValid())
        {
            return false;
        }

        mFlareShader.SetUniformBlockBinding("GhostBlock", GHOST_BLOCK_BINDING);
        return true;
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

    void LensFlareRenderer::bakeGhostBlock(const LensFlareConfig &flare)
    {
        GhostBlockData ghostBlock = {};
        BakeGhostTable(flare, ghostBlock.ghosts);
        mGhostBlock.SetData(&ghostBlock, sizeof(ghostBlock));
        mGhostsBaked = true;
    }

} // namespace EdgeLighting
