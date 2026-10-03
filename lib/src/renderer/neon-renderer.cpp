#include "renderer/neon-renderer.h"
#include "renderer/neon-tuning.h"
#include "util/geometry-utils.h"
#include "util/segment-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdint>
#include <string>

namespace EdgeLighting
{
    namespace
    {
        /// Pixel distance the shaders should treat as a cutoff's boundary -
        /// @c Cutoff::size, where its fade STARTS - as uInsideCutoff /
        /// uOutsideCutoff, for the glow and the fill alike. The shaders place
        /// the fade from there themselves (see inMid / outMid in neon.frag and
        /// black-rect.frag): solid up to @c size, gone by @c size + @c softness.
        ///
        /// Disabled cutoffs collapse to a huge sentinel so the shader's
        /// smoothstep / discard math naturally no-ops on realistic geometry;
        /// only the CPU knows this number, shaders see it as a plain uniform.
        constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;
        inline float GetCutoffSize(const Cutoff &c)
        {
            return c.enable ? c.size : CUTOFF_DISABLED_SIZE;
        }

        /// Pixel distance where a cutoff's fade ENDS and the layer is gone,
        /// for a shader that floors the fade's width at @p floorPx. Every CPU
        /// bound derived from a cutoff - the glow quad's outer cap and inner
        /// hole, the fill ring - asks this, so the geometry follows the rule
        /// the shaders use to place the fade and cannot drift from it.
        ///
        /// That rule, mirrored: the fade's midpoint is @c size + softness/2,
        /// and the floored width is laid symmetrically about it. So the end is
        ///
        ///     size + softness/2 + max(softness, floorPx)/2
        ///
        /// which is exactly @c size + @c softness at or above the floor, and
        /// half a floor past the midpoint below it. A negative softness is
        /// treated as 0, as the shaders do. The same sentinel when disabled.
        ///
        /// @p floorPx is the caller's best statement of the shader's floor in
        /// the same units as @c size. It cannot always be exact - the direct
        /// paths floor at fwidth(d), 1 px on a straight edge and up to 1.41 on
        /// a diagonal, which only the GPU sees - so callers carry slack on top.
        inline float GetCutoffEnd(const Cutoff &c, float floorPx)
        {
            const float softness = std::max(c.softness, 0.0f);
            return c.enable ? c.size + 0.5f * softness + 0.5f * std::max(softness, floorPx)
                            : CUTOFF_DISABLED_SIZE;
        }

        /// Slack, in full-res px, on the bounds @ref NeonRenderer::setupGeometry
        /// derives for the glow quad's inner hole and for its outer edge under
        /// GlowSide::INSIDE.
        ///
        /// These bounds say where the shader's own discards stop drawing, and
        /// they are computed from the same expressions - but from a nominal
        /// one-pixel `fwidth`, where the shader's is 1.0 to 1.41 depending on
        /// which way the boundary faces. Rounding outward by a few px is free:
        /// the extra fragments shade to coverage 0, which this pass's
        /// premultiplied blend leaves the destination untouched by. Rounding
        /// inward would clip the glow, so this errs outward on purpose, exactly
        /// as FILL_EDGE_SAFETY does in @ref NeonRenderer::setupFillGeometry.
        ///
        /// NOT applied to the outside-cutoff cap, which keeps its own +1: that
        /// one feeds the shader's fadeStart floor through uQuadMargin, so its
        /// value is load-bearing beyond bounding the rasteriser.
        constexpr float GLOW_EDGE_SAFETY = 3.0f;

        /// Does this config's opaque fill cover EVERY pixel at coverage 1?
        ///
        /// The question the fill passes actually need answered is not "which
        /// OpaqueMode did the host pick" but "is the coverage uniformly 1",
        /// because that is what decides between a scissored glClear and a
        /// shaded draw. ALL says so by definition. BOTH says so too whenever
        /// NEITHER of the fill's cutoffs is enabled - and since both default to
        /// disabled (@ref NeonConfig::opaqueInsideCutoff / opaqueOutsideCutoff),
        /// that is the state a host lands in by simply selecting BOTH. The
        /// glow's own cutoffs play no part: they no longer shape the fill.
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
                    !neon.opaqueInsideCutoff.enable && !neon.opaqueOutsideCutoff.enable);
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

        /// One candidate texture format for a buffer that walks a list - the
        /// emission table and the gather buffer.
        typedef struct TargetFormat
        {
            GLint internalFormat;
            GLenum format;
            GLenum type;
            const char *name; ///< For the fallback log line.
        } TargetFormat;

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
        /// WHY ONLY THIS BUFFER AND GATHER_FORMATS HAVE A LIST. They are the
        /// only two that ask for a format a conforming driver may refuse. RGBA8
        /// - what mScaledBuffer, LensFlareRenderer's scaled buffer and
        /// OffscreenCapture all take - is mandatory colour-renderable in both
        /// GL 3.3 core and GLES 3.0, so there is nothing for those to fall
        /// back FROM, and nothing to fall back TO either: an RGBA8 failure is
        /// out-of-memory or a broken driver, which no other format fixes. They
        /// bail instead, and should.
        ///
        /// They are also the only two that WANT float. This one stores arc
        /// intensity and stacked segment boosts, which routinely exceed 1.0;
        /// the gather buffer stores data that a shading pass re-amplifies. The
        /// others store composited output - premultiplied colour after
        /// tone-mapping, in [0, 1] - where 8 bits is the right storage rather
        /// than a compromise. (8 bits is not free there; see R7 in
        /// docs/review-findings.md on halo/bloom contour banding. If that is
        /// ever fixed with a float target rather than a dither, this walk
        /// generalises to those buffers unchanged.)
        constexpr TargetFormat EMISSION_FORMATS[] = {
            {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, "RGBA16F"},
            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "RGBA8"},
        };

        /// Gather-buffer formats in PREFERENCE ORDER, best first - walked by
        /// @ref NeonRenderer::resizeGatherBuffer.
        ///
        /// RGBA16F leads because the gather's results are DATA that pass 1 and
        /// the edge ring shade from - multiplied by intensity, the falloffs and
        /// the bloom, then tone-mapped - so a storage step is amplified, not
        /// hidden. Measured on the twelve scenes of `probe scenes`
        /// (docs/neon-resolution-scale-plan.md section 13): RGBA8 holds 3/255
        /// of the 1.0 render, RGBA16F 1-2, better than the 2-3 of the edge
        /// ring before the gather was split out, which also stored it in
        /// RGBA8. The buffer is usually one or two hundred texels a side
        /// (0.08-0.4 MB at 1080p), so the doubled texel is cheap; the dear
        /// case is a rect small enough to gather at resolutionScale itself
        /// (1.1 MB for a 120 x 80 rect at 0.5). Half float is texture-filterable
        /// in GLES 3.0 core; only RENDERING to it needs an extension, which is
        /// what the RGBA8 row is for.
        constexpr TargetFormat GATHER_FORMATS[] = {
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

        /// Whether this config gives @c mScaledBuffer anything to do.
        ///
        /// ONE predicate for two questions that have to agree: @ref Render asks
        /// it to pick the path, and @ref OnConfigChanged asks it to decide
        /// whether the buffer may be freed. Answer them separately and they
        /// drift - the failure being a release of the buffer the very pass that
        /// needs it is about to bind, which Resize would then quietly rebuild
        /// once per frame.
        ///
        /// Both terms matter, and for the same reason: the buffer is
        /// @c width * height * 4 bytes and neither a disabled layer nor the
        /// direct path ever reads it. Only @c enable is a genuine gate on the
        /// PASS, though - @ref Render returns on it before the scale is even
        /// clamped - so inside Render, past that return, this is exactly
        /// @c scale < 1.0.
        inline bool UsesScaledBuffer(const Config &config)
        {
            return config.neon.enable && GetClampedResolutionScale(config) < 1.0f;
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

        /// @p src with `#define @p define` spliced in after its first line.
        ///
        /// How one shader file yields several programs. Every embedded source
        /// starts with the @GLSL_VERSION@ line, which must stay first, so the
        /// define goes immediately after it - ahead of everything else, the
        /// injected tuning header included. See @ref NeonRenderer::setupShaders
        /// for the variants built this way.
        inline std::string WithDefine(const char *src, const char *define)
        {
            std::string out(src);
            const size_t eol = out.find('\n');
            out.insert(eol == std::string::npos ? out.size() : eol + 1,
                       std::string("#define ") + define + "\n");
            return out;
        }

        /// Floor, in FULL-RES px, under every glow cutoff ramp's width. Mirrors
        /// neon.frag's softFloor, which is what actually decides how far past a
        /// cutoff the feather runs and therefore how much quad the shader needs.
        /// Shared by the glow quad's outer cap (@ref GetGlowMargin) and its
        /// inner hole. Leave it out and the quad edge lands inside the feather,
        /// which is exactly the rectangular seam the +1 safety exists to prevent.
        ///
        /// ONE DESTINATION PIXEL ON BOTH PATHS. Below scale 1.0 the blit draws
        /// the cutoff masks at destination resolution and neon.frag only places
        /// its discards to agree with it, so the floor is a destination pixel
        /// there too. (It was CUTOFF_SOFT_FLOOR_PX / scale while the masks were
        /// drawn into the buffer.)
        ///
        /// Nominal, because the shader's is `sideAA` - fwidth(d), so it runs
        /// 1.0 to 1.41 depending on which way the boundary faces and cannot be
        /// known here. Under-stating it by the diagonal factor is absorbed by
        /// the safety margins: the fade can end up to ~0.2 px past what
        /// GetCutoffEnd reports on a diagonal, well inside the outer cap's
        /// 1 px and GLOW_EDGE_SAFETY's 3. Conservative in the direction that
        /// keeps the quad covering the band.
        constexpr float CUTOFF_FLOOR_PX = 1.0f;

        /// How far past the END of a cutoff's ramp neon.frag still draws, in
        /// FULL-RES px. Mirrors its `cutGuard`: nothing on the direct path,
        /// which culls where its masks end, and on the scaled path the guard
        /// band the blit rebuilds the boundary from - stated in BUFFER px, so it
        /// divides back out. The outer cap and the inner hole below both carry
        /// it; leave it off either and the quad clips the texels the blit needs.
        inline float GetCutoffGuardPx(float scale)
        {
            return (scale < 1.0f) ? (static_cast<float>(BLIT_CUTOFF_GUARD_PX) / scale) : 0.0f;
        }

        /// The filament's half-width @c sigma and the distance @c reach where
        /// it is gone, in the px space of @p scale - buffer px below 1.0, the
        /// full-res values at 1.0 - exactly as neon.frag computes them there.
        typedef struct FilamentExtent
        {
            float sigma; ///< Half-brightness radius, floored as neon.frag floors it.
            float reach; ///< sigma times the falloff's reach in sigmas.
        } FilamentExtent;

        /// @ref FilamentExtent for @p config at @p scale. The glow quad needs
        /// it to clear the filament (@ref GetGlowMargin), and the edge ring
        /// sizes itself from the full-res one (@ref NeonRenderer::setupRingGeometry).
        /// One function so the two cannot drift from each other or from
        /// neon.frag, whose sigma / reachSigmas this mirrors.
        inline FilamentExtent GetFilamentExtent(const Config &config, float scale)
        {
            // The filament is sized by lineWidth, not glowRadius, so the quad must
            // clear it as well as the glow. Without it, glowRadius = 0 ("filament only")
            // produced a zero margin: the quad landed exactly on the rect and clipped
            // every exterior fragment, so the outside half of the filament
            // disappeared while the inside half stayed. Mirrors the shader's `sigma`.
            // Reach in sigmas depends on the falloff exponent, not a constant - a
            // soft filament's tail runs for hundreds of sigmas. Same expression as
            // the shaders' reachSigmas; see neon-tuning.h.
            const float filN = 2.0f * std::max(config.neon.filamentFalloff, 1e-3f);
            const float filSigmas = std::clamp(
                std::pow(std::log2(float(FILAMENT_GAIN) / float(FILAMENT_CUTOFF)), 1.0f / filN),
                float(FILAMENT_REACH_MIN_SIGMAS), float(FILAMENT_REACH_MAX_SIGMAS));
            // FILAMENT_MIN_HALF_WIDTH is a full-res constant, so the stated half
            // width is taken in full-res px and scaled once - the same conversion
            // the shader does with uResolutionScale.
            //
            // The SAMPLING floor is then applied in BUFFER px, after the scale,
            // exactly as the shader's sigma does it: it is a property of the
            // buffer, not of the width the caller asked for. Scale it here too and
            // the quad is sized for a thinner filament than the shader draws,
            // which clips the outside half of a thin line at a reduced scale -
            // the same failure the lineWidth floor itself exists to prevent.
            //
            // It is not a fixed half width: what survives the blit depends on the
            // profile's SHAPE as well as its width, so the floor is stated as "the
            // profile must still be at FILAMENT_NYQUIST_MIN_SHARE of its peak
            // FILAMENT_NYQUIST_SAMPLE_PX out" and inverted for sigma. Mirrors
            // neon.frag exactly, gate included - at scale 1.0 there is no blit to
            // survive and the direct path must stay bit-identical. Change one and
            // the quad stops matching what the shader draws. See neon-tuning.h.
            const float filamentNyquist =
                (scale < 1.0f) ? (static_cast<float>(FILAMENT_NYQUIST_SAMPLE_PX) /
                                  std::pow(std::log2(1.0f / static_cast<float>(FILAMENT_NYQUIST_MIN_SHARE)),
                                           1.0f / filN))
                               : 0.0f;
            const float filamentSigma = std::max(
                std::max(config.neon.lineWidth * 0.5f, float(FILAMENT_MIN_HALF_WIDTH)) * scale,
                filamentNyquist);
            return {filamentSigma, filamentSigma * filSigmas};
        }

        /// The edge ring's half-width R, in FULL-RES px, at the scale @p scale
        /// is drawn at: how far either side of the rect edge the scaled path
        /// re-shades at full resolution. Meaningless at 1.0, where no ring is
        /// drawn.
        ///
        /// It has to cover the filament at BOTH resolutions: the real one the
        /// ring draws, and the one pass 1 drew into the reduced buffer, which is
        /// floored to what that buffer can sample and so reaches further
        /// whenever the line is thin - a 1 px line at scale 0.25 reaches 1 px at
        /// full res but 5.7 px as pass 1 drew it, and a ring sized for the
        /// first left the second showing just outside it. Each reach is where
        /// that filament has fallen to FILAMENT_CUTOFF of its gain, about 2/255
        /// after the grade, so the reach itself is the right width and nothing
        /// is capped. RING_GUARD_TEXELS then adds the reduced buffer's bilinear
        /// footprint. See neon-tuning.h for the calibration.
        ///
        /// Asked by @ref NeonRenderer::setupRingGeometry to build the ring (and,
        /// through it, the gather pass's quad, which covers the ring).
        inline float GetRingWidth(const Config &config, float scale)
        {
            const float fullReach = GetFilamentExtent(config, 1.0f).reach;
            const float reducedReach = GetFilamentExtent(config, scale).reach / scale; // buffer px -> full-res
            return std::max(fullReach, reducedReach) + static_cast<float>(RING_GUARD_TEXELS) / scale;
        }

        /// How far past the rect edge the glow quad reaches, in the px space
        /// of @p scale: the margin @ref NeonRenderer::setupGeometry builds the
        /// quad with and hands neon.frag as uQuadMargin. At @p scale 1.0 it is
        /// the direct path's margin, which the edge ring fades against.
        ///
        /// Size the quad to cover the lit region: rect + glowReach, so geometry
        /// bounds the far region instead of a per-fragment discard
        /// (tiler-friendly). Factors come from the shared neon-tuning.h.
        ///
        /// glowRadius ONLY - deliberately not the old
        /// max(glowRadius * RADIUS, sampleSpacing * SPACING). sampleSpacing is
        /// perimeter / NEON_MAX_LOOP_SAMPLES, so the spacing term won on any
        /// reasonably large rect at default glowRadius and made the quad - and
        /// with it the distance the bloom got truncated at, and the brightness
        /// it still had there - track the rect size: the fade began at 250 px on
        /// a 200x150 rect and 1542 px on a 1920x1080 one. The bloom's reach is a
        /// function of glowRadius and nothing else, so that is all that sizes the
        /// quad now. It also caps the worst case: the old spacing term asked for
        /// a ~5800x4900 px quad on a 1920x1080 rect.
        ///
        /// The wide bloom (1/D tail) stays visible further out as bloomStrength /
        /// intensity rise, so grow the quad with them. The shader reproduces this
        /// exact expression to place its bloom pedestal, which is what lets the
        /// margin stay this tight without the truncation showing - keep the two
        /// in step.
        inline float GetGlowMargin(const Config &config, float scale)
        {
            const float glowReach = config.neon.glowRadius * scale * float(GLOW_REACH_RADIUS_FACTOR) *
                                    (1.0f + config.neon.bloomStrength * config.neon.intensity);

            float margin = std::max(glowReach, GetFilamentExtent(config, scale).reach);

            // Hard cap: when the outside cutoff is enabled the shader discards
            // emission past size + softness (the feather starts at size - see
            // GetCutoffEnd), so there's no point rasterising further. Disabled
            // outside cutoff leaves the natural glowRadius / bloom-driven margin
            // untouched. Add a 1 px safety so the shader's own softmask fades to
            // zero *before* the quad edge and no rectangular seam leaks through.
            //
            // The WHOLE expression is built in full-res px and scaled once, so the
            // safety margin is 1 FULL-RES px at every resolution scale. Adding the
            // +1 after the scale instead makes it 1 buffer px - 2 full-res px at
            // scale 0.5 - which pushes the quad edge out and with it the ramp the
            // shader fits between the cutoff boundary and uQuadMargin. Same units
            // on both sides is also what makes the shader's fadeStart floor engage
            // at the same cutoff size regardless of scale.
            //
            // Skipped under GlowSide::INSIDE, where neon.frag neutralises this
            // cutoff as subsumed by the cut - see the band-distance block there. A
            // cap derived from a mask the shader no longer applies would bound the
            // quad to a region the shader still lights, and on the scaled path the
            // region it would eat is the one-sided cut's guard band
            // (BLIT_SIDE_GUARD_PX), which the blit rebuilds that cut from. A small
            // enough cutoff lands inside it.
            if (config.neon.outsideCutoff.enable && config.neon.glowSide != GlowSide::INSIDE)
            {
                // Where neon.frag's outside fade ends, plus the scaled path's guard
                // band, plus the 1 px safety. The capped margin becomes
                // uQuadMargin, and neon.frag's fadeStart floor needs its cutEdge -
                // that same end, guard included - to sit strictly inside it; the +1
                // is what guarantees that. Drop it and the quad fade starts at
                // 0.8 * margin, inside the band.
                const float cutoffCap =
                    (GetCutoffEnd(config.neon.outsideCutoff, CUTOFF_FLOOR_PX) + GetCutoffGuardPx(scale) + 1.0f) * scale;
                margin = std::min(margin, cutoffCap);
            }
            return margin;
        }

        /// 1 - 1/sqrt(2): how far a rounded box's corner arc pulls the largest
        /// axis-aligned rectangle inside it in from the box's half-extents, per
        /// px of corner radius. That rectangle touches the arc at 45 degrees.
        constexpr float CORNER_INSET_FACTOR = 0.2928932f;

        /// Half-extents of the axis-aligned box that CONTAINS every point
        /// within signed distance @p dist of a rounded box with half-extents
        /// (@p hw, @p hh): the box itself grown by @p dist (shrunk for a
        /// negative one). Clamped at 0.
        inline glm::vec2 CircumscribedBox(float hw, float hh, float dist)
        {
            return glm::max(glm::vec2(hw + dist, hh + dist), glm::vec2(0.0f));
        }

        /// Half-extents of an axis-aligned box CONTAINED in the set of points
        /// within signed distance @p dist of a rounded box (@p hw, @p hh,
        /// corner radius @p r): that set is a rounded box grown by @p dist with
        /// its radius grown to match, and its largest inscribed rectangle clears
        /// the corner arc by CORNER_INSET_FACTOR of that radius. The hole
        /// construction setupFillGeometry and the glow quad have always used,
        /// for either sign of @p dist. Clamped at 0.
        inline glm::vec2 InscribedBox(float hw, float hh, float r, float dist)
        {
            const float inset = std::max(r + dist, 0.0f) * CORNER_INSET_FACTOR;
            return glm::max(glm::vec2(hw + dist - inset, hh + dist - inset), glm::vec2(0.0f));
        }

        /// Append the two triangles of the rectangle [l, r] x [b, t], wound
        /// counter-clockwise like every other strip in this renderer.
        inline void PushRect(std::vector<float> &v, float l, float b, float r, float t)
        {
            const float quad[] = {l, t, l, b, r, b, l, t, r, b, r, t};
            v.insert(v.end(), std::begin(quad), std::end(quad));
        }

        /// Append the region inside the centred box @p outer and outside the
        /// centred box @p inner (both half-extents) as up to four rectangles
        /// that tile it without overlapping: top and bottom full width, left
        /// and right across the inner box's height only. @p inner is clipped
        /// to @p outer first, so this is the exact set difference whatever the
        /// two are - nothing for an empty @p outer, all of it for an empty
        /// @p inner.
        ///
        /// Every edge comes straight from the two boxes' own floats, so two
        /// calls that share a box share its edges bit for bit. That is what
        /// lets NeonRenderer::setupRingGeometry split the screen between the
        /// ring and the blit with no pixel drawn by both and none by neither.
        inline void PushAnnulus(std::vector<float> &v, glm::vec2 outer, glm::vec2 inner)
        {
            if (outer.x <= 0.0f || outer.y <= 0.0f)
            {
                return;
            }
            const float ix = std::min(inner.x, outer.x);
            const float iy = std::min(inner.y, outer.y);
            if (ix <= 0.0f || iy <= 0.0f)
            {
                PushRect(v, -outer.x, -outer.y, outer.x, outer.y);
                return;
            }
            if (iy < outer.y)
            {
                PushRect(v, -outer.x, iy, outer.x, outer.y);   // top: full width
                PushRect(v, -outer.x, -outer.y, outer.x, -iy); // bottom: full width
            }
            if (ix < outer.x)
            {
                PushRect(v, -outer.x, -iy, -ix, iy); // left: across the inner box only
                PushRect(v, ix, -iy, outer.x, iy);   // right
            }
        }

        /// How far either side of the rect edge the glow can still be non-zero
        /// once the one-sided cut and the cutoffs have been applied, in
        /// full-res px: @c in toward the centre, @c out away from it.
        typedef struct EdgeExtent
        {
            float in;  ///< Inward reach; CUTOFF_DISABLED_SIZE when nothing bounds it.
            float out; ///< Outward reach; CUTOFF_DISABLED_SIZE when nothing bounds it.
        } EdgeExtent;

        /// Slack, in full-res px, on @ref GetLitExtent: the half pixel the
        /// one-sided cut reaches back across the line (up to 0.71 px on a
        /// diagonal, where fwidth is 1.41), the ~0.2 px a diagonal moves a
        /// cutoff ramp's end past what CUTOFF_FLOOR_PX predicts, and rounding.
        constexpr float LIT_EDGE_SAFETY = 2.0f;

        /// The glow's lit band, @ref EdgeExtent. Mirrors exactly what the
        /// masks in neon.frag and neon-blit.frag take to an EXACT zero: past
        /// the cut, `back` (half a destination pixel) on the side glowSide
        /// culls; past a cutoff, the end of its fade (@ref GetCutoffEnd). A
        /// cutoff on the side glowSide culls is neutralised exactly as those
        /// shaders neutralise it. So beyond this extent, plus LIT_EDGE_SAFETY,
        /// every pass that composites the glow writes 0 - and drawing there is
        /// pure cost, which is what @ref NeonRenderer::setupRingGeometry stops
        /// paying for.
        inline EdgeExtent GetLitExtent(const Config &config)
        {
            const NeonConfig &neon = config.neon;
            EdgeExtent lit{CUTOFF_DISABLED_SIZE, CUTOFF_DISABLED_SIZE};
            if (neon.glowSide == GlowSide::OUTSIDE)
            {
                lit.in = 0.0f;
            }
            else if (neon.insideCutoff.enable)
            {
                lit.in = GetCutoffEnd(neon.insideCutoff, CUTOFF_FLOOR_PX);
            }
            if (neon.glowSide == GlowSide::INSIDE)
            {
                lit.out = 0.0f;
            }
            else if (neon.outsideCutoff.enable)
            {
                lit.out = GetCutoffEnd(neon.outsideCutoff, CUTOFF_FLOOR_PX);
            }
            return lit;
        }

        /// The edge ring's extent either side of the rect edge, in full-res px:
        /// @ref GetRingWidth, clipped to the lit band (@ref GetLitExtent plus
        /// LIT_EDGE_SAFETY). Past the lit band the ring would only shade
        /// pixels its own masks then take to zero - and the blit, which gets
        /// them instead, takes them to zero too - so the clip changes no
        /// pixel. What it changes is the bill: a one-sided glow drops the half
        /// of the ring on its dark side, and a cutoff band narrower than the
        /// ring drops what lies past the band.
        inline EdgeExtent GetRingExtent(const Config &config, float scale)
        {
            const float ringWidth = GetRingWidth(config, scale);
            const EdgeExtent lit = GetLitExtent(config);
            return {std::min(ringWidth, lit.in + LIT_EDGE_SAFETY), std::min(ringWidth, lit.out + LIT_EDGE_SAFETY)};
        }

        /// How many reduced-buffer texels the blit's bilinear read can reach
        /// past a boundary: one for the filter, one for the sub-0.2% density
        /// difference between the buffer and the exact scaled viewport and for
        /// rounding. Sizes the blit's outer frame past the glow's fade.
        constexpr float FOOTPRINT_TEXELS = 2.0f;

        /// The scale the scaled path's GATHER runs at: as coarse as the
        /// gather's own smoothness allows, never finer than @p scale.
        ///
        /// The gather produces the perimeter hue and the two gathered coverages,
        /// each a Lorentzian-weighted mean over the whole perimeter with kernel
        /// width kc = perimeter * COLOR_BLEND_PERIM_FRAC (neon.frag's `kc`) -
        /// and nowhere narrower: at a distance d from the line the kernel is
        /// sqrt(d^2 + kc^2) wide. So a grid GATHER_TEXELS_PER_KERNEL texels per
        /// kc carries it through a bilinear read, which the edge ring already
        /// relies on, and the loop - ~95% of the neon's cost - runs on that grid
        /// instead of on every texel the shading needs. Floored at
        /// GATHER_MIN_SCALE so a very large rect does not pin a viewport-sized
        /// gather to a handful of texels. See neon-tuning.h for the calibration.
        ///
        /// A function of the geometry and @p scale only, so it moves only under
        /// geometryDirty, which rebuilds the gather pass's quad with it. It
        /// moves continuously under a size animation; GetBufferRegion is what
        /// keeps that from reallocating the buffer every frame.
        inline float GetGatherScale(const Config &config, float scale)
        {
            const float w = config.geometry.width;
            const float h = config.geometry.height;
            const float r = std::clamp(GeometryUtils::GetEffectiveCornerRadius(config.geometry), 0.0f,
                                       std::min(w, h) * 0.5f);
            const float perimeter = 2.0f * (w + h - 4.0f * r) + glm::two_pi<float>() * r;
            const float kc = std::max(perimeter * static_cast<float>(COLOR_BLEND_PERIM_FRAC),
                                      static_cast<float>(EMISSION_MIN_WIDTH));
            const float floorScale = std::min(static_cast<float>(GATHER_MIN_SCALE), scale);
            return std::clamp(static_cast<float>(GATHER_TEXELS_PER_KERNEL) / kc, floorScale, scale);
        }

        /// A buffer's size in texels at @p scale of a @p viewport px axis:
        /// truncated, and at least one - what a buffer covering the whole
        /// viewport at @p scale holds. GetBufferRegion never sizes the reduced
        /// buffer past it.
        inline int ScaledExtent(int viewport, float scale)
        {
            return std::max(static_cast<int>(static_cast<float>(viewport) * scale), 1);
        }

        /// An offscreen buffer bounded to a region is sized per axis to a
        /// multiple of this many texels. The region follows the rect's size
        /// and, for a rect partly off screen, its position; rounded, an
        /// animation of either reallocates the buffer once per this many texels
        /// of change instead of on every frame (Framebuffer::Resize
        /// reallocates on any size change).
        constexpr int REGION_ALLOC_STEP = 16;

        /// The part of the frame an offscreen buffer covers, and its size.
        typedef struct BufferRegion
        {
            glm::vec2 origin;  ///< Lower-left corner, rect-local FULL-RES px (y up).
            glm::vec2 size;    ///< Full-res px.
            glm::ivec2 texels; ///< The buffer's size.
        } BufferRegion;

        /// Where a buffer at @p scale has to sit for its readers: the box
        /// @p outer (half-extents in full-res px about the rect centre
        /// @p centerFull) clipped to the viewport - grown past the viewport's
        /// edge by the bilinear footprint (FOOTPRINT_TEXELS), since a fragment
        /// on the edge reads that far.
        ///
        /// Both scaled-path buffers used to cover the whole viewport, whatever
        /// the rect. The reduced buffer is read only by the blit, inside the
        /// lit band, and the gather buffer only by pass 1 and the ring; a rect
        /// smaller than the screen needed a fraction of either. The gather
        /// buffer was the costly one: a rect small enough to gather at
        /// resolutionScale itself gathered into a whole viewport at that scale,
        /// RGBA16F, with two attachments under segments - four times the
        /// reduced buffer.
        ///
        /// With @p capToViewport, PER AXIS never more than the viewport-sized
        /// buffer: when the rounded region would reach @ref ScaledExtent
        /// texels the axis falls back to exactly that buffer - the whole
        /// viewport, @c floor(viewport * scale) texels drawn through an ortho
        /// over the EXACT scaled extent, viewport * scale - so a full-screen
        /// rect keeps the reduced buffer, and the output, it always had.
        ///
        /// The exact extent there is a global stretch, not a rounding detail.
        /// Map the truncated texel count instead and the round trip (draw at
        /// x * scale, blit back over the viewport) scales everything by
        /// viewport * scale / floor(viewport * scale), pushing it outward from
        /// the viewport origin by an amount that grows with distance from it:
        /// measured on a 1234 px viewport at scale 0.7, the outside-cutoff
        /// boundary landed 0.9 px past its stated size. With the exact extent
        /// the buffer just samples at floor(viewport * scale) / viewport rather
        /// than at `scale`, a sub-0.2% density difference.
        ///
        /// NOT for the gather buffer: that fallback ends the buffer AT the
        /// viewport's edge, where a reader's bilinear footprint clamps to the
        /// last texel - harmless at the reduced buffer's pitch, but a gather
        /// texel is 1 / gatherScale px (11 px for an 800 x 500 rect), and a
        /// rect running off the bottom of the frame read 5/255 off 1.0 along
        /// the last rows from holding one gather value across them.
        ///
        /// Otherwise the texel grid stays where the viewport-sized buffer put
        /// it - texel centres (i + 0.5) / @p scale from the viewport's corner -
        /// so a reader still lands on the same texel centres. The texel COUNT
        /// comes from the unsnapped box plus one, which covers the box whatever
        /// its offset from that grid, so a rect that moves without reaching
        /// the viewport's edge keeps the same buffer. Rounding it up to
        /// REGION_ALLOC_STEP grows the region on its upper side; the texels
        /// past what was asked for are cleared and never read.
        inline BufferRegion GetBufferRegion(const glm::vec2 &outer, const glm::vec2 &centerFull,
                                            int viewportWidth, int viewportHeight, float scale,
                                            bool capToViewport)
        {
            const float pitch = 1.0f / scale;
            const glm::ivec2 viewportPx(viewportWidth, viewportHeight);
            const glm::vec2 viewport(viewportPx);
            const glm::vec2 reach(FOOTPRINT_TEXELS * pitch);
            // In viewport px from its lower-left corner, y up.
            const glm::vec2 lo = glm::max(centerFull - outer, -reach);
            const glm::vec2 hi = glm::min(centerFull + outer, viewport + reach);
            BufferRegion region;
            for (int axis = 0; axis < 2; ++axis)
            {
                // At least one texel even when the box misses the viewport
                // entirely: nothing reads it then, but the passes still bind it.
                const float span = std::max(hi[axis] - lo[axis], 0.0f);
                const int wanted = static_cast<int>(std::ceil(span / pitch)) + 1;
                const int rounded = (wanted + REGION_ALLOC_STEP - 1) / REGION_ALLOC_STEP * REGION_ALLOC_STEP;
                const int whole = ScaledExtent(viewportPx[axis], scale);
                if (capToViewport && rounded >= whole)
                {
                    region.origin[axis] = -centerFull[axis];
                    region.size[axis] = viewport[axis];
                    region.texels[axis] = whole;
                }
                else
                {
                    region.origin[axis] = std::floor(lo[axis] / pitch) * pitch - centerFull[axis];
                    region.size[axis] = static_cast<float>(rounded) * pitch;
                    region.texels[axis] = rounded;
                }
            }
            return region;
        }

        /// The ortho that draws @p region, at @p scale, from rect-local SCALED
        /// px - the space every scaled-path quad is built in.
        inline glm::mat4 RegionProjection(const BufferRegion &region, float scale)
        {
            const glm::vec2 lo = region.origin * scale;
            const glm::vec2 hi = (region.origin + region.size) * scale;
            return glm::ortho(lo.x, hi.x, lo.y, hi.y, -1.0f, 1.0f);
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
        mRingVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        mBlitVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);
        mGatherVertexArray.SetAttribPointer(0, 2, GL_FLOAT, 2 * sizeof(float), 0);

        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        setupFillGeometry(mCurrentConfig);
        // After setupGeometry: the blit's outer frame reads the glow margin it
        // computes.
        setupRingGeometry(mCurrentConfig);
        // The atlas bakes read the merged transient+preserved view, which
        // OnConfigChanged normally keeps current; seed it here for the first.
        SegmentUtils::FillEffectiveSegments(mCurrentConfig.neon, mEffectiveSegments);
        bakeLUTs(mCurrentConfig);

        setupFullscreenQuad();
        mInitialized = true;
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
        // TWO PHASES, in this order on both paths: everything that renders
        // OFFSCREEN first (the emission table, and below 1.0 the gather and
        // pass 1), then everything that lands on the caller's
        // framebuffer (the fill, then the glow - pass 1 itself at 1.0, the
        // blit and the edge ring below it). So the caller's target is drawn in
        // one unbroken run per frame. On a tile-based GPU every switch away
        // from it and back stores its tiles out and loads them in again, and
        // the fill used to go first and force exactly that whenever an
        // offscreen pass followed it. The fill depends on nothing offscreen
        // and only has to land under the glow, so moving it costs nothing.
        const float scale = GetClampedResolutionScale(config);
        // Past the enable return above, this is `scale < 1.0` - asked through
        // the shared predicate so it cannot disagree with the release gate in
        // OnConfigChanged about which configs want the buffer.
        const bool scaled = UsesScaledBuffer(config);
        const int bufW = ScaledExtent(viewportWidth, scale);
        const int bufH = ScaledExtent(viewportHeight, scale);

        // The DIRECT path's transform: the full-res ortho over the viewport,
        // rect centre translated in. bufW/bufH are the viewport and the extent
        // is exactly it at scale 1.0, which is what makes the direct path
        // identical to the dedicated full-res renderer this class replaced.
        // The scaled path's two offscreen passes draw through their own
        // region projections instead (GetBufferRegion, RegionProjection),
        // still in SCALED space, so the quads, the rect size and the loop
        // samples all agree; the passes on the caller's framebuffer build
        // their own full-res transforms.
        //
        // Viewport y runs down in Config but up in the projection, so the
        // centre is mirrored about the viewport height.
        //
        // The max() only guards a zero viewport from reaching glm::ortho as an
        // empty range - bufW's own max(1) used to cover that.
        const float halfRectW = config.geometry.width * 0.5f;
        const float halfRectH = config.geometry.height * 0.5f;
        const glm::vec2 extent(std::max(static_cast<float>(viewportWidth) * scale, 1.0e-3f),
                               std::max(static_cast<float>(viewportHeight) * scale, 1.0e-3f));
        const glm::mat4 proj = glm::ortho(0.0f, extent.x, 0.0f, extent.y, -1.0f, 1.0f);
        const glm::vec2 centerFull(config.geometry.position.x + halfRectW,
                                   static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        const glm::vec2 center = centerFull * scale;
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));

        // Debug: the fill and nothing else, on both paths. What lands on
        // screen is the opaque silhouette by itself - which is how the fill's
        // square corner at cornerRadius 0 gets compared against the emission's
        // round one. DebugRenderer honours the same flag, so the overlays do
        // not reappear over a fill-only frame.
        //
        // The one field this renderer reads out of DebugConfig. It lives there
        // because it is a debug control, and it is read HERE because it is a
        // mode of this renderer's pass schedule rather than something another
        // layer can draw - no other renderer can decline to run these passes.
        // Ahead of everything else, so fill-only mode compiles no neon program,
        // uploads no UBO and bakes no table it would never sample.
        if (config.debug.opaqueOnly)
        {
            if (config.neon.opaqueMode != OpaqueMode::NONE)
            {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                renderOpaqueFill(viewportWidth, viewportHeight, config);
            }
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            return;
        }

        // The programs this path draws with, compiled the first frame it
        // runs - see ensurePathPrograms. A failure is logged once and the
        // frame degrades to the fill: no glow, nothing stale.
        bool glowReady = ensurePathPrograms(scaled);

        // The render target this renderer was handed - framebuffer AND
        // viewport, saved as a pair because the offscreen phase has to put both
        // back. The framebuffer is not always the window's: an offscreen frame
        // capture (@ref OffscreenCapture) binds a real FBO, so a retargeting
        // pass must return to what was bound rather than assuming 0. Read
        // BEFORE any pass binds a target of its own - querying later would
        // capture that.
        //
        // SCALED PATH ONLY, because it is the only one whose passes this
        // function retargets: on the direct path pass 1 draws straight onto the
        // caller's framebuffer and there is nothing to come back to.
        //
        // @ref renderEmissionPass captures its own rather than being handed
        // this one: a pass restores what IT finds, which is what keeps it
        // correct wherever it is called from.
        RenderTargetState prevTarget;
        // Full-res rect-local px -> buffer uv, for the passes on the caller's
        // framebuffer: the edge ring's onto the gather buffer and the blit's
        // onto the reduced one. Set with the regions below.
        glm::vec2 gatherUVFullScale(0.0f);
        glm::vec2 gatherUVFullOffset(0.0f);
        glm::vec2 blitUVScale(0.0f);
        glm::vec2 blitUVOffset(0.0f);
        if (scaled)
        {
            prevTarget = RenderTargetState::Capture();
        }

        // ===== Offscreen phase ===============================================
        if (glowReady)
        {
            // --- Pass 0: per-sample emission table --------------------------
            packLightBlocks(config);
            // ...and only re-bake the table when something it reads has
            // actually moved. The buffer is allocated once and nothing else
            // writes it, so a frame that changes neither the config nor (at a
            // non-zero hue rate) the time reads the same texels the last bake
            // left. A still ring therefore costs one FBO bind, eight uniform
            // sets, three texture binds and a draw on the frame it changes, and
            // nothing on the frames after.
            if (isEmissionTableStale(time, config))
            {
                // A table write is not a composite: blending would mix this
                // frame's emission into last frame's. Every later pass sets its
                // own blend mode, so leaving this off changes nothing
                // downstream.
                glDisable(GL_BLEND);
                renderEmissionPass(viewportWidth, viewportHeight, time, config);
            }

            if (scaled)
            {
                // Both scaled-path buffers cover a REGION of the frame, not
                // the viewport (GetBufferRegion): each pass draws through an
                // ortho onto its region, in pass 1's scaled rect-local space -
                // where every scaled quad lives - and each reader maps its own
                // rect-local px onto the buffer's uv.
                //
                // --- Pass 1a: the gather, at its own coarse scale, into the
                // gather buffer. Unblended: the buffer is data. Read by pass 1b
                // from scaled px and by the ring from full-res px.
                const BufferRegion gatherRegion = GetBufferRegion(
                    mGatherOuter, centerFull, viewportWidth, viewportHeight, GetGatherScale(config, scale), false);
                const glm::vec2 gatherUVOffset = -gatherRegion.origin / gatherRegion.size;
                const glm::vec2 gatherUVScale = glm::vec2(1.0f) / (gatherRegion.size * scale);
                gatherUVFullScale = glm::vec2(1.0f) / gatherRegion.size;
                gatherUVFullOffset = gatherUVOffset;
                glDisable(GL_BLEND);
                glowReady = renderGatherPass(RegionProjection(gatherRegion, scale), gatherRegion.texels.x,
                                             gatherRegion.texels.y, scale, config);

                // --- Pass 1b: the shading, at the reduced scale, into the
                // reduced buffer, from that gather. Still unblended: the buffer
                // was just cleared and the quad covers each texel once, so
                // premultiplied-over would only add zero, at the price of a
                // destination read per texel. A failed allocation in either
                // pass skips both composites below, so a failed frame degrades
                // to the fill rather than compositing a stale buffer from an
                // earlier frame. Read by the blit, from full-res px.
                if (glowReady)
                {
                    const BufferRegion scaledRegion =
                        GetBufferRegion(mScaledOuter, centerFull, viewportWidth, viewportHeight, scale, true);
                    blitUVScale = glm::vec2(1.0f) / scaledRegion.size;
                    blitUVOffset = -scaledRegion.origin / scaledRegion.size;
                    glowReady = renderNeonPass(RegionProjection(scaledRegion, scale), scaledRegion.texels.x,
                                               scaledRegion.texels.y, true, gatherUVScale, gatherUVOffset, time,
                                               config);
                }

                // Back to the caller's target and viewport, both at once.
                // Unconditional: the pass may have bound its target before
                // failing, and leaving the caller on our buffer would silently
                // redirect every renderer after this one.
                prevTarget.Restore();
            }
        }

        // ===== Caller's framebuffer ==========================================
        // Premultiplied-alpha "over": final = src.rgb + dst * (1 - src.a), for
        // every pass from here on, so each composites cleanly over the last.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // --- Pass 2a: opaque-mode background fill ---------------------------
        // Full-res on the caller's framebuffer on both paths, and under the
        // glow on both: here it lands before pass 1 at scale 1.0 and before
        // the blit and the ring below it.
        if (config.neon.opaqueMode != OpaqueMode::NONE)
        {
            renderOpaqueFill(viewportWidth, viewportHeight, config);
        }

        if (glowReady)
        {
            if (!scaled)
            {
                // --- Pass 1 (direct): the gather, composited onto the target.
                renderNeonPass(mvp, bufW, bufH, false, glm::vec2(0.0f), glm::vec2(0.0f), time, config);
            }
            else
            {
                // --- Pass 2b / 2c: the blit and the edge ring. They cover
                // disjoint areas (setupRingGeometry), so their order between
                // themselves does not matter for the result.
                renderBlitPass(viewportWidth, viewportHeight, blitUVScale, blitUVOffset, config);
                renderRingPass(viewportWidth, viewportHeight, gatherUVFullScale, gatherUVFullOffset, time, config);
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
                                   config.neon.outsideCutoff != mCurrentConfig.neon.outsideCutoff ||
                                   // glowSide and insideCutoff BOUND THE QUAD NOW. They did not
                                   // when this gate was written - the quad was rect + glow reach
                                   // whatever the glow did with its own interior, so both of these
                                   // moved only the shader's discards. @ref setupGeometry now caps
                                   // the margin under GlowSide::INSIDE and cuts a hole for the
                                   // other two, so a change to either has to rebuild it.
                                   //
                                   // Missing them does not under-draw, it draws the PREVIOUS
                                   // config's bound: switching BOTH + insideCutoff 8 to INSIDE with
                                   // no cutoff kept the ring built for the first and rendered the
                                   // second through it, holing out 48131 px of lit interior and
                                   // leaving a 12 px frame of glow round the inside of the edge.
                                   // The same shape of mistake as adding a field to a Config struct
                                   // and not to its operator== - see AGENTS.md.
                                   config.neon.glowSide != mCurrentConfig.neon.glowSide ||
                                   config.neon.insideCutoff != mCurrentConfig.neon.insideCutoff;
        // The fill ring is bounded by the FILL's own cutoff pair (sizes and
        // feathers), not by the glow reach, so it gets its own gate rather
        // than riding on geometryDirty: opaqueMode and the fill cutoffs move
        // the ring but not the glow quad, and glowRadius / bloomStrength / the
        // glow cutoffs move the glow quad but not the ring. Only the geometry
        // moves both.
        const bool fillDirty = config.geometry != mCurrentConfig.geometry ||
                               config.neon.opaqueMode != mCurrentConfig.neon.opaqueMode ||
                               config.neon.opaqueInsideCutoff != mCurrentConfig.neon.opaqueInsideCutoff ||
                               config.neon.opaqueOutsideCutoff != mCurrentConfig.neon.opaqueOutsideCutoff;
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

        // Give the scaled buffer back the moment this config stops wanting it -
        // the layer switched off, or the scale returned to 1.0. It is the only
        // allocation in this renderer that is not a handful of KB: at 1920x1080
        // and scale 0.5 it is 2.1 MB of colour attachment, and nothing else
        // here freed it, so a host that enabled the neon at a reduced scale
        // during setup and then turned it off held that for the life of the
        // effect. Everything else the renderer owns - the three atlases, the
        // emission table, the UBOs, the quads - is fixed-size and small enough
        // that reclaiming it would cost more in reallocation than it saves.
        //
        // Cheap to get wrong in only one direction, and this is the safe one:
        // @ref renderNeonPass re-Resizes before it binds, so a release of a
        // buffer that turns out to be wanted again costs one allocation on the
        // next drawn frame and nothing else. Resize's own early-out then keeps
        // it allocated for as long as the size and format hold.
        //
        // Here rather than in Render because Render must not be the thing that
        // deletes a framebuffer - see Framebuffer::Release on why the deletion
        // wants to be outside a pass. This runs between frames, from SetConfig
        // or the effect's active-config refresh.
        //
        // BEFORE the shader-validity return below, which guards REBUILDS: a
        // release is not one, and this path is also reachable pre-Initialize
        // (AddRenderer calls OnConfigChanged), where Release no-ops on the
        // buffer it finds unallocated.
        if (!UsesScaledBuffer(config))
        {
            mScaledBuffer.Release();
            mGatherBuffer.Release();
        }

        // Rebuilds need the GL objects Initialize creates. The neon.frag
        // programs are no gate any more - they are built on first draw, so a
        // valid program no longer means "initialised".
        if (!mInitialized)
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
            // The blit / ring partition, after setupGeometry because the blit's
            // outer frame reads the glow margin it computes. Gated on the same
            // set: the ring is sized by the filament and the scale and clipped
            // by glowSide and the cutoffs, and the blit is bounded by the glow
            // margin too - all of them in geometryDirty already.
            setupRingGeometry(config);
        }

        if (fillDirty)
        {
            setupFillGeometry(config);
        }


        bakeLUTs(config);
    }

    bool NeonRenderer::setupShaders()
    {
        // Only the two programs both resolution paths draw with. The neon.frag
        // programs and the blit are per PATH and are built the first time that
        // path renders - see ensurePathPrograms.
        //
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
        if (!mBlackRectShader.IsValid() || !mEmissionShader.IsValid())
        {
            return false;
        }

        // The pre-pass reads the same two blocks the main pass does, so they
        // share bindings and are packed once per frame before either runs.
        mEmissionShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        mEmissionShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        return true;
    }

    bool NeonRenderer::buildNeonProgram(ShaderProgram &program, const char *define, const char *name,
                                        unsigned int programBit, bool gathers)
    {
        if (program.IsValid())
        {
            return true;
        }
        // Tried and failed before: the error is in the log already, and a
        // retry would cost a full compile on every frame of a path that can
        // never draw.
        if ((mFailedPrograms & programBit) != 0)
        {
            return false;
        }

        const std::string source = define ? WithDefine(ShaderSource::NEON_FRAG_SRC, define)
                                          : std::string(ShaderSource::NEON_FRAG_SRC);
        program = ShaderProgram(ShaderSource::NEON_VERT_SRC, source.c_str(), name);
        if (!program.IsValid())
        {
            mFailedPrograms |= programBit;
            LOG_E("NeonRenderer: %s failed to compile/link - that resolution path will draw no glow.", name);
            return false;
        }
        // Every neon program reads the segment and arc blocks, at the same
        // bindings; only the ones that run the gather have the sample block.
        program.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        program.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        if (gathers)
        {
            program.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        }
        return true;
    }

    bool NeonRenderer::ensurePathPrograms(bool scaled)
    {
        // The direct path draws with neon.frag as it is, and nothing else.
        if (!scaled)
        {
            return buildNeonProgram(mNeonShader, nullptr, "NeonRenderer", PROGRAM_PLAIN, true);
        }

        // The scaled path draws with four, none of them the direct path's:
        // the gather pass (neon.frag stopped right after the gather), the
        // shading that reads it back - TWICE, one program object for pass 1 at
        // the reduced scale and another for the edge ring at full resolution -
        // and the composite. So a host that never leaves 1.0 never compiles
        // the last four, and one that never sits at 1.0 never compiles the
        // first - each pays for exactly the path it draws.
        //
        // Why the shading is compiled twice from one source: each program
        // object then draws ONE render target, in one blend state, per frame.
        // One program drawing two differently configured targets in a frame -
        // an offscreen buffer unblended and the caller's framebuffer blended -
        // is the shape that measured 93-624x slower on the AMD macOS driver
        // (docs/neon-resolution-scale-plan.md section 7, build AB). A second
        // compile on the first scaled frame is the price of never finding out
        // which half of that difference the driver keys on.
        if (!buildNeonProgram(mNeonGatherShader, "NEON_GATHER_ONLY", "NeonRenderer.Gather", PROGRAM_GATHER, true) ||
            !buildNeonProgram(mNeonShadeShader, "NEON_READS_GATHER", "NeonRenderer.Shade", PROGRAM_SHADE, false) ||
            !buildNeonProgram(mNeonRingShader, "NEON_READS_GATHER", "NeonRenderer.Ring", PROGRAM_RING, false))
        {
            return false;
        }
        if (!mBlitShader.IsValid())
        {
            if ((mFailedPrograms & PROGRAM_BLIT) != 0)
            {
                return false;
            }
            mBlitShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                        ShaderSource::NEON_BLIT_FRAG_SRC,
                                        "NeonRenderer.Blit");
            if (!mBlitShader.IsValid())
            {
                mFailedPrograms |= PROGRAM_BLIT;
                LOG_E("NeonRenderer: the blit failed to compile/link - the scaled path will draw no glow.");
                return false;
            }
        }
        return true;
    }

    void NeonRenderer::setupFullscreenQuad()
    {
        // Static NDC quad, shared by the three passes that cover their whole
        // target with an identity MVP: the emission bake, the opaque-mode
        // black fill at OpaqueMode::ALL (whose shader derives its shape from
        // gl_FragCoord, not aPos - every narrower mode is bounded by
        // @ref setupFillGeometry's ring instead). The scaled path's blit used
        // to share it; it now draws mBlitVertexArray.
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
        // Size the quad to cover the lit region: rect + the glow's margin, so
        // geometry bounds the far region instead of a per-fragment discard
        // (tiler-friendly). The margin - glow reach, the filament floor under
        // it and the outside-cutoff cap over it - is @ref GetGlowMargin, which
        // the edge ring also asks for at scale 1.0. See there for each term.
        //
        // The whole quad is built in SCALED space, matching the transform and
        // the uniforms Render uploads. At resolutionScale 1.0 the factor is
        // identity and every expression below is its full-res form.
        const float scale = GetClampedResolutionScale(config);
        const float margin = GetGlowMargin(config, scale);

        // The scaled path's cutoff guard band, in FULL-RES px - see
        // GetCutoffGuardPx. The inner hole below carries it like the outer cap.
        const float cutGuardPx = GetCutoffGuardPx(scale);

        // How far past the rect edge the one-sided cut still draws, on the side
        // it culls, in FULL-RES px. Mirrors neon.frag's `sideCull`: half a
        // destination pixel of anti-aliasing reach on the direct path, and on
        // the scaled path the guard band the blit reconstructs the cut from,
        // which is stated in BUFFER px and so divides back out. 1.0 covers the
        // direct-path case including its diagonal.
        const float sideCullPx = (scale < 1.0f)
                                     ? (static_cast<float>(BLIT_SIDE_GUARD_PX) / scale)
                                     : 1.0f;

        // uQuadMargin KEEPS THE UNCAPPED VALUE. It is not a description of the
        // rectangle being drawn; it is the distance the shader fades its
        // emission out over, and neon.frag's quad-edge fade is written against
        // the margin the GLOW needs, not the one the rasteriser gets. Feeding
        // it a capped margin puts the fade's ramp a few px from the rect edge,
        // where it lands on the lit interior and erases it - measured on a
        // 300x200 rect at glowRadius 40, glowSide INSIDE: the glow survived to
        // 11 px inside the edge and was flat black from 13 px in, 48131 pixels
        // of interior gone. GetGlowMargin's outside-cutoff cap has the same shape and
        // is safe only where the cutoff's own mask HAS taken that region to
        // zero - which is why it is now skipped on the side glowSide culls,
        // where the shader neutralises that mask and nothing takes it to zero.
        //
        // So the two margins are separated: the shader is told what the glow
        // does, the rasteriser is told what to cover. They agree except where a
        // cull makes the second one smaller, and where they disagree the
        // fragments that go missing are ones the shader discards anyway.
        mQuadMargin = margin;

        // The edge ring shades at FULL resolution, so it fades against the
        // margin the direct path would use rather than this one - which on the
        // scaled path carries the Nyquist-widened filament and the guard band.
        // See renderRingPass.
        mRingQuadMargin = GetGlowMargin(config, 1.0f);

        float halfW = config.geometry.width * 0.5f * scale;
        float halfH = config.geometry.height * 0.5f * scale;

        // GlowSide::INSIDE discards every fragment past the rect edge, so the
        // entire exterior margin is rasterised and thrown away - at glowRadius
        // 20 on a 900x600 rect that is 1248 px of quad per side, shaded and
        // discarded, for a lit region that stops at the edge.
        //
        // This is the same move @ref setupGeometry already makes for an outside
        // cutoff, one step further: that one caps the margin the shader sees
        // too, this one caps only the geometry. The comment at the discards in
        // neon.frag used to say the one-sided cuts "cull a useful half-band the
        // quad can't express" - true of OUTSIDE, whose lit region is an
        // annulus, and false of INSIDE, whose lit region is exactly a quad.
        // OUTSIDE is handled by the hole below instead.
        float geomMargin = margin;
        if (config.neon.glowSide == GlowSide::INSIDE)
        {
            geomMargin = std::min(geomMargin, (sideCullPx + GLOW_EDGE_SAFETY) * scale);
        }
        // No extension for the edge ring. Pass 1 used to write the gather the
        // ring reads, so its quad had to reach the ring's outer edge; the
        // gather now has a pass of its own whose quad covers both
        // (setupRingGeometry), and pass 1 writes colour only, which nothing
        // reads past uQuadMargin. So the quad is sized by the glow alone.

        // How far INWARD the glow still reaches, in full-res px. The quad has
        // no hole, so until now nothing that bounds the emission from the
        // inside bought any fill rate at all - the interior was rasterised in
        // full and discarded a fragment at a time. Two settings do bound it,
        // they are independent discards in the shader, and a fragment has to
        // survive both, so the reach is the NEARER of the two.
        //
        // A disabled bound contributes nothing and leaves the sentinel, which
        // collapses the hole below and gives back the plain quad - so the
        // default config, and every config that lights its own interior, is
        // untouched.
        // MUTUALLY EXCLUSIVE, mirroring the cutoff neutralisation in neon.frag:
        // under GlowSide::OUTSIDE the inside cutoff is subsumed by the cut and
        // the shader ignores it, so taking a min() with it here would be worse
        // than pointless. It would shrink the hole to the cutoff's reach while
        // the shader still lights out to the guard band, and the quad would
        // clip what the blit reconstructs the cut from - the same dark seam by
        // a second route. With insideCutoff size 0 at scale 0.25 that put the
        // hole at 1.25 buffer px against a 2.0 px guard.
        float innerReach = CUTOFF_DISABLED_SIZE;
        if (config.neon.glowSide == GlowSide::OUTSIDE)
        {
            innerReach = sideCullPx;
        }
        else if (config.neon.insideCutoff.enable)
        {
            // neon.frag discards at dIn < -(inHalf + cutGuard), i.e. past the
            // end of the inside fade - the point GetCutoffEnd computes - plus
            // the scaled path's guard band.
            innerReach = GetCutoffEnd(config.neon.insideCutoff, CUTOFF_FLOOR_PX) + cutGuardPx;
        }
        const float innerMargin = (innerReach + GLOW_EDGE_SAFETY) * scale;

        const float ow = halfW + geomMargin;
        const float oh = halfH + geomMargin;

        // HOLE, by the same construction @ref setupFillGeometry uses and for
        // the same reason: the region the glow cannot reach is the INWARD
        // parallel curve, a rounded box shrunk by innerMargin with its radius
        // shrunk to match, and the largest axis-aligned rectangle inside a
        // rounded box is not the box's own half-extents - its corners have to
        // clear the corner arc. Cutting it square would carve a wedge out of
        // each corner of the glow.
        //
        // Clamped at zero throughout, which is what absorbs the sentinel: an
        // inner bound deeper than the rect (or a disabled one) drives both
        // half-extents to 0, the side strips come out degenerate, and the top
        // and bottom strips meet at y = 0 to tile the whole quad.
        const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry) * scale;
        const float holeRadius = std::max(radius - innerMargin, 0.0f);
        const float cornerInset = holeRadius * CORNER_INSET_FACTOR;
        const float iw = std::max(halfW - innerMargin - cornerInset, 0.0f);
        const float ih = std::max(halfH - innerMargin - cornerInset, 0.0f);

        // The quad in FULL-RES px, for the gather pass's quad to cover - see
        // setupRingGeometry. A hole exists only when both extents are positive.
        mGlowOuter = glm::vec2(ow, oh) / scale;
        mGlowHole = (iw > 0.0f && ih > 0.0f) ? glm::vec2(iw, ih) / scale : glm::vec2(0.0f);

        // No hole to cut: emit the plain quad, byte for byte the geometry this
        // method has always produced. Kept as its own arm rather than letting
        // the ring degenerate into it so that every config without an inner
        // bound - which includes the default - is provably unchanged by this,
        // rather than relying on the rasteriser's fill rule to make eight
        // triangles land exactly where two did.
        if (iw <= 0.0f && ih <= 0.0f)
        {
            float l = -ow;
            float r = ow;
            float b = -oh;
            float t = oh;

            // clang-format off
            float quad[] = {
                l, t, l, b, r, b,
                l, t, r, b, r, t,
            };
            // clang-format on
            mGlowVertexArray.SetVertexData(quad, sizeof(quad), GL_DYNAMIC_DRAW);
            mGlowVertexCount = 6;
            return;
        }

        // Four strips that TILE the ring without overlapping - top and bottom
        // full width, left and right only across the hole's height. Overlap
        // would matter here exactly as it does for the fill: this pass
        // composites premultiplied-over, so a fragment covered twice blends
        // twice and reads denser than the shader's own coverage.
        // clang-format off
        float verts[] = {
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
        mGlowVertexCount = 24;
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

        // How far the fill's coverage can run past each rect edge side: out
        // to the END of that side's feather (GetCutoffEnd) - solid up to
        // size, gone by size + softness at or above the floor. The floor is
        // black-rect.frag's `aa`, fwidth(d): a nominal ONE pixel is passed,
        // as setupGeometry does for the glow's direct path, so a near-zero
        // softness is bounded at size + 0.5 rather than at size. SAFETY is on
        // top of that, for the part of `aa` the CPU cannot see (up to ~1.4 px
        // on a diagonal, so ~0.2 px more reach) plus rounding.
        // Over-covering by a couple of pixels is free - those fragments come
        // out at coverage 0, which this pass's premultiplied blend leaves the
        // destination untouched by - while under-covering would clip the
        // feather, so this rounds outward on purpose.
        constexpr float FILL_EDGE_SAFETY = 3.0f;
        constexpr float FILL_SOFT_FLOOR_PX = 1.0f;
        const Cutoff &fillIn = config.neon.opaqueInsideCutoff;
        const Cutoff &fillOut = config.neon.opaqueOutsideCutoff;

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
            outerMargin = GetCutoffEnd(fillOut, FILL_SOFT_FLOOR_PX) + FILL_EDGE_SAFETY;
        }
        if (mode == OpaqueMode::INSIDE || mode == OpaqueMode::BOTH)
        {
            innerMargin = GetCutoffEnd(fillIn, FILL_SOFT_FLOOR_PX) + FILL_EDGE_SAFETY;
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
        // ring's dirty set (geometry, opaqueMode, the fill's two cutoffs)
        // contains no AnimatableField today, so this fires on host
        // edits and not per frame. It is hinted correctly anyway, because the
        // day a cutoff or the geometry becomes animatable is not the day
        // anyone will think to come back and look at a usage flag.
        mFillVertexArray.SetVertexData(verts, sizeof(verts), GL_DYNAMIC_DRAW);
        mFillVertexCount = 24;
    }

    void NeonRenderer::setupRingGeometry(const Config &config)
    {
        // Scale 1.0 is the direct path: neither the blit nor the ring runs, so
        // there is nothing to partition. Keyed on the clamped scale rather than
        // on UsesScaledBuffer, because `enable` is not in this method's dirty
        // gate and nothing here depends on it.
        const float scale = GetClampedResolutionScale(config);
        if (scale >= 1.0f)
        {
            mRingVertexCount = 0;
            mBlitVertexCount = 0;
            mGatherVertexCount = 0;
            mGatherOuter = glm::vec2(0.0f);
            mScaledOuter = glm::vec2(0.0f);
            return;
        }

        const float halfW = config.geometry.width * 0.5f;
        const float halfH = config.geometry.height * 0.5f;
        const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry);

        // The RING: GetRingWidth either side of the edge, clipped to the lit
        // band - see GetRingExtent for why the clip changes no pixel.
        //
        // OUTER box: the outward parallel curve of a rounded box is a rounded
        // box grown by the same distance, so its circumscribed box bounds it
        // exactly - the same argument as setupFillGeometry's outer edge. HOLE:
        // the largest axis-aligned rectangle inside the INWARD parallel curve.
        // The ring over-covers its band near the corners, and those few pixels
        // are shaded at full resolution, which is exact.
        const EdgeExtent ring = GetRingExtent(config, scale);
        const glm::vec2 ringOuter = CircumscribedBox(halfW, halfH, ring.out);
        const glm::vec2 ringHole = InscribedBox(halfW, halfH, radius, -ring.in);

        // The BLIT: everything outside the ring that can still be non-zero,
        // and nothing else. It used to be the whole viewport minus the ring -
        // about 870k fragments at 1280 x 720 whatever the glow did - and two
        // bounds make most of that free to skip, each exact:
        //
        //   - The lit band (@ref GetLitExtent). Past it the blit's own cut and
        //     cutoff masks are exactly 0, so a one-sided glow loses the whole
        //     dark side and a cutoff band everything past its cutoffs.
        //   - The glow's fade. Pass 1's quad-edge fade takes every texel past
        //     mQuadMargin to exactly 0 (and texels past the quad are the clear
        //     colour), so a destination pixel whose bilinear footprint lies
        //     entirely out there composites 0. FOOTPRINT_TEXELS buffer texels
        //     plus a pixel past the margin is that footprint.
        //
        // Never inside the ring's own boxes, so the two arrays still tile.
        const EdgeExtent lit = GetLitExtent(config);
        const float glowBound = (mQuadMargin + FOOTPRINT_TEXELS) / scale + 1.0f;
        const float blitOut = std::max(ring.out, std::min(glowBound, lit.out + LIT_EDGE_SAFETY));
        const float blitIn = std::max(ring.in, lit.in + LIT_EDGE_SAFETY);
        const glm::vec2 blitOuter = CircumscribedBox(halfW, halfH, blitOut);
        const glm::vec2 blitHole = InscribedBox(halfW, halfH, radius, -blitIn);

        // What the blit reads of the reduced buffer: its own outer box plus
        // its bilinear footprint and a pixel. The reduced buffer is sized to
        // this (GetBufferRegion), not to pass 1's quad - texels pass 1 draws
        // past it are never read, so they need not exist.
        mScaledOuter = blitOuter + glm::vec2(FOOTPRINT_TEXELS / scale + 1.0f);

        // THE PARTITION. Both arrays are emitted from the SAME floats: the
        // ring's outer box is the blit's outer band's inner edge, and the
        // ring's hole is the blit's inner band's outer edge. Axis-aligned edges
        // at identical coordinates rasterise identically, and the fill rule
        // hands a pixel centred exactly on one to one side only - so every
        // pixel the two cover is drawn by exactly one of them, with no
        // per-pixel test and no float compared across programs. Both composite
        // premultiplied-over, so a pixel drawn twice would composite twice;
        // build these two anywhere but here, from separately computed values,
        // and that guarantee goes.
        std::vector<float> ringVerts;
        ringVerts.reserve(48);
        PushAnnulus(ringVerts, ringOuter, ringHole);

        std::vector<float> blitVerts;
        blitVerts.reserve(96);
        PushAnnulus(blitVerts, blitOuter, ringOuter); // outside the ring
        PushAnnulus(blitVerts, ringHole, blitHole);   // inside it

        // The GATHER pass's area, in pass 1's SCALED space: everything pass 1
        // or the ring will read the gather at, plus the gather's own bilinear
        // footprint. Pass 1 reads it at every texel of its quad and the ring at
        // every pixel of its annulus, so this is the union of the two, grown
        // by FOOTPRINT_TEXELS gather texels plus a pixel at both edges - the
        // inner one included, since the passes that shade can reach into a
        // hole by that footprint too. Drawn with no culls (see neon.frag), so
        // every texel either of them can touch holds a real gather.
        const float gatherPad = FOOTPRINT_TEXELS / GetGatherScale(config, scale) + 1.0f;
        const glm::vec2 gatherOuter = glm::max(mGlowOuter, ringOuter) + glm::vec2(gatherPad);
        const glm::vec2 holeBoth = glm::min(mGlowHole, ringHole);
        const glm::vec2 gatherHole = (holeBoth.x > 0.0f && holeBoth.y > 0.0f)
                                         ? glm::max(holeBoth - glm::vec2(gatherPad), glm::vec2(0.0f))
                                         : glm::vec2(0.0f);
        std::vector<float> gatherVerts;
        gatherVerts.reserve(48);
        PushAnnulus(gatherVerts, gatherOuter * scale, gatherHole * scale);
        mGatherOuter = gatherOuter;

        // GL_DYNAMIC_DRAW and no SetAttribPointer, as in setupGeometry - and
        // for its reason too: this runs under geometryDirty, which an
        // intensity animation sets every frame.
        mGatherVertexArray.SetVertexData(gatherVerts.data(), gatherVerts.size() * sizeof(float), GL_DYNAMIC_DRAW);
        mGatherVertexCount = static_cast<int>(gatherVerts.size() / 2);
        mRingVertexArray.SetVertexData(ringVerts.data(), ringVerts.size() * sizeof(float), GL_DYNAMIC_DRAW);
        mRingVertexCount = static_cast<int>(ringVerts.size() / 2);
        mBlitVertexArray.SetVertexData(blitVerts.data(), blitVerts.size() * sizeof(float), GL_DYNAMIC_DRAW);
        mBlitVertexCount = static_cast<int>(blitVerts.size() / 2);
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
            const TargetFormat &f = EMISSION_FORMATS[i];
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

    bool NeonRenderer::resizeGatherBuffer(int width, int height, int attachments)
    {
        // The same walk as resizeEmissionBuffer, with one difference: this
        // buffer is RELEASED whenever the scaled path has nothing to draw (see
        // OnConfigChanged), so its own format cannot record how far down the
        // list a driver forced it. mGatherFormat does, and only ever advances -
        // a driver that refused RGBA16F once is not asked again on every
        // re-arm, and logs once.
        for (; mGatherFormat < std::size(GATHER_FORMATS); ++mGatherFormat)
        {
            const TargetFormat &f = GATHER_FORMATS[mGatherFormat];
            if (mGatherBuffer.Resize(width, height, f.internalFormat, f.format, f.type, GL_LINEAR, attachments))
            {
                return true;
            }
            if (mGatherFormat + 1 < std::size(GATHER_FORMATS))
            {
                LOG_E("NeonRenderer: %s gather target unavailable, falling back to %s. "
                      "Below resolutionScale 1.0 the glow reads up to ~2/255 further from 1.0.",
                      f.name, GATHER_FORMATS[mGatherFormat + 1].name);
            }
        }
        // Every candidate refused: leave the index on the last one, so the
        // next frame retries the format most likely to allocate rather than
        // running off the end of the list for good.
        mGatherFormat = std::size(GATHER_FORMATS) - 1;
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
        // The render target handed to this pass - framebuffer AND viewport,
        // both of which the bind below replaces. NOT necessarily the window's
        // framebuffer: an offscreen frame capture (@ref OffscreenCapture) hands
        // this renderer a real FBO, and the gather that follows has no bind of
        // its own, so returning to 0 here would redirect the whole neon pass to
        // the window and leave the capture empty.
        //
        // Capturing the viewport rather than reconstructing it matters a
        // little more here than at the other sites, because this pass can be
        // SKIPPED: anything a run frame leaves behind that a skipped frame does
        // not is a difference the glow can show, and "hand back what you
        // found" needs no precondition to hold for the two to agree.
        const RenderTargetState prevTarget = RenderTargetState::Capture();

        // The emission table's axes are sample index and row, not pixels, so a
        // host scissor box - which is in the CALLER's window coordinates -
        // means nothing here and would discard most of the bake. The table is
        // two texels tall, so any box with a y origin above 1 discards ALL of
        // it and the gather reads whatever the buffer held last frame. See
        // GLUtils::NoScissorScope; the host's clip still applies to pass 1,
        // which is where it belongs.
        GLUtils::NoScissorScope noScissor;

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
        prevTarget.Restore();

        // What the buffer now holds. Recorded by the only writer of it, so the
        // staleness test upstream can never describe a bake that did not run.
        mEmissionDirty = false;
        mEmissionTime = time;
    }

    void NeonRenderer::uploadShapeUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale,
                                           const Config &config)
    {
        // Every pixel-valued uniform here and in uploadNeonUniforms is
        // multiplied by `scale`, which is 1.0 on the direct path - so the two
        // paths upload literally the same numbers there, and the scaled path
        // is the only one that moves. The shader converts neon-tuning.h's own
        // full-res px constants with uResolutionScale to land in the same
        // space.
        shader.SetUniform("uMVP", mvp);
        shader.SetUniform("uRectSize", glm::vec2(config.geometry.width * scale,
                                                 config.geometry.height * scale));
        shader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry) * scale);
    }

    void NeonRenderer::uploadNeonUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale,
                                          float time, float quadMargin, const Config &config)
    {
        uploadShapeUniforms(shader, mvp, scale, config);
        shader.SetUniform("uResolutionScale", scale);
        shader.SetUniform("uLineWidth", config.neon.lineWidth * scale);
        shader.SetUniform("uFilamentFalloff", config.neon.filamentFalloff);
        shader.SetUniform("uIntensity", config.neon.intensity);
        shader.SetUniform("uTime", time);
        shader.SetUniform("uHueRotationRate", config.neon.hueRotationRate);
        shader.SetUniform("uGlowRadius", config.neon.glowRadius * scale);
        shader.SetUniform("uBloomStrength", config.neon.bloomStrength);
        shader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        shader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness * scale);
        shader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff) * scale);
        shader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness * scale);
        shader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff) * scale);
        shader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness * scale);

        shader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));

        // The three LUT atlases are no longer read by the gather (the emission
        // pre-pass consumes them instead), but the pointwise path still samples
        // them for the colour-stop alpha - see the alpha reads in neon.frag.
        mGradientLUT.Bind(0);
        shader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        shader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        shader.SetUniform("uArcLUT", 2);
        shader.SetUniform("uQuadMargin", quadMargin);
    }

    bool NeonRenderer::renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight, bool scaled,
                                      const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset,
                                      float time, const Config &config)
    {
        const float scale = GetClampedResolutionScale(config);

        // SCALED PATH ONLY: mScaledBuffer is a reduced-size copy of the
        // viewport, so a host scissor box - in the CALLER's window coordinates
        // - lands on the wrong texels of it. The clear and the gather below
        // would skip everything outside the box, and pass 2b would then read
        // the region the box maps DOWN to, which is a different region again
        // and one nothing wrote this frame: last frame's pixels, blitted back
        // under the host's clip. See GLUtils::NoScissorScope.
        //
        // The direct path takes none of this - `scaled` false short-circuits
        // even the query - because there the gather IS the composite, drawn
        // straight onto the caller's framebuffer in the caller's coordinates,
        // and the host's clip is exactly what it asked for. The scaled path's
        // composite gets the same treatment once this scope ends, which is
        // before Render calls renderBlitPass.
        GLUtils::NoScissorScope noScissor(scaled);

        if (scaled)
        {
            // Resize destroys the attachment on its failure path, so a failure
            // leaves mScaledBuffer holding id 0 - and Bind would then bind the
            // CALLER'S framebuffer, with only Framebuffer::ClearBuffer's own
            // no-attachment guard standing between that and erasing everything
            // already drawn this frame (a clear is not clipped by the
            // viewport). Under an OffscreenCapture that target is the capture.
            // Do not lean on that guard - bail here instead;
            // Render skips the blit with us, and noScissor puts the host's
            // scissor back as this return unwinds.
            //
            // The filter is requested through Resize, which is the ONLY writer
            // of the tracked value, so it cannot drift. Setting it on the
            // texture afterwards instead leaves mFilter disagreeing with the
            // texture, and the next frame's Resize then sees a mismatch and
            // destroys and recreates the FBO - one reallocation per frame,
            // measured.
            //
            // ONE attachment: the composited colour. The gather lives in its
            // own, coarser buffer (renderGatherPass).
            if (!mScaledBuffer.Resize(bufWidth, bufHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR))
            {
                return false;
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

        // The direct path gathers and shades in one program. The scaled path
        // shades from the gather pass's result instead of running the loop
        // here; every other uniform is the same.
        ShaderProgram &shader = scaled ? mNeonShadeShader : mNeonShader;
        shader.Use();
        uploadNeonUniforms(shader, mvp, scale, time, mQuadMargin, config);
        if (scaled)
        {
            bindGatherBuffer(shader, gatherUVScale, gatherUVOffset);
        }
        else
        {
            bindGatherInputs(shader, config);
        }

        // Tight glow quad in both modes - opaque's far region is covered by the
        // fill pass, so the gather never runs fullscreen.
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, mGlowVertexCount);
        shader.Unuse();
        return true;
    }

    void NeonRenderer::bindGatherInputs(ShaderProgram &shader, const Config &config)
    {
        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon.frag) - raw float32 vec4[N], .xy holds the perimeter point in
        // the same scaled space as the transform.
        mLoopSamplesBlock.BindBase(LOOP_SAMPLES_BLOCK_BINDING);
        shader.SetUniform("uNumSamples", GetClampedNumSamples(config));
        // Emission table from pass 0 on unit 3; the gather texelFetches both
        // of its rows per sample.
        mEmissionBuffer.BindTexture(3);
        shader.SetUniform("uEmission", 3);
    }

    void NeonRenderer::bindGatherBuffer(ShaderProgram &shader, const glm::vec2 &uvScale, const glm::vec2 &uvOffset)
    {
        // Attachment 1 exists only with segments; without them this binds
        // texture 0, which the shader's uSegmentCount branch never reads.
        mGatherBuffer.BindTexture(3, 0);
        shader.SetUniform("uGather", 3);
        mGatherBuffer.BindTexture(4, 1);
        shader.SetUniform("uGatherSeg", 4);
        shader.SetUniform("uGatherUVScale", uvScale);
        shader.SetUniform("uGatherUVOffset", uvOffset);
    }

    bool NeonRenderer::renderGatherPass(const glm::mat4 &mvp, int gatherWidth, int gatherHeight, float scale,
                                        const Config &config)
    {
        // A reduced-size target, so the host's scissor is in the wrong
        // coordinates here - see renderNeonPass.
        GLUtils::NoScissorScope noScissor(true);

        // Attachment 0 holds the hue and the arc coverage, attachment 1 the
        // segments' - only when there ARE segments, keyed on the count so an
        // animation that moves segments never reallocates.
        const int attachments = mEffectiveSegments.empty() ? 1 : 2;
        if (!resizeGatherBuffer(gatherWidth, gatherHeight, attachments))
        {
            return false;
        }
        mGatherBuffer.Bind();
        mGatherBuffer.ClearBuffer();

        // The SAME transform as pass 1 - its scaled space, its samples -
        // drawn onto a smaller buffer: the ortho maps the scaled viewport onto
        // whatever viewport is bound, so only the texel grid changes, and vPos
        // at a gather texel centre is the scaled rect-local position that
        // texel stands for.
        //
        // The shape uniforms only, not uploadNeonUniforms: everything else is
        // shading, which this variant compiles out, and ShaderProgram logs an
        // error for each uniform it is handed that the program does not have.
        mNeonGatherShader.Use();
        uploadShapeUniforms(mNeonGatherShader, mvp, scale, config);
        bindGatherInputs(mNeonGatherShader, config);
        mGatherVertexArray.DrawArrays(GL_TRIANGLES, mGatherVertexCount);
        mNeonGatherShader.Unuse();
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
            // way round to be wrong. This IS the host's viewport: on the
            // scaled path Render has already put back the target and viewport
            // the offscreen phase moved off (prevTarget.Restore), and nothing
            // before this pass retargets on the direct path - the emission
            // pass restores its own.
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
        // The FILL's own cutoff pair, never the glow's - see
        // NeonConfig::opaqueInsideCutoff. Same size, sentinel and softness as
        // setupFillGeometry reads, so the ring bounds exactly what this
        // shades. Softness goes up as configured, exactly as neon.frag's does:
        // black-rect.frag treats a negative one as 0 when placing the fade and
        // floors its width at `aa`, so a CPU-side clamp would add nothing.
        const Cutoff &fillIn = config.neon.opaqueInsideCutoff;
        const Cutoff &fillOut = config.neon.opaqueOutsideCutoff;
        mBlackRectShader.SetUniform("uOpaqueMode", static_cast<int>(config.neon.opaqueMode));
        mBlackRectShader.SetUniform("uInsideCutoff", GetCutoffSize(fillIn));
        mBlackRectShader.SetUniform("uInsideCutoffSoftness", fillIn.softness);
        mBlackRectShader.SetUniform("uOutsideCutoff", GetCutoffSize(fillOut));
        mBlackRectShader.SetUniform("uOutsideCutoffSoftness", fillOut.softness);
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

    void NeonRenderer::renderBlitPass(int viewportWidth, int viewportHeight, const glm::vec2 &uvScale,
                                      const glm::vec2 &uvOffset, const Config &config)
    {
        // Bilinear upscaling of premultiplied alpha is fringe-free; the blit
        // shader composites over whatever is on the target already (the black
        // fill if opaque, the original background otherwise).
        //
        // It also applies the one-sided cut and the inside/outside cutoffs,
        // which is why it takes a config at all. Neither can be made in the
        // gather: that runs at resolutionScale and the bilinear upsample smears
        // any edge it draws across 1/scale destination pixels in both
        // directions, which put glow on the dark side of the line and softened
        // every cutoff by a buffer texel. This pass is full-res, so both land
        // where the direct path puts them. See neon-blit.frag.
        // The area this pass covers can be EMPTY - a one-sided glow, or a
        // cutoff band no wider than the ring, leaves nothing outside the ring
        // that can be lit (setupRingGeometry). That is not a failure to fall
        // back from: drawing anything here would draw over the ring.
        if (mBlitVertexCount == 0)
        {
            return;
        }
        mBlitShader.Use();

        // FULL-RES geometry, and derived here rather than from Render's scaled
        // transform - the same reasoning, and the same y mirror, as
        // @ref renderOpaqueFill, which is the other always-full-res pass.
        // uGlowSideSoftness goes up UNSCALED for the same reason: this pass
        // measures in destination pixels, the gather measures in buffer ones.
        const glm::vec2 centerFull(config.geometry.position.x + config.geometry.width * 0.5f,
                                   static_cast<float>(viewportHeight) - config.geometry.position.y -
                                       config.geometry.height * 0.5f);
        const glm::vec2 viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));

        // The area this pass covers is everything outside the edge ring that
        // can still be lit (setupRingGeometry) - mBlitVertexArray, in full-res
        // rect-local px, so it goes up under the full-res transform the fill
        // uses. vPos is then rect-local px rather than NDC, and maps onto the
        // reduced buffer through @p uvScale / @p uvOffset - its region's map
        // (GetBufferRegion), which Render derives with the region. The ring
        // finds its gather texels the same way.
        const glm::mat4 proj = glm::ortho(0.0f, viewport.x, 0.0f, viewport.y, -1.0f, 1.0f);
        mBlitShader.SetUniform("uMVP", proj * glm::translate(glm::mat4(1.0f), glm::vec3(centerFull, 0.0f)));
        mBlitShader.SetUniform("uUVScale", uvScale);
        mBlitShader.SetUniform("uUVOffset", uvOffset);
        mBlitShader.SetUniform("uRectSize", glm::vec2(config.geometry.width, config.geometry.height));
        mBlitShader.SetUniform("uCornerRadius", GeometryUtils::GetEffectiveCornerRadius(config.geometry));
        mBlitShader.SetUniform("uRectCenter", centerFull);
        mBlitShader.SetUniform("uGlowSide", static_cast<int>(config.neon.glowSide));
        mBlitShader.SetUniform("uGlowSideSoftness", config.neon.glowSideSoftness);
        // The four values renderNeonPass gives neon.frag, without the scale. A
        // disabled side goes up as the sentinel, and the shader works out for
        // itself which side glowSide subsumes, exactly as neon.frag does.
        mBlitShader.SetUniform("uInsideCutoff", GetCutoffSize(config.neon.insideCutoff));
        mBlitShader.SetUniform("uInsideCutoffSoftness", config.neon.insideCutoff.softness);
        mBlitShader.SetUniform("uOutsideCutoff", GetCutoffSize(config.neon.outsideCutoff));
        mBlitShader.SetUniform("uOutsideCutoffSoftness", config.neon.outsideCutoff.softness);

        // Just bind it. The filter is requested through Resize in pass 1b
        // (renderNeonPass), so this pass sets no texture parameters at all.
        mScaledBuffer.BindTexture(0);
        mBlitShader.SetUniform("uSource", 0);

        mBlitVertexArray.DrawArrays(GL_TRIANGLES, mBlitVertexCount);
        mBlitShader.Unuse();
    }

    void NeonRenderer::renderRingPass(int viewportWidth, int viewportHeight, const glm::vec2 &gatherUVScale,
                                      const glm::vec2 &gatherUVOffset, float time, const Config &config)
    {
        if (mRingVertexCount == 0)
        {
            return;
        }

        // FULL resolution, on the caller's framebuffer: the same full-res
        // transform and rect-local space as the blit, so the ring lands exactly
        // in the hole the blit area leaves.
        const glm::vec2 centerFull(config.geometry.position.x + config.geometry.width * 0.5f,
                                   static_cast<float>(viewportHeight) - config.geometry.position.y -
                                       config.geometry.height * 0.5f);
        const glm::vec2 viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        const glm::mat4 proj = glm::ortho(0.0f, viewport.x, 0.0f, viewport.y, -1.0f, 1.0f);
        const glm::mat4 mvp = proj * glm::translate(glm::mat4(1.0f), glm::vec3(centerFull, 0.0f));

        // Every shading uniform exactly as the DIRECT path uploads it - scale
        // 1.0, so the Nyquist floor is off and the cut and cutoffs are applied
        // in the shader - fading against the direct path's margin rather than
        // the scaled one. The segment and arc blocks are still bound from
        // packLightBlocks; the LUTs are bound by the upload.
        mNeonRingShader.Use();
        uploadNeonUniforms(mNeonRingShader, mvp, 1.0f, time, mRingQuadMargin, config);

        // What the gather pass produced, in place of running the loop here -
        // the same buffer pass 1 shaded from, through the same region, here
        // mapped from full-res px.
        bindGatherBuffer(mNeonRingShader, gatherUVScale, gatherUVOffset);

        mRingVertexArray.DrawArrays(GL_TRIANGLES, mRingVertexCount);
        mNeonRingShader.Unuse();
    }
}
