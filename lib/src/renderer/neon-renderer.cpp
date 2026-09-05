#include "renderer/neon-renderer.h"
#include "renderer/neon-tuning.h"
#include "util/geometry-utils.h"
#include "util/segment-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdint>

namespace EdgeLighting
{
    namespace
    {
        /// Pixel distance the shader should treat as the cutoff boundary.
        /// Disabled cutoffs collapse to a huge sentinel so the shader's
        /// smoothstep / discard math naturally no-ops on realistic geometry;
        /// only the CPU knows this number, shaders see it as a plain uniform.
        constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;
        inline float GetCutoffSize(const Cutoff &c)
        {
            return c.enable ? c.size : CUTOFF_DISABLED_SIZE;
        }

        /// Does this config's opaque fill cover EVERY pixel at coverage 1?
        ///
        /// The question the fill passes actually need answered is not "which
        /// OpaqueMode did the host pick" but "is the coverage uniformly 1",
        /// because that is what decides between a scissored glClear and a
        /// shaded draw. ALL says so by definition. BOTH says so too whenever
        /// NEITHER cutoff is enabled - and since both cutoffs default to
        /// disabled (@ref NeonConfig::insideCutoff / outsideCutoff), that is
        /// the state a host lands in by simply selecting BOTH.
        ///
        /// Trace it: a disabled cutoff arrives as CUTOFF_DISABLED_SIZE, so in
        /// black-rect.frag dIn = d + 1e6 is hugely positive and dOut = d - 1e6
        /// hugely negative, both smoothsteps saturate, and coverage is exactly
        /// 1 at every fragment. @ref setupFillGeometry independently
        /// degenerates its ring to a solid +-1e6 quad in the same case (the
        /// hole collapses to zero extent). The two together were drawing the
        /// whole viewport through the shader to write what a clear writes -
        /// measured at ~0.4 ms per frame at 3840x2160, the exact cost the ALL
        /// clear path exists to avoid.
        ///
        /// Deliberately NOT normalised into the stored config: the C ABI
        /// getters round-trip @c opaqueMode, so a host that sets BOTH must
        /// read BOTH back. This is a render-time question, asked at both the
        /// geometry build and the draw so the two cannot disagree.
        ///
        /// The partial cases stay on the shader path, and must: BOTH with one
        /// cutoff enabled is bounded on that side, OUTSIDE with its cutoff
        /// disabled covers the viewport MINUS the rect interior, and INSIDE
        /// with its cutoff disabled covers only the interior. None of those is
        /// a clear.
        inline bool FillsWholeViewport(const NeonConfig &neon)
        {
            return neon.opaqueMode == OpaqueMode::ALL ||
                   (neon.opaqueMode == OpaqueMode::BOTH &&
                    !neon.insideCutoff.enable && !neon.outsideCutoff.enable);
        }

        /// Would a glClear land on the same pixels a coverage-1 fullscreen
        /// draw would, given the CURRENT GL state?
        ///
        /// A clear is not a draw, and the difference is entirely in what CLIPS
        /// it. Scissor and colour mask apply to both, which is what makes the
        /// substitution work at all. The DEPTH and STENCIL tests apply only to
        /// the draw - a clear is defined to ignore them. So a host masking the
        /// effect through a stencil buffer (a rounded window, a cut-out, a
        /// portal) had that honoured by the fullscreen quad and would find a
        /// clear painting straight through it.
        ///
        /// Queried rather than assumed because this renderer never touches
        /// either test: whatever they hold is the host's, and the host is
        /// exactly who would be relying on them.
        ///
        /// @note Blending is NOT part of this question even though a clear
        ///       ignores it too - @ref NeonRenderer::Render owns the blend
        ///       mode for the phase and sets premultiplied-over immediately
        ///       above, under which a coverage-1 source composites to itself.
        inline bool ClearClipsLikeDraw()
        {
            return !glIsEnabled(GL_STENCIL_TEST) && !glIsEnabled(GL_DEPTH_TEST);
        }

        /// CPU-side mirror of neon.frag's std140 `SegmentBlock`: the int is
        /// padded to 16 bytes and each vec3 element to a vec4 stride.
        typedef struct SegmentBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 segments[MAX_SEGMENT_BOOSTS];
        } SegmentBlockData;

        static_assert(sizeof(SegmentBlockData) == 16 + 16 * MAX_SEGMENT_BOOSTS,
                      "SegmentBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon.frag's std140 `LoopSamplesBlock`. std140
        /// pads each vec2 to a 16-byte stride, so we store as vec4 and the
        /// shader reads .xy. Sized by NEON_MAX_LOOP_SAMPLES (neon-tuning.h),
        /// which also sizes the shader's uLoopSamples array.
        typedef struct LoopSamplesBlockData
        {
            glm::vec4 samples[NEON_MAX_LOOP_SAMPLES];
        } LoopSamplesBlockData;

        static_assert(sizeof(LoopSamplesBlockData) == 16 * NEON_MAX_LOOP_SAMPLES,
                      "LoopSamplesBlockData must match the shader's std140 layout");

        /// CPU-side mirror of neon.frag's std140 `ArcBlock`. Same layout
        /// pattern as SegmentBlockData: int padded to 16 bytes, then a vec4
        /// per array element (start, length, intensity, hasStops).
        typedef struct ArcBlockData
        {
            int32_t count;
            float pad[3];
            glm::vec4 arcs[MAX_ARCS];
        } ArcBlockData;

        static_assert(sizeof(ArcBlockData) == 16 + 16 * MAX_ARCS,
                      "ArcBlockData must match the shader's std140 layout");

        constexpr GLuint SEGMENT_BLOCK_BINDING = 0;
        constexpr GLuint LOOP_SAMPLES_BLOCK_BINDING = 1;
        constexpr GLuint ARC_BLOCK_BINDING = 2;

        /// One candidate texture format for the emission table.
        typedef struct EmissionFormat
        {
            GLint internalFormat;
            GLenum format;
            GLenum type;
            const char *name; ///< For the fallback log line.
        } EmissionFormat;

        /// Emission-table formats in PREFERENCE ORDER, best first.
        ///
        /// RGBA16F leads because row 0 carries Arc::intensity and row 1 sums
        /// stacked SegmentBoost::boost values, both of which exceed 1.0 in
        /// ordinary use. GLES 3.0 exposes float colour-renderability only
        /// through an extension, so RGBA8 follows for drivers that refuse it -
        /// the picture is otherwise identical, but highlights above 1.0 clamp.
        ///
        /// Adding a candidate is adding a row; @ref NeonRenderer::resizeEmissionBuffer
        /// walks whatever is here.
        ///
        /// WHY ONLY THIS BUFFER HAS A LIST. It is the only one that asks for a
        /// format a conforming driver may refuse. RGBA8 - what mScaledBuffer,
        /// LensFlareRenderer's scaled buffer and OffscreenCapture all
        /// take - is mandatory colour-renderable in both GL 3.3 core and GLES
        /// 3.0, so there is nothing for those to fall back FROM, and nothing to
        /// fall back TO either: an RGBA8 failure is out-of-memory or a broken
        /// driver, which no other format fixes. They bail instead, and should.
        ///
        /// This buffer is also the only one that WANTS float: it stores arc
        /// intensity and stacked segment boosts, which routinely exceed 1.0.
        /// The others store composited output - premultiplied colour after
        /// tone-mapping, in [0, 1] - where 8 bits is the right storage rather
        /// than a compromise. (8 bits is not free there; see R7 in
        /// docs/review-findings.md on halo/bloom contour banding. If that is
        /// ever fixed with a float target rather than a dither, this walk
        /// generalises to those buffers unchanged.)
        constexpr EmissionFormat EMISSION_FORMATS[] = {
            {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, "RGBA16F"},
            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "RGBA8"},
        };

        /// GL_NEAREST because the consumer reads the table with texelFetch:
        /// adjacent texels are unrelated perimeter samples (and the two rows
        /// are different quantities entirely), so filtering across them is
        /// meaningless.
        constexpr GLint EMISSION_FILTER = GL_NEAREST;

        /// Smallest resolution scale the passes will honour. Not a taste
        /// judgement about how blurry is too blurry - it keeps the scaled
        /// buffer at least a pixel in each axis and keeps the shader's
        /// `constant * uResolutionScale` conversions away from zero, where the
        /// feather and gate divisions would blow up.
        constexpr float MIN_RESOLUTION_SCALE = 1.0e-3f;

        /// Width of each segment's row in the segment gradient atlas. Half
        /// the base LUT is enough - a segment's visible span is short so
        /// higher resolution wouldn't be visible; segments also don't wrap
        /// (CLAMP on X), so the extra texels would only pad head/tail.
        constexpr int SEGMENT_LUT_WIDTH = 128;
        /// Width of each arc's row in the arc gradient atlas. Same rationale
        /// as SEGMENT_LUT_WIDTH: an arc's LUT is sampled over the perimeter
        /// hue coordinate (uTime * rate) which cycles slowly, so 128 texels
        /// look identical to 256.
        constexpr int ARC_LUT_WIDTH = 128;

        /// Packs @c uArcs[].w for arc @p i: bit 0 = the arc has its own colour
        /// stops, bit 1 = another arc covers the perimeter immediately BEFORE
        /// its start, bit 2 = another arc covers it immediately AFTER its end.
        ///
        /// The abutment bits choose each endpoint's feather direction in the
        /// shaders' @c arcCoverContinuous, and getting that per-endpoint is what
        /// lets two arcs tile the ring without a seam notch while a lone arc
        /// still lights nothing outside its own span. Both properties matter:
        /// see the long note in neon.frag, and in particular why a symmetric
        /// feather cannot satisfy both at @c cornerRadius 0.
        ///
        /// It is a pure function of the arc set, so it is resolved here, once
        /// per frame, rather than by an O(arcs^2) scan in every fragment.
        ///
        /// @p arcs is the list as packed - only the first @p count entries are
        /// visible to the shader, so only they can abut.
        inline float PackArcFlags(const std::vector<Arc> &arcs, int i, int count)
        {
            // Perimeter-fraction slop. Endpoints that are meant to coincide are
            // usually authored as exact values or driven by an animation, so
            // this only has to absorb float round-trip error.
            constexpr float EPS = 1e-5f;
            const Arc &a = arcs[i];
            float flags = a.colorStops.empty() ? 0.0f : 1.0f;

            float end = a.start + a.length;
            bool tailAbuts = false;
            bool headAbuts = false;
            for (int b = 0; b < count; ++b)
            {
                if (b == i)
                {
                    continue;
                }
                const Arc &o = arcs[b];
                // A dark arc is skipped by the shader's coverage loop, so it
                // cannot take over a neighbour's endpoint either.
                if (o.length <= 0.0f || o.intensity <= 0.0f)
                {
                    continue;
                }
                // Where a.start falls within o, measured forward from o.start.
                float rTail = a.start - o.start;
                rTail -= std::floor(rTail);
                // Covers strictly BEFORE a.start: rTail must be past o's start
                // (rTail > 0 excludes two arcs that merely share a start point)
                // and no further than its end.
                if (rTail > EPS && rTail <= o.length + EPS)
                {
                    tailAbuts = true;
                }
                // Where a's end falls within o. Covers strictly AFTER it when
                // the end lands inside o but not exactly on o's own end - an
                // arc finishing where this one finishes extends nothing.
                float rHead = end - o.start;
                rHead -= std::floor(rHead);
                if (rHead < o.length - EPS)
                {
                    headAbuts = true;
                }
            }
            if (tailAbuts)
            {
                flags += 2.0f;
            }
            if (headAbuts)
            {
                flags += 4.0f;
            }
            return flags;
        }

        /// @c NeonConfig::resolutionScale, clamped to the range the passes can
        /// actually honour. Read through this everywhere rather than off the
        /// config: a zero or negative scale would give a zero-size buffer and a
        /// division by zero in the shader's constant conversions.
        ///
        /// Above 1.0 is refused rather than supersampled: the whole point of
        /// the knob is to draw FEWER fragments, and honouring 2.0 would quietly
        /// allocate a buffer four times the viewport.
        inline float GetClampedResolutionScale(const Config &config)
        {
            return std::clamp(config.neon.resolutionScale, MIN_RESOLUTION_SCALE, 1.0f);
        }

        /// @c NeonConfig::numSamples clamped to [1, NEON_MAX_LOOP_SAMPLES] -
        /// the UBO and the shader array are sized by that ceiling, and a count
        /// of zero would leave the gather with nothing to normalise by.
        ///
        /// The emission pre-pass and the gather MUST be handed the same value,
        /// which is the reason this is one function and not two clamps at the
        /// two call sites: texel i in the table has to be sample i in the loop.
        inline int GetClampedNumSamples(const Config &config)
        {
            return std::clamp(config.neon.numSamples, 1, int(NEON_MAX_LOOP_SAMPLES));
        }

        /// Warn when a host hands over more arcs / segments than the shader
        /// arrays can hold. The excess is dropped silently otherwise: the UBOs
        /// are fixed-size (@c MAX_ARCS / @c MAX_SEGMENT_BOOSTS, shared with the
        /// GLSL array declarations), and everything past the cap never reaches
        /// the GPU. The demo's UI enforces the caps so it never sees this, but
        /// a library or C-ABI host gets no other signal.
        ///
        /// Fires on the TRANSITION into overflow - @p prev at or under the cap,
        /// @p now above it - which is what keeps it to one line per overflow
        /// without a latch to store. The counts are already kept: the arcs in
        /// @c mCurrentConfig, the segments in @c mEffectiveSegments, both read
        /// before @ref NeonRenderer::OnConfigChanged overwrites them. Dropping
        /// back under the cap and overflowing again warns again, because the
        /// transition happens again.
        ///
        /// Called from OnConfigChanged rather than per frame, which is also the
        /// only place either count can change.
        inline void WarnOnOverflow(const char *what, size_t prev, size_t now, int cap)
        {
            if (static_cast<int>(now) > cap && static_cast<int>(prev) <= cap)
            {
                LOG_E("NeonRenderer: %zu %s configured but only %d fit - the rest are ignored.",
                      now, what, cap);
            }
        }
    }

    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------

    bool NeonRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link NeonRenderer shaders.");
            return false;
        }
        // Allocated ONCE, here, and never touched again: the emission table's
        // dimensions are compile-time constants, so unlike every other buffer
        // in the renderer it has no reason to be revisited per frame. Its
        // format is settled here too - see resizeEmissionBuffer.
        if (!resizeEmissionBuffer())
        {
            LOG_E("Failed to allocate the NeonRenderer emission table in any supported format.");
            return false;
        }
        // Vertex FORMAT for the two arrays whose contents are rebuilt at
        // runtime, declared once here rather than on every rebuild.
        //
        // A VAO remembers both the attribute format and the buffer it reads
        // from, and neither ever changes for these two: the layout is a fixed
        // vec2, and the VBO ids are fixed for the life of the renderer
        // (VertexArray is move-only and both are members, never reassigned).
        // So re-declaring it alongside each upload was four redundant GL calls
        // - bind VAO, bind VBO, enable array, attrib pointer - describing state
        // that had not moved. glVertexAttribPointer only records the binding;
        // it does not need the store to exist yet, which is why this can run
        // before the uploads below.
        //
        // @ref setupFullscreenQuad keeps its own paired call: that one uploads
        // exactly once and never returns, so there is nothing to separate.
        mGlowVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        mFillVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        setupFillGeometry(mCurrentConfig);
        // The atlas bakes read the merged transient+preserved view, which
        // OnConfigChanged normally keeps current; seed it here for the first.
        SegmentUtils::FillEffectiveSegments(mCurrentConfig.neon, mEffectiveSegments);
        bakeLUTs(mCurrentConfig);

        setupFullscreenQuad();
        return true;
    }

    void NeonRenderer::Update(float deltaTime, float, const Config &)
    {
        // A fade frame re-uploads the ring the emission table is baked FROM,
        // and does it without any config change for OnConfigChanged to catch -
        // so the table has to be invalidated from here or it would hold the
        // ring's colours from the frame the fade began for the whole fade.
        // |=, not =: a config change earlier in this same frame must not be
        // cleared by a settled ring reporting false.
        mEmissionDirty = mGradientLUT.Tick(deltaTime) || mEmissionDirty;
    }

    void NeonRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.neon.enable)
        {
            return;
        }

        // Render is a pass schedule and nothing else: derive the transform,
        // then one call per pass. Each pass owns its own shader and, where it
        // retargets, its own framebuffer restore. Blend state is owned HERE.
        //
        // ONE schedule serves both resolution paths. `scaled` changes only
        // where the gather lands and whether the blit runs - not the order,
        // not the blend timeline, and not the guards. The fill is what makes
        // that possible: it draws full-res on the caller's framebuffer, which
        // is independent of the buffer the gather draws into, so fill-first is
        // correct whether the glow arrives directly (it composites over the
        // fill) or through the blit (which composites over it later).
        const float scale = GetClampedResolutionScale(config);
        const bool scaled = (scale < 1.0f);
        const int bufW = std::max(static_cast<int>(static_cast<float>(viewportWidth) * scale), 1);
        const int bufH = std::max(static_cast<int>(static_cast<float>(viewportHeight) * scale), 1);

        // The transform is derived in SCALED space, so the gather quad, the
        // rect size and the loop samples all agree. At scale 1.0 this IS the
        // full-res transform - bufW/bufH are the viewport and the centre is
        // unmoved - which is what makes the direct path identical to the
        // dedicated full-res renderer this class replaced.
        //
        // Viewport y runs down in Config but up in the projection, so the
        // centre is mirrored about the viewport height BEFORE scaling.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        // The extent is the EXACT scaled viewport, NOT the truncated buffer
        // size, and the difference is a global stretch rather than a rounding
        // detail. Trace the round trip: the gather places world x at scaled
        // coordinate x * scale, this ortho maps [0, extent] onto a viewport
        // bufW wide, and @ref renderBlitPass then maps the WHOLE buffer back
        // across the full-width viewport with a fullscreen quad. Composed,
        // that scales by (viewport * scale) / extent. Feeding it bufW makes
        // the factor viewport * scale / floor(viewport * scale) - greater than
        // 1 whenever the product is not an integer - so everything is pushed
        // outward from the viewport origin by an amount that GROWS with
        // distance from it. Measured on a 1234 px viewport at scale 0.7, the
        // outside-cutoff boundary landed 0.9 px past its stated size, and the
        // rect drifted with it. On 1280x720 every common scale divides
        // exactly, which is why this hid for so long.
        //
        // extent = viewport * scale makes that factor exactly 1 whatever the
        // truncation does. bufW/bufH stay the buffer's real size, so the
        // buffer still covers the viewport corner to corner; the gather just
        // samples at bufW / viewport rather than at `scale`, a sub-0.2%
        // density difference that nothing here is measured against. At scale
        // 1.0 the extent IS bufW and the direct path does not move.
        //
        // The max() only guards a zero viewport from reaching glm::ortho as an
        // empty range - bufW's own max(1) used to cover that.
        const glm::mat4 proj = glm::ortho(0.0f, std::max(static_cast<float>(viewportWidth) * scale, 1.0e-3f),
                                          0.0f, std::max(static_cast<float>(viewportHeight) * scale, 1.0e-3f),
                                          -1.0f, 1.0f);
        const glm::vec2 center((config.geometry.position.x + halfRectW) * scale,
                               (static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH) * scale);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));

        // The framebuffer this renderer was handed. Usually the window's
        // default one, but an offscreen frame capture (@ref OffscreenCapture)
        // binds a real FBO, so every pass that retargets has to come back to
        // whatever was bound rather than assuming 0. Read BEFORE any pass binds
        // one of its own - querying later would capture that instead.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a). Used
        // for the opaque black fill, the neon and the blit, so each composites
        // cleanly over the last. (Blending stays ON across them - toggling
        // GL_BLEND mid-draw is a common cross-driver footgun on mobile GLES.)
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 2a: opaque-mode background fill ---------------------------
        // Numbered 2a because it belongs to the full-res phase with the blit,
        // not because it runs late: it has no dependency on the passes below
        // and every one of them is allowed to skip.
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(viewportWidth, viewportHeight, config);
        }

        // Debug: stop after the fill, on both paths. What lands on screen is
        // the opaque silhouette by itself - which is how the fill's square
        // corner at cornerRadius 0 gets compared against the emission's round
        // one. DebugRenderer honours the same flag, so the overlays do not
        // reappear over a fill-only frame.
        //
        // The one field this renderer reads out of DebugConfig. It lives there
        // because it is a debug control, and it is read HERE because it is a
        // mode of this renderer's pass schedule rather than something another
        // layer can draw - no other renderer can decline to run these passes.
        // Restores the blend state the tail of this function would have set.
        if (config.debug.opaqueOnly)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            return;
        }

        // --- Pass 0: per-sample emission table ------------------------------
        // Deliberately AFTER the opaqueOnly return: the table feeds only the
        // gather below, so the debug fill-only mode must not pay for a UBO
        // upload plus a draw it never samples. Safe to retarget the framebuffer
        // here - the fill has already landed, and this pass restores the target
        // it was handed.
        packLightBlocks(config);
        // ...and only re-bake the table when something it reads has actually
        // moved. The buffer is allocated once and nothing else writes it, so a
        // frame that changes neither the config nor (at a non-zero hue rate)
        // the time reads the same texels the last bake left. A still ring
        // therefore costs one FBO bind, eight uniform sets, three texture binds
        // and a draw on the frame it changes, and nothing on the frames after.
        if (isEmissionTableStale(time, config))
        {
            // A table write is not a composite: blending would mix this frame's
            // emission into last frame's. Pass 1 below re-asserts the blend
            // mode unconditionally, so leaving this alone on the skip path
            // changes nothing downstream.
            glDisable(GL_BLEND);
            renderEmissionPass(viewportWidth, viewportHeight, time, config);
        }

        // --- Pass 1: the neon gather ----------------------------------------
        // Re-assert the phase mode: pass 0 leaves blending disabled. Setting it
        // immediately before the phase that needs it (rather than relying on
        // the carry-over from above) is what makes the pass order safe to
        // change without silently breaking compositing.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        // Only the SCALED path can still fail here, on its buffer allocation;
        // the emission table was secured at Initialize. A failure skips the
        // blit below with us, so the frame degrades to the opaque fill rather
        // than compositing a stale buffer.
        const bool glowReady = renderNeonPass(mvp, bufW, bufH, scaled, time, config);

        // --- Pass 2b: composite the scaled gather ---------------------------
        // Only the scaled path has anything to composite; on the direct path
        // pass 1 already drew onto the target. Skipped along with the gather
        // when either allocation failed, so a failed frame degrades to the
        // fill rather than blitting a stale buffer from an earlier frame.
        if (scaled)
        {
            // Back to the caller's target and its full-resolution viewport.
            // Unconditional: pass 1 binds the scaled buffer before it can fail
            // at Resize, and leaving the caller on our buffer would silently
            // redirect every renderer after this one.
            Framebuffer::BindId(targetFbo);
            glViewport(0, 0, viewportWidth, viewportHeight);
            if (glowReady)
            {
                renderBlitPass();
            }
        }

        // Restore a known blend state for following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        // Snapshot dirtiness before we overwrite mCurrentConfig. Each rebuild
        // is gated on the exact set of fields it reads (see the corresponding
        // methods below) - dragging a slider like `bloomStrength` used to
        // re-upload the whole LUT and loop-samples UBO every frame; now only
        // the geometry quad refreshes.
        //
        // resolutionScale and numSamples join geometry on the sample walk:
        // the samples are stored pre-scaled, and only the first numSamples of
        // them are filled. Both therefore reach the quad as well, through
        // samplesDirty.
        const bool samplesDirty = config.geometry != mCurrentConfig.geometry ||
                                  config.neon.resolutionScale != mCurrentConfig.neon.resolutionScale ||
                                  config.neon.numSamples != mCurrentConfig.neon.numSamples;
        const bool geometryDirty = samplesDirty ||
                                   config.neon.glowRadius != mCurrentConfig.neon.glowRadius ||
                                   config.neon.bloomStrength != mCurrentConfig.neon.bloomStrength ||
                                   config.neon.intensity != mCurrentConfig.neon.intensity ||
                                   // lineWidth feeds setupGeometry's filament-reach floor, which is
                                   // what sizes the quad whenever glowRadius is small. Miss it and a
                                   // widened filament keeps the old, tighter margin and gets clipped
                                   // on the OUTSIDE only (the quad bounds the exterior; the interior
                                   // is always covered) - the "outer glow dropped at glowRadius 0"
                                   // report. Only bites at small glowRadius, because above
                                   // lineWidth / 10.4 the glow term wins the max() anyway.
                                   config.neon.lineWidth != mCurrentConfig.neon.lineWidth ||
                                   // filamentFalloff sets how many sigmas the filament
                                   // reaches, so it sizes the quad too (see setupGeometry).
                                   config.neon.filamentFalloff != mCurrentConfig.neon.filamentFalloff ||
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        // The fill ring is bounded by the CUTOFFS and the fill's own feather,
        // not by the glow reach, so it gets its own gate rather than riding on
        // geometryDirty: insideCutoff, opaqueMode and opaqueSoftness move the
        // ring but not the glow quad, and glowRadius / bloomStrength move the
        // glow quad but not the ring.
        const bool fillDirty = config.geometry != mCurrentConfig.geometry ||
                               config.neon.opaqueMode != mCurrentConfig.neon.opaqueMode ||
                               config.neon.opaqueSoftness != mCurrentConfig.neon.opaqueSoftness ||
                               config.neon.insideCutoff != mCurrentConfig.neon.insideCutoff ||
                               config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff;
        // The merged transient+preserved view is a pure function of the two
        // segment pools, so it gets a gate like every other rebuild here. It
        // used to run on EVERY config change, which with an animation attached
        // is nearly every frame - and it is not free: SegmentBoost owns a
        // colorStops vector, so clear() + push_back frees and reallocates one
        // heap block per stopped segment each time, to reproduce a list that
        // in a segment-less animation never differs.
        const bool segmentsDirty = config.neon.segmentBoosts != mCurrentConfig.neon.segmentBoosts ||
                                   config.neon.preservedSegmentBoosts != mCurrentConfig.neon.preservedSegmentBoosts;
        // Overflow warnings, before mCurrentConfig is overwritten below: the
        // previous counts are still in it, which is what lets these fire once
        // per overflow without a latch of their own.
        //
        // Both count what the HOST ASKED FOR, not what survives. That matters
        // for segments: SegmentUtils::FillEffectiveSegments stops merging at
        // MAX_SEGMENT_BOOSTS_CAP, so mEffectiveSegments is already clamped and
        // measuring it could never exceed the cap - which is exactly why the
        // old warning here never fired. The two pools are summed because they
        // share the slots.
        WarnOnOverflow("arcs", mCurrentConfig.neon.arcs.size(), config.neon.arcs.size(),
                       int(MAX_ARCS));
        WarnOnOverflow("segments",
                       mCurrentConfig.neon.segmentBoosts.size() +
                           mCurrentConfig.neon.preservedSegmentBoosts.size(),
                       config.neon.segmentBoosts.size() +
                           config.neon.preservedSegmentBoosts.size(),
                       int(MAX_SEGMENT_BOOSTS));

        // The merged transient+preserved view feeds both the segment atlas
        // below and the per-frame UBO pack, so it is refilled here - on a
        // change to either pool, and nowhere else in this call. Leaving it
        // alone otherwise is safe precisely because it is derived: an
        // unchanged pair of pools rebuilds to the list already in it, which
        // mSegmentLUT's own dirty check and packLightBlocks would both then
        // see as unmoved anyway.
        if (segmentsDirty)
        {
            SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        }

        // The emission table reads a wide slice of this config - the hue rate,
        // the sample count, all three LUTs and both light UBOs - so it is
        // invalidated on any change rather than on a gate that has to be kept
        // in step with the shader. A missed field would be a stale ring; a
        // spare rebuild is one small pass.
        mEmissionDirty = true;
        // The light blocks get the OPPOSITE treatment, because their inputs are
        // narrow and visible rather than wide and indirect: @ref
        // packLightBlockData reads mEffectiveSegments and config.neon.arcs, and
        // nothing else. mEffectiveSegments moves exactly when segmentsDirty
        // does - it was rebuilt from that flag ten lines up - so the two
        // together are the whole input set, and gating on them is the same
        // enumeration samplesDirty and fillDirty above already do. Being
        // conservative here would cost the gate its point: the common animation
        // is an intensity or geometry sweep that touches neither list, and
        // "any config change" would repack on every frame of it.
        //
        // ACCUMULATED, not assigned, and the difference is not subtle.
        //
        // This runs BEFORE the first Render - AddRenderer calls it - and on
        // that call the incoming config usually matches the defaults it is
        // compared against, so both terms are false. An assignment would clear
        // the `true` the flag is born with, no Render would ever pack, and the
        // two UBOs would be left with no data store at all: SetData is the only
        // glBufferData they ever get. Binding those and letting the shader read
        // them segfaults in the driver - measured, reproducibly, on the first
        // frame. Not a stale frame; no frame.
        //
        // The same hazard returns later in a milder form: a host that calls
        // SetConfig twice before Update gets two of these, and the second
        // compares against the arcs the first one already installed.
        // mEmissionDirty is immune to all of it only because it is
        // unconditional; a narrow gate has to hold until the pack clears it.
        mLightBlocksDirty = mLightBlocksDirty || segmentsDirty ||
                            config.neon.arcs != mCurrentConfig.neon.arcs;

        mCurrentConfig = config;
        if (!mNeonShader.IsValid())
        {
            return;
        }

        if (samplesDirty)
        {
            rebuildLoopSamples(config);
        }

        if (geometryDirty)
        {
            setupGeometry(config);
        }

        if (fillDirty)
        {
            setupFillGeometry(config);
        }

        bakeLUTs(config);
    }

    bool NeonRenderer::setupShaders()
    {
        mNeonShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                       ShaderSource::NEON_FRAG_SRC,
                                       "NeonRenderer");
        // Emission pre-pass. Reuses the neon vertex shader (uMVP -> vPos); the
        // fragment shader ignores vPos and keys off gl_FragCoord instead.
        mEmissionShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_EMISSION_FRAG_SRC,
                                        "NeonRenderer.Emission");
        // Cheap fullscreen black fill, used only by opaque mode. Reuses the
        // standard neon vertex shader (uMVP) so the fill quad respects the
        // viewport.
        mBlackRectShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::BLACK_RECT_FRAG_SRC,
                                         "NeonRenderer.BlackRect");
        // Scaled path only: composites the scaled buffer back at full res.
        // Built unconditionally rather than lazily - a shader compile in the
        // middle of a frame, the first time someone drags the scale slider off
        // 1.0, is a stall exactly where it will be blamed on the scale.
        mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                    ShaderSource::NEON_BLIT_FRAG_SRC,
                                    "NeonRenderer.Blit");
        if (!mNeonShader.IsValid() || !mBlackRectShader.IsValid() ||
            !mBlitShader.IsValid() || !mEmissionShader.IsValid())
        {
            return false;
        }

        mNeonShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        // The pre-pass reads the same two blocks the main pass does, so they
        // share bindings and are packed once per frame before either runs.
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        return true;
    }

    void NeonRenderer::setupFullscreenQuad()
    {
        // Static NDC quad, shared by the three passes that cover their whole
        // target with an identity MVP: the emission bake, the opaque-mode
        // black fill at OpaqueMode::ALL (whose shader derives its shape from
        // gl_FragCoord, not aPos - every narrower mode is bounded by
        // @ref setupFillGeometry's ring instead) and the scaled path's blit.
        //
        // Unlike setupGeometry's quad this one never changes - it is in NDC,
        // so it is independent of the geometry, the viewport and the
        // resolution scale alike. Hence uploaded once from Initialize and
        // never revisited.
        // clang-format off
        float ndc[] = {
            -1.0f,  1.0f,  -1.0f, -1.0f,   1.0f, -1.0f,
            -1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
        };
        // clang-format on
        mFullscreenVertexArray.SetVertexData(ndc, sizeof(ndc));
        mFullscreenVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
    }

    void NeonRenderer::setupGeometry(const Config &config)
    {
        // Size the quad to cover the lit region: rect + glowReach, so geometry
        // bounds the far region instead of a per-fragment discard
        // (tiler-friendly). Factors come from the shared neon-tuning.h.
        //
        // glowRadius ONLY - deliberately not the old
        // max(glowRadius * RADIUS, sampleSpacing * SPACING). sampleSpacing is
        // perimeter / NEON_MAX_LOOP_SAMPLES, so the spacing term won on any
        // reasonably large rect at default glowRadius and made the quad - and
        // with it the distance the bloom got truncated at, and the brightness
        // it still had there - track the rect size: the fade began at 250 px on
        // a 200x150 rect and 1542 px on a 1920x1080 one. The bloom's reach is a
        // function of glowRadius and nothing else, so that is all that sizes the
        // quad now. It also caps the worst case: the old spacing term asked for
        // a ~5800x4900 px quad on a 1920x1080 rect.
        //
        // The wide bloom (1/D tail) stays visible further out as bloomStrength /
        // intensity rise, so grow the quad with them. The shader reproduces this
        // exact expression to place its bloom pedestal, which is what lets the
        // margin stay this tight without the truncation showing - keep the two
        // in step.
        //
        // The whole quad is built in SCALED space, matching the transform and
        // the uniforms Render uploads. At resolutionScale 1.0 the factor is
        // identity and every expression below is its full-res form.
        const float scale = GetClampedResolutionScale(config);

        float glowReach = config.neon.glowRadius * scale * float(GLOW_REACH_RADIUS_FACTOR) *
                          (1.0f + config.neon.bloomStrength * config.neon.intensity);

        // ...but the filament is sized by lineWidth, not glowRadius, so the quad
        // must clear it too. Without this floor, glowRadius = 0 ("filament only")
        // produced a zero margin: the quad landed exactly on the rect and clipped
        // every exterior fragment, so the outside half of the filament
        // disappeared while the inside half stayed. Mirrors the shader's `sigma`.
        // Reach in sigmas depends on the falloff exponent, not a constant - a
        // soft filament's tail runs for hundreds of sigmas. Same expression as
        // the shaders' reachSigmas; see neon-tuning.h.
        float filN = 2.0f * std::max(config.neon.filamentFalloff, 1e-3f);
        float filSigmas = std::clamp(
            std::pow(std::log2(float(FILAMENT_GAIN) / float(FILAMENT_CUTOFF)), 1.0f / filN),
            float(FILAMENT_REACH_MIN_SIGMAS), float(FILAMENT_REACH_MAX_SIGMAS));
        // FILAMENT_MIN_HALF_WIDTH is a full-res constant, so the whole
        // half-width is taken in full-res px and scaled once - the same
        // conversion the shader does with uResolutionScale.
        float filamentReach = std::max(config.neon.lineWidth * 0.5f, float(FILAMENT_MIN_HALF_WIDTH)) * scale * filSigmas;

        float margin = std::max(glowReach, filamentReach);

        // Hard cap: when the outside cutoff is enabled the shader discards
        // emission past size + softness, so there's no point rasterising
        // further. Disabled outside cutoff leaves the natural glowRadius /
        // bloom-driven margin untouched. Add a 1 px safety so the shader's
        // own softmask fades to zero *before* the quad edge and no
        // rectangular seam leaks through.
        //
        // The WHOLE expression is built in full-res px and scaled once, so the
        // safety margin is 1 FULL-RES px at every resolution scale. Adding the
        // +1 after the scale instead makes it 1 buffer px - 2 full-res px at
        // scale 0.5 - which pushes the quad edge out and with it the ramp the
        // shader fits between the cutoff boundary and uQuadMargin. Same units
        // on both sides is also what makes the shader's fadeStart floor engage
        // at the same cutoff size regardless of scale.
        if (config.neon.outsideCutoff.enable)
        {
            // Mirrors neon.frag's softFloor, which is what actually decides how
            // far past the cutoff the feather runs and therefore how much quad
            // the shader needs. The shader floors in BUFFER px, so it is
            // converted back to full-res here - this whole expression is
            // full-res and scaled once, per the note above. Leave it out and
            // the quad edge lands inside the widened feather, which is exactly
            // the rectangular seam the +1 safety exists to prevent.
            const float softFloor = (scale < 1.0f)
                                        ? (static_cast<float>(CUTOFF_SOFT_FLOOR_PX) / scale)
                                        : static_cast<float>(SIDE_SOFT_EPSILON);
            float outSoft = std::max(config.neon.outsideCutoff.softness, softFloor);
            float cutoffCap = (config.neon.outsideCutoff.size + outSoft + 1.0f) * scale;
            margin = std::min(margin, cutoffCap);
        }
        mQuadMargin = margin;

        float halfW = config.geometry.width * 0.5f * scale;
        float halfH = config.geometry.height * 0.5f * scale;
        float l = -(halfW + margin);
        float r = halfW + margin;
        float b = -(halfH + margin);
        float t = halfH + margin;

        // clang-format off
        float verts[] = {
            l, t, l, b, r, b,
            l, t, r, b, r, t,
        };
        // clang-format on

        // GL_DYNAMIC_DRAW, and no SetAttribPointer - the format was declared
        // once in Initialize.
        //
        // The hint is the part that matters. This quad is rebuilt from
        // geometryDirty, whose inputs include intensity, lineWidth, glowRadius,
        // bloomStrength and filamentFalloff - and every one of those is an
        // AnimatableField. So an intensity pulse, the most ordinary animation
        // this library offers, respecifies this buffer EVERY FRAME. Telling
        // the driver GL_STATIC_DRAW ("specify once, use many") about a buffer
        // rewritten 60 times a second is the wrong hint, and invites exactly
        // the placement that makes a per-frame rewrite expensive.
        //
        // Still glBufferData and not glBufferSubData, deliberately. A whole
        // respecification lets the driver orphan the old store and hand back
        // fresh memory, which is the standard way to rewrite a buffer the GPU
        // may still be reading; a SubData into that same store is what risks
        // an implicit sync. Same reasoning in @ref setupFillGeometry.
        mGlowVertexArray.SetVertexData(verts, sizeof(verts), GL_DYNAMIC_DRAW);
    }

    void NeonRenderer::setupFillGeometry(const Config &config)
    {
        // Bound the opaque fill with geometry, exactly as @ref setupGeometry
        // bounds the glow. black-rect.frag shapes the band from an analytic
        // SDF and discards everything outside it, so a fullscreen quad drew
        // the right picture - but it SHADED every pixel in the viewport to do
        // it, and a discarded fragment costs very nearly what a kept one does.
        // Measured on a 3840x2160 target, that was a fixed ~1.4 ms per frame
        // whether the band was 20 px wide or the whole screen.
        //
        // Everything here is FULL-RES and unscaled, matching the pass: the
        // fill always draws on the caller's framebuffer at its own resolution,
        // whatever NeonConfig::resolutionScale is doing to the glow.
        //
        // A fill that covers the viewport at coverage 1 needs no geometry at
        // all - @ref renderOpaqueFill clears instead. That is ALL by
        // definition and BOTH with both cutoffs disabled by arithmetic; see
        // @ref FillsWholeViewport, which is the ONE place the two passes agree
        // on the question. NONE never reaches the pass at all.
        const OpaqueMode mode = config.neon.opaqueMode;
        if (mode == OpaqueMode::NONE || FillsWholeViewport(config.neon))
        {
            mFillVertexCount = 0;
            return;
        }

        // How far the fill's coverage can run past each boundary. The shader
        // centres a ramp of width `softW` on the boundary, so it reaches
        // softW/2 beyond it; SAFETY absorbs that rounding plus the fwidth-based
        // `aa` floor, which is ~1 px on a flat edge and up to ~1.4 px on a
        // diagonal one. Over-covering by a couple of pixels is free - those
        // fragments come out at coverage 0, which this pass's premultiplied
        // blend leaves the destination untouched by - while under-covering
        // would clip the feather, so this rounds outward on purpose.
        constexpr float FILL_EDGE_SAFETY = 3.0f;
        const float softHalf = 0.5f * std::max(config.neon.opaqueSoftness,
                                               static_cast<float>(SIDE_SOFT_EPSILON));

        // Per mode, how far the band extends either side of the rect edge.
        // A side the mode does not fill still gets FILL_EDGE_SAFETY, because
        // the d == 0 edge itself carries a one-pixel AA ramp that straddles it.
        //
        // A DISABLED cutoff arrives as the huge CUTOFF_DISABLED_SIZE sentinel
        // and is handled by the arithmetic rather than by a branch: outward it
        // pushes the ring off-viewport, where the rasteriser clips it (the
        // fill genuinely does reach the screen edge there, and the cap below
        // keeps "off-viewport" from meaning "1e6 px off-viewport"), and inward
        // it drives the hole's half-extent to zero below, collapsing the ring
        // into a solid quad (the fill genuinely does cover the interior).
        float outerMargin = FILL_EDGE_SAFETY;
        float innerMargin = FILL_EDGE_SAFETY;
        if (mode == OpaqueMode::OUTSIDE || mode == OpaqueMode::BOTH)
        {
            outerMargin = GetCutoffSize(config.neon.outsideCutoff) + softHalf + FILL_EDGE_SAFETY;
        }
        if (mode == OpaqueMode::INSIDE || mode == OpaqueMode::BOTH)
        {
            innerMargin = GetCutoffSize(config.neon.insideCutoff) + softHalf + FILL_EDGE_SAFETY;
        }

        // Cap on how far OUTWARD the ring is allowed to run, in full-res px.
        //
        // Only the outward direction needs one. Inward, the sentinel is
        // absorbed before it can reach a vertex - it drives holeRadius and
        // both hole half-extents through a max(..., 0) below, so innerMargin
        // never appears in the buffer. Outward it lands in `ow` / `oh`
        // directly, which meant a disabled outside cutoff shipped vertices at
        // ~1e6 px: about 555 in NDC on a 3600-wide viewport, far outside any
        // guard band, so the driver has to genuinely clip rather than trivially
        // accept. Eight triangles make that cheap, and nothing observably wrong
        // has been seen from it here - this is insurance for the Mali / Tizen
        // side, not a fix for a reproduced defect.
        //
        // What makes the clamp SAFE is that this geometry is a conservative
        // bound and nothing else: the silhouette comes from the SDF reading
        // gl_FragCoord, and the shader still receives the true sentinel through
        // uOutsideCutoff. Replacing one conservative bound with a tighter one
        // changes which fragments are rasterised, never what they shade to -
        // so long as the tighter one still covers every pixel the shader would
        // give non-zero coverage.
        //
        // It does. A disabled outside cutoff means "fill everything outside the
        // rect", which is bounded in practice by the viewport, and a viewport
        // cannot exceed GL_MAX_VIEWPORT_DIMS - 16384 or 32768 on the hardware
        // this targets. 65536 clears the larger of those by 2x while staying an
        // exactly-representable float with room to spare (integers are exact to
        // 2^24), so a rect placed anywhere inside any legal viewport is still
        // covered to its far corner. Viewport-independence is preserved, which
        // matters: the ring is built from OnConfigChanged, which has no
        // viewport to consult, and a resize must keep not rebuilding it.
        constexpr float FILL_MAX_OUTER_MARGIN = 65536.0f;
        outerMargin = std::min(outerMargin, FILL_MAX_OUTER_MARGIN);

        const float halfW = config.geometry.width * 0.5f;
        const float halfH = config.geometry.height * 0.5f;
        const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry);

        // OUTER edge. The outward parallel curve of a rounded box at distance m
        // is a rounded box grown by m on each half-extent, so its axis-aligned
        // bound is exactly this - no corner correction needed. Same at
        // cornerRadius 0, where black-rect.frag's bandOuterDistance offsets the
        // box per-axis and reaches halfSize + cut on the nose.
        const float ow = halfW + outerMargin;
        const float oh = halfH + outerMargin;

        // HOLE. This one DOES need the corner correction, and getting it wrong
        // is visible: the region the fill cannot reach is the INWARD parallel
        // curve, a rounded box shrunk by innerMargin with its radius shrunk to
        // match - and the largest axis-aligned rectangle inside a rounded box
        // is not the box's own half-extents. Its corners have to clear the
        // corner arc, which pulls each half-extent in by r - r/sqrt(2).
        //
        // Cutting the hole square instead left a triangular wedge at each
        // corner outside the ring but inside the fill's real footprint, and the
        // fill lost it: at the default cornerRadius of 40 that is a ~12 px bite
        // out of all four corners of the band.
        constexpr float CORNER_INSET_FACTOR = 0.2928932f; // 1 - 1/sqrt(2)
        const float holeRadius = std::max(radius - innerMargin, 0.0f);
        const float cornerInset = holeRadius * CORNER_INSET_FACTOR;
        // Clamped at zero so an inside cutoff deeper than the rect - or a
        // disabled one, arriving as the sentinel - collapses the hole and
        // degenerates the ring into a filled quad instead of inverting it.
        const float iw = std::max(halfW - innerMargin - cornerInset, 0.0f);
        const float ih = std::max(halfH - innerMargin - cornerInset, 0.0f);

        // Four quads that TILE the ring without overlapping - top and bottom
        // full width, left and right only across the hole's height. Overlap
        // would matter: the pass composites premultiplied-over, so a fragment
        // covered twice would blend twice and read denser than the shader's
        // own coverage. When the hole has collapsed (iw == ih == 0) top and
        // bottom already meet at y = 0 and the side quads come out degenerate,
        // which is the filled-quad case falling out for free.
        // clang-format off
        const float verts[] = {
            // top band: y in [ih, oh]
            -ow, oh,  -ow, ih,   ow, ih,
            -ow, oh,   ow, ih,   ow, oh,
            // bottom band: y in [-oh, -ih]
            -ow, -ih,  -ow, -oh,   ow, -oh,
            -ow, -ih,   ow, -oh,   ow, -ih,
            // left band: x in [-ow, -iw], across the hole only
            -ow, ih,  -ow, -ih,  -iw, -ih,
            -ow, ih,  -iw, -ih,  -iw,  ih,
            // right band: x in [iw, ow], across the hole only
             iw, ih,   iw, -ih,   ow, -ih,
             iw, ih,   ow, -ih,   ow,  ih,
        };
        // clang-format on

        // GL_DYNAMIC_DRAW and no SetAttribPointer, for the reasons spelled out
        // at the end of @ref setupGeometry. Latent here rather than live: the
        // ring's dirty set (geometry, opaqueMode, opaqueSoftness, the two
        // cutoffs) contains no AnimatableField today, so this fires on host
        // edits and not per frame. It is hinted correctly anyway, because the
        // day a cutoff or the geometry becomes animatable is not the day
        // anyone will think to come back and look at a usage flag.
        mFillVertexArray.SetVertexData(verts, sizeof(verts), GL_DYNAMIC_DRAW);
        mFillVertexCount = 24;
    }

    void NeonRenderer::rebuildLoopSamples(const Config &config)
    {
        // Evenly spaced points (by arc length) around the rounded-rect perimeter.
        // Drives the additive halo/spill/colour gather in the fragment shader.
        // Uploaded directly to the std140 UBO: vec4[N] where .xy holds the
        // position in SCALED px - raw float32 through the constant cache, no
        // decode step in the shader. (.zw stays 0 - the shader recovers a
        // fragment's continuous perimeter position geometrically from vPos, so
        // the per-sample phase pairs are no longer needed.)
        //
        // Only the first `n` entries are written; the rest of the block stays
        // (0,0,0,0) and is never read, because the shader's loop bound is the
        // same `n`. The spacing is 1/n of the perimeter, so lowering the count
        // spreads the samples rather than truncating the walk partway round.
        const float scale = GetClampedResolutionScale(config);
        const int n = GetClampedNumSamples(config);

        LoopSamplesBlockData block = {};
        for (int i = 0; i < n; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(n);
            glm::vec2 p = GeometryUtils::GetPointOnRectangle(t, config.geometry) * scale;
            block.samples[i] = glm::vec4(p, 0.0f, 0.0f);
        }
        mLoopSamplesBlock.SetData(&block, sizeof(block));
    }

    void NeonRenderer::bakeLUTs(const Config &config)
    {
        // All three LUT wrappers self-guard: each re-bakes only when the inputs
        // it actually reads have moved, so this is called unconditionally and
        // no dirty flag lives at the call site. The live fields an animation
        // rewrites every frame (a segment's position / length / boost, an arc's
        // start / length / intensity) ride the UBOs and never dirty a LUT.
        //
        // @note mEffectiveSegments must already hold the merged
        //       transient+preserved view - both callers leave it current
        //       first (Initialize fills it outright; OnConfigChanged refills
        //       it on a change to either pool and otherwise it is unmoved).
        // The ring width is a runtime knob; a change to it makes GradientRingLUT
        // SNAP rather than fade, since two rings of different length cannot be
        // blended element-wise. The two atlases below keep fixed widths - a
        // segment or arc span is short and does not wrap, so extra texels would
        // only pad head and tail.
        mGradientLUT.Bake(config.neon.colorStops, config.neon.blendSpace,
                          config.neon.gradientLutSize, config.neon.colorTransitionDuration);
        mSegmentLUT.Bake(mEffectiveSegments, SEGMENT_LUT_WIDTH, MAX_SEGMENT_BOOSTS);
        mArcLUT.Bake(config.neon.arcs, ARC_LUT_WIDTH, MAX_ARCS);
    }

    bool NeonRenderer::resizeEmissionBuffer()
    {
        // Walk EMISSION_FORMATS from the best the driver has not already
        // refused, and take the first that allocates.
        //
        // Where the walk STARTS is what keeps this cheap. Re-asking for a
        // format the driver refused would churn the attachment every frame -
        // Framebuffer::Resize treats a format change as a reallocation, and its
        // failure path destroys what was there - so a live buffer starts at the
        // format it is already holding, which Resize then early-outs on. Only a
        // buffer with no attachment (first frame, or after a failure) starts at
        // the top. The buffer's own state is the record of how far down the list
        // this renderer got; there is no flag here saying so.
        size_t first = 0;
        if (mEmissionBuffer.IsValid())
        {
            for (size_t i = 0; i < std::size(EMISSION_FORMATS); ++i)
            {
                if (EMISSION_FORMATS[i].internalFormat == mEmissionBuffer.GetInternalFormat())
                {
                    first = i;
                    break;
                }
            }
        }

        for (size_t i = first; i < std::size(EMISSION_FORMATS); ++i)
        {
            const EmissionFormat &f = EMISSION_FORMATS[i];
            if (mEmissionBuffer.Resize(NEON_MAX_LOOP_SAMPLES, 2,
                                       f.internalFormat, f.format, f.type, EMISSION_FILTER))
            {
                return true;
            }
            if (i + 1 < std::size(EMISSION_FORMATS))
            {
                // Once per driver, not once per frame: the next candidate's
                // success moves `first` past this one for every later call.
                LOG_E("NeonRenderer: %s emission target unavailable, falling back to %s. "
                      "Arc intensities and stacked segment boosts above 1.0 will clamp.",
                      f.name, EMISSION_FORMATS[i + 1].name);
            }
        }
        return false;
    }

    void NeonRenderer::packLightBlocks(const Config &config)
    {
        // Both the emission pre-pass and the main pass read these, so they are
        // current before either draws.
        //
        // The PACK is gated; the BIND is not. The block contents are a pure
        // function of the config, so repacking them on a frame that changed
        // nothing reproduces bytes byte for byte - the same argument the
        // emission table rests on, one tier cheaper. The binding is different:
        // glBindBufferBase writes global context state that a host (or a
        // future pass) can repoint between frames, and re-asserting it costs
        // two calls against a whole frame's worth of drawing, so it stays
        // unconditional rather than being inferred from a flag this class
        // owns.
        if (mLightBlocksDirty)
        {
            packLightBlockData(config);
            mLightBlocksDirty = false;
        }
        mSegmentBlock.BindBase(SEGMENT_BLOCK_BINDING);
        mArcBlock.BindBase(ARC_BLOCK_BINDING);
    }

    void NeonRenderer::packLightBlockData(const Config &config)
    {
        // Pack the segment vector as vec4(position, invSigma, boost, hasStops)
        // into the std140 SegmentBlock UBO (DALi-compatible pattern - see
        // neon.frag). Empty vector -> uSegmentCount=0 and both shaders skip the
        // whole feature.
        SegmentBlockData segBlock = {};
        // mEffectiveSegments is NOT refilled here. OnConfigChanged refills it
        // whenever either segment pool changes, and this runs once per frame
        // from Render - so on a frame where the pools did not move the merged
        // view is already current, and on a frame where they did,
        // OnConfigChanged has already run (Update -> refreshActiveConfig
        // precedes Render). Refilling here would be a second
        // FillEffectiveSegments of the same frame.
        const std::vector<SegmentBoost> &effSegments = mEffectiveSegments;
        int segCount = std::min(static_cast<int>(effSegments.size()),
                                int(MAX_SEGMENT_BOOSTS));
        segBlock.count = segCount;
        for (int i = 0; i < segCount; ++i)
        {
            const auto &s = effSegments[i];
            float invSigma = 1.0f / std::max(s.length * 0.5f, 1e-3f);
            // .w = hasOwnStops flag; the shader reads its colour from row `i`
            // of the segment LUT atlas when set, else falls back to the base
            // gradient at that sample.
            float hasStops = s.colorStops.empty() ? 0.0f : 1.0f;
            segBlock.segments[i] = glm::vec4(s.position, invSigma, s.boost, hasStops);
        }
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));

        // Pack the arcs vector into ArcBlock: vec4(start, length, intensity,
        // hasStops) per entry. .w picks between the winner arc's own atlas row
        // and the base gradient in the shader's winner-take-all branch.
        ArcBlockData arcBlock = {};
        int arcCount = std::min(static_cast<int>(config.neon.arcs.size()),
                                int(MAX_ARCS));
        arcBlock.count = arcCount;
        for (int i = 0; i < arcCount; ++i)
        {
            const auto &a = config.neon.arcs[i];
            // .w is a bitmask, not just hasStops - see PackArcFlags.
            float flags = PackArcFlags(config.neon.arcs, i, arcCount);
            arcBlock.arcs[i] = glm::vec4(a.start, a.length, a.intensity, flags);
        }
        mArcBlock.SetData(&arcBlock, sizeof(arcBlock));
    }

    bool NeonRenderer::isEmissionTableStale(float time, const Config &config) const
    {
        if (mEmissionDirty)
        {
            return true;
        }

        // uTime enters neon-emission.frag in exactly one place - `float ti =
        // si - uTime * uHueRotationRate` - so at rate 0 it is multiplied out
        // and the table is the same at every time. That is not a tolerance:
        // the product is exactly zero, so the two bakes agree bit for bit.
        //
        // The comparison is exact for the same reason it can be: `time` is
        // fed straight back from the last bake, not recomputed, so an
        // unchanged clock reproduces the identical float. A moving clock
        // essentially never lands on the same value twice, and if it did the
        // table it wants IS the one already in the buffer.
        return config.neon.hueRotationRate != 0.0f && time != mEmissionTime;
    }

    void NeonRenderer::renderEmissionPass(int viewportWidth, int viewportHeight,
                                          float time, const Config &config)
    {
        // The target the gather below draws into. NOT necessarily the default
        // framebuffer: an offscreen frame capture (@ref OffscreenCapture) hands
        // this renderer a real FBO, and the gather has no bind of its own, so
        // restoring 0 here would silently redirect the whole neon pass to the
        // window and leave the capture empty. Read BEFORE the resize below, so
        // it stays correct even if a reallocation ever rebinds.
        const GLuint targetFbo = Framebuffer::GetBoundId();

        // The viewport BOX, not just its size. This used to restore
        // (0, 0, viewportWidth, viewportHeight), which is the same thing only
        // while the caller's viewport starts at the origin and fills the
        // target - the assumption Render's signature encourages but does not
        // enforce. A host drawing the effect into a sub-rect got its viewport
        // silently replaced with the full one here.
        //
        // That was invisible while this pass ran unconditionally, because
        // every frame was overwritten the same way. Now that it is skipped on
        // frames the table has not moved, a bake frame and a skip frame would
        // leave DIFFERENT viewports and the glow would jump between them -
        // which is how the sub-rect case surfaced. Restoring what was actually
        // found makes the two paths identical, and matches the rule every
        // other pass here follows: hand back the state you were given.
        GLint prevViewport[4] = {0, 0, viewportWidth, viewportHeight};
        glGetIntegerv(GL_VIEWPORT, prevViewport);

        // Binds the FBO and sets the viewport to NEON_MAX_LOOP_SAMPLES x 2. No
        // clear: the NDC quad covers every texel, so each one is written.
        mEmissionBuffer.Bind();

        mEmissionShader.Use();
        mEmissionShader.SetUniform("uMVP", glm::mat4(1.0f));
        mEmissionShader.SetUniform("uTime", time);
        mEmissionShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        // The SAME count the gather is given below - texel i here has to be
        // sample i there, or every fragment reads emission belonging to a
        // different perimeter position. Both go through GetClampedNumSamples
        // for exactly that reason.
        mEmissionShader.SetUniform("uNumSamples", GetClampedNumSamples(config));
        mGradientLUT.Bind(0);
        mEmissionShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mEmissionShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mEmissionShader.SetUniform("uArcLUT", 2);
        mFullscreenVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mEmissionShader.Unuse();

        // Hand the framebuffer and viewport back exactly as found. Blend mode
        // is untouched here - it is a phase property owned by Render.
        Framebuffer::BindId(targetFbo);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

        // What the buffer now holds. Recorded by the only writer of it, so the
        // staleness test upstream can never describe a bake that did not run.
        mEmissionDirty = false;
        mEmissionTime = time;
    }

    bool NeonRenderer::renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight,
                                      bool scaled, float time, const Config &config)
    {
        const float scale = GetClampedResolutionScale(config);

        if (scaled)
        {
            // Resize destroys the attachment on its failure path, so a failure
            // leaves mScaledBuffer holding id 0 - and Bind() would then bind
            // the CALLER'S framebuffer, whereupon the glClear below erases
            // everything already drawn this frame (glClear is not clipped by
            // the viewport). Under an OffscreenCapture that target is the
            // capture. Bail instead; Render skips the blit with us.
            //
            // The filter is requested through Resize, which is the ONLY writer
            // of the tracked value, so it cannot drift. Setting it on the
            // texture afterwards instead leaves mFilter disagreeing with the
            // texture, and the next frame's Resize then sees a mismatch and
            // destroys and recreates the FBO - one reallocation per frame,
            // measured.
            if (!mScaledBuffer.Resize(bufWidth, bufHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR))
            {
                return false;
            }
            mScaledBuffer.Bind();

            // Transparent black, passed as an ARGUMENT rather than staged
            // through GL_COLOR_CLEAR_VALUE. This used to save the host's clear
            // colour, overwrite it, clear, and put it back - a glGetFloatv and
            // two glClearColor calls every frame on the scaled path, plus a
            // window in which a host reading its own clear colour would find
            // it replaced. glClearBufferfv touches none of that context state.
            // See the fuller note in @ref renderOpaqueFill, which makes the
            // same swap on the pass's other clear.
            static const GLfloat TRANSPARENT_BLACK[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 0, TRANSPARENT_BLACK);
        }

        // Every pixel-valued uniform below is multiplied by `scale`, which is
        // 1.0 on the direct path - so the two paths upload literally the same
        // numbers there, and the scaled path is the only one that moves. The
        // shader converts neon-tuning.h's own full-res px constants with
        // uResolutionScale to land in the same space.
        mNeonShader.Use();
        mNeonShader.SetUniform("uMVP", mvp);
        mNeonShader.SetUniform("uResolutionScale", scale);
        mNeonShader.SetUniform("uRectSize", glm::vec2(config.geometry.width * scale,
                                                         config.geometry.height * scale));
        mNeonShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry) * scale);
        mNeonShader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        mNeonShader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        mNeonShader.SetUniform("uIntensity", config.neon.intensity);
        mNeonShader.SetUniform("uTime", time);
        mNeonShader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        mNeonShader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        mNeonShader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        mNeonShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mNeonShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        mNeonShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff) * scale);
        mNeonShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        mNeonShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff) * scale);
        mNeonShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);

        mNeonShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));

        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point in
        // the same scaled space as the transform above.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);
        mNeonShader.SetUniform("uNumSamples", GetClampedNumSamples(config));

        // The three LUT atlases are no longer read by the gather (the emission
        // pre-pass consumes them instead), but the pointwise path still samples
        // them for the colour-stop alpha - see the alpha reads in neon.frag.
        mGradientLUT.Bind(0);
        mNeonShader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        mNeonShader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        mNeonShader.SetUniform("uArcLUT", 2);
        // Emission table from pass 0 on unit 3; the gather texelFetches both
        // of its rows per sample.
        mEmissionBuffer.BindTexture(3);
        mNeonShader.SetUniform("uEmission", 3);
        mNeonShader.SetUniform("uQuadMargin", mQuadMargin);

        // Tight glow quad in both modes - opaque's far region is covered by the
        // fill pass, so the gather never runs fullscreen.
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mNeonShader.Unuse();
        return true;
    }

    void NeonRenderer::renderOpaqueFill(int viewportWidth, int viewportHeight, const Config &config)
    {
        // The fragment shader shapes the black coverage from an analytic
        // rounded-box SDF read off gl_FragCoord (highp - exact on Mali/Tizen):
        //   ALL     -> black everywhere (whole viewport opaque).
        //   BOTH    -> black across the whole band, inside cutoff to outside.
        //   INSIDE  -> black only where d <= softEdge (off-side stays clear).
        //   OUTSIDE -> mirror of INSIDE.
        //
        // ...except that the two of those whose coverage is 1 everywhere never
        // reach the shader at all - see @ref FillsWholeViewport and the clear
        // below.
        //
        // Everything here is FULL-RES and unscaled, unlike every other pass:
        // this one always draws on the caller's framebuffer. Rounded corners
        // anti-alias analytically via fwidth(d), so drawing the silhouette at
        // a reduced scale and letting the blit stretch it would trade its one
        // real advantage - a clean edge at any radius - for nothing.
        //
        // The centre is derived here rather than taken from Render's scaled
        // transform for the same reason. Viewport y runs down in Config but up
        // in gl_FragCoord, hence the mirror about the viewport height.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        const glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                                   static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);

        // A shaped mode reaching the shader below draws the band ring built by
        // @ref setupFillGeometry, which is in full-res rect-local pixels and so
        // needs a full-res transform to place it. A coverage-1 mode that could
        // not take the clear draws the identity-MVP fullscreen quad instead,
        // which is what `ring` selects between - no ring was built for it. The
        // SDF is unaffected either way: it reads gl_FragCoord, not the vertex
        // position, so the transform decides only WHICH fragments are shaded,
        // never what they shade to.
        // A fill whose coverage is 1 at every pixel is a flat overwrite, so it
        // does not need a shader at all. Premultiplied-over with coverage 1
        // leaves exactly uOpaqueColor.rgb and alpha 1, which is what glClear
        // writes - and a clear costs no fragments, no blending and no vertex
        // work. Measured here at 3840x2160 it takes ~0.4 ms off the pass, the
        // one case @ref setupFillGeometry's ring cannot help with because the
        // coverage really is the whole screen.
        //
        // The test is @ref FillsWholeViewport, not `mode == ALL`: BOTH with
        // both cutoffs disabled produces identical output, and that is the
        // DEFAULT cutoff state, so it was the common way into this cost rather
        // than a corner case. @ref setupFillGeometry asks the same function, so
        // a coverage-1 mode always arrives with mFillVertexCount == 0.
        //
        // Measured on that BOTH case with debug.opaqueOnly isolating the pass,
        // 3600x2126, min of 200 frames around glFinish: 1.18 ms before the
        // routing, 0.59 ms after, stable to +-0.07 ms across runs. The frame
        // is byte-identical either way - and byte-identical to ALL, which is
        // the whole argument for the routing.
        //
        // WHAT THIS DOES NOT BUY, because an earlier version of this comment
        // claimed it did: on a tile-based GPU a clear can mean "the previous
        // contents are dead, never load the tile from memory". That is a
        // property of an UNSCISSORED, full-attachment clear at the START of a
        // render pass, and this clear is neither. It runs mid-pass - the host
        // has already drawn its backdrop into this target and the neon is
        // still to come - so the load has happened either way; and it is
        // scissored by construction (see the box derivation below), which is
        // enough on its own to keep most tilers off the fast path. Expect the
        // saving here to be exactly what the numbers above measure - no
        // fragments, no blending, no vertex work - and nothing structural
        // beyond it. Still a clear win, just a smaller one than advertised.
        // Both measurements are desktop macOS, an immediate-mode renderer;
        // the tiler behaviour is reasoned, not measured on device.
        //
        // @ref ClearClipsLikeDraw is the second half of the condition, and it
        // is a correctness guard rather than an optimisation: the clear is an
        // exact substitute only while nothing that clips a draw but not a
        // clear is switched on. When one is, this falls through to the shader
        // path below and pays the full-viewport shade, which is the right way
        // round - a host enabled those tests in order to clip something, and
        // the saving is not worth silently ignoring it. Note the short-circuit
        // ordering: the two glIsEnabled queries run only for the modes that
        // could actually use a clear, not on every frame of every mode.
        if (FillsWholeViewport(config.neon) && ClearClipsLikeDraw())
        {
            // glClear is bounded by the SCISSOR, not the viewport, so a
            // framebuffer larger than the viewport would be wiped outside it.
            // Hence a scissor of our own - but INTERSECTED with whatever the
            // host already had, never replacing it. The quad this replaced was
            // clipped by a host scissor like any other draw; a clear box set
            // to the bare viewport is not, and would paint over exactly the
            // region the host clipped this pass out of. A host clipping the
            // effect to a sub-rect saw the whole surface go opaque.
            //
            // Every piece of state touched here is restored: a host that keeps
            // its own scissor must not find it changed. The scissor is now the
            // ONLY state this branch touches - see the glClearBufferfv note
            // below for how the clear colour stopped being part of that list.
            //
            // These two queries are the irreducible ones. The box has to be
            // read to be intersected with, and whether the test is on decides
            // both the intersection and the restore - there is no way to ask
            // "clear this rectangle" without going through the scissor, and no
            // shadow copy would be trustworthy since the host owns this state
            // between frames. Both are static-state reads, the cheap kind of
            // glGet; they are not the pipeline-draining kind that reads back a
            // result. Kept deliberately, not overlooked.
            const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
            GLint prevBox[4];
            glGetIntegerv(GL_SCISSOR_BOX, prevBox);

            // The box starts at the VIEWPORT, and the viewport is QUERIED
            // rather than assumed to be (0, 0, viewportWidth, viewportHeight).
            // A fullscreen NDC quad - what this clear replaced - is clipped to
            // wherever the viewport actually sits, so reproducing that is the
            // whole job here. Assuming the origin instead meant a host drawing
            // through glViewport(x, y, w, h) with a non-zero origin got a
            // different rectangle erased than the one it asked the effect to
            // draw into; erased, note, not merely clipped, which is the worse
            // way round to be wrong. Read before this pass binds anything -
            // the fill runs first in Render's schedule, so this IS the host's
            // viewport.
            GLint vp[4];
            glGetIntegerv(GL_VIEWPORT, vp);

            GLint clearX = vp[0];
            GLint clearY = vp[1];
            GLint clearW = vp[2];
            GLint clearH = vp[3];
            if (prevScissor)
            {
                const GLint maxX = std::min(clearX + clearW, prevBox[0] + prevBox[2]);
                const GLint maxY = std::min(clearY + clearH, prevBox[1] + prevBox[3]);
                clearX = std::max(clearX, prevBox[0]);
                clearY = std::max(clearY, prevBox[1]);
                clearW = std::max(maxX - clearX, 0);
                clearH = std::max(maxY - clearY, 0);
            }
            // The host has clipped this pass away entirely. Return BEFORE
            // touching any state, so there is nothing to put back.
            if (clearW <= 0 || clearH <= 0)
            {
                return;
            }

            glEnable(GL_SCISSOR_TEST);
            glScissor(clearX, clearY, clearW, clearH);

            // glClearBufferfv, not glClearColor + glClear. It takes the colour
            // as an ARGUMENT instead of reading it out of context state, which
            // deletes the whole save-mutate-restore dance this used to need:
            // one glGetFloatv(GL_COLOR_CLEAR_VALUE) and two glClearColor calls
            // per frame, all three of them gone. Better than making the query
            // cheap - the clear colour is now never touched, so there is no
            // window in which a host that reads its own GL_COLOR_CLEAR_VALUE
            // could observe it as transparent black, and no restore to get
            // wrong on an early return.
            //
            // Same clipping semantics as the glClear it replaces, which is
            // what makes the swap safe: scissor and colour mask apply, depth
            // and stencil do not (hence @ref ClearClipsLikeDraw above, still
            // exactly the right guard). GL 3.0 / GLES 3.0 core, so it is
            // available on both of this project's version lines. Only draw
            // buffer 0 is cleared rather than every enabled one, which is the
            // same thing here - these targets carry a single colour
            // attachment.
            //
            // Alpha 1, matching the coverage the shader path writes - the
            // fill is opaque, whatever uOpaqueColor.a says (see the note on
            // the colour uniform in black-rect.frag).
            const GLfloat fillRGBA[4] = {config.neon.opaqueColor.r,
                                         config.neon.opaqueColor.g,
                                         config.neon.opaqueColor.b, 1.0f};
            glClearBufferfv(GL_COLOR, 0, fillRGBA);

            glScissor(prevBox[0], prevBox[1], prevBox[2], prevBox[3]);
            if (!prevScissor)
            {
                glDisable(GL_SCISSOR_TEST);
            }
            return;
        }

        const bool ring = (mFillVertexCount > 0);
        glm::mat4 fillMvp(1.0f);
        if (ring)
        {
            const glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                              0.0f, static_cast<float>(viewportHeight), -1.0f, 1.0f);
            fillMvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(centerFull, 0.0f));
        }

        mBlackRectShader.Use();
        mBlackRectShader.SetUniform("uMVP", fillMvp);
        mBlackRectShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlackRectShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
        mBlackRectShader.SetUniform("uRectCenter", centerFull);
        float opaqueSoft = std::max(config.neon.opaqueSoftness,
                                    static_cast<float>(SIDE_SOFT_EPSILON));
        mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
        mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mBlackRectShader.SetUniform("uOpaqueSoftness", opaqueSoft);
        mBlackRectShader.SetUniform("uOpaqueColor", config.neon.opaqueColor);
        if (ring)
        {
            mFillVertexArray.DrawArrays(GL_TRIANGLES, mFillVertexCount);
        }
        else
        {
            mFullscreenVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }
        mBlackRectShader.Unuse();
    }

    void NeonRenderer::renderBlitPass()
    {
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader is a plain texture read that composites over whatever is on
        // the target already (the black fill if opaque, the original
        // background otherwise).
        mBlitShader.Use();
        mBlitShader.SetUniform("uMVP", glm::mat4(1.0f));

        // Just bind it. The filter is requested through Resize in the gather
        // pass, so this pass sets no texture parameters at all.
        mScaledBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mFullscreenVertexArray.DrawArrays(GL_TRIANGLES, 6);
        mBlitShader.Unuse();
    }
}
