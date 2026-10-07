#include "renderer/neon-renderer.h"
#include "renderer/neon-tuning.h"
#include "util/geometry-utils.h"
#include "util/segment-utils.h"
#include "shaders.h"
#include "util/log-util.h"
#include "util/gl-utils.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
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
        /// WHY ONLY THIS BUFFER, GLOW_COVER_FORMATS AND GATHER_FORMATS HAVE A
        /// LIST. They are the only ones that ask for a format a conforming
        /// driver may refuse. RGBA8
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

        /// Glow-coverage-table formats in PREFERENCE ORDER, best first - walked
        /// by @ref NeonRenderer::ensureGlowCoverBuffer. Float for the same
        /// reason as the emission table: the table stores arc coverage TIMES
        /// Arc::intensity, which is unbounded. It is written encoded
        /// c / (1 + c), as the gather buffer is, so the RGBA8 row loses
        /// precision rather than clamping. Half float is texture-filterable in
        /// GLES 3.0 core, which the consumer's linear fetch needs.
        constexpr TargetFormat GLOW_COVER_FORMATS[] = {
            {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, "RGBA16F"},
            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "RGBA8"},
        };
        /// The same list, row for row, with two channels: the table of a config
        /// with no segments, whose .b / .a (the segments' halo and bloom
        /// coverage) are zero at every texel - half the memory, and half the
        /// bytes per fetch. A row's index is its precision tier in both lists,
        /// so one refused tier (mGlowCoverFormat) holds for both.
        constexpr TargetFormat GLOW_COVER_FORMATS_RG[] = {
            {GL_RG16F, GL_RG, GL_HALF_FLOAT, "RG16F"},
            {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, "RG8"},
        };
        static_assert(std::size(GLOW_COVER_FORMATS) == std::size(GLOW_COVER_FORMATS_RG),
                      "the glow coverage table's two format lists are walked by one index");

        /// How long, in seconds of frame time, the glow coverage table may go
        /// unread before Update releases it. Long enough that an arc animation
        /// passing through a full ring once per loop keeps it; short enough
        /// that a ring which has settled uniform gives back its 0.5-1 MB.
        constexpr float GLOW_COVER_RELEASE_SECONDS = 5.0f;

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

        /// The std140 SegmentBlock for @p segments (the merged effective
        /// list), as neon.frag, neon-emission.frag and neon-glow-cover.frag
        /// read it: vec4(position, invSigma, boost, hasStops), capped at the
        /// block's size. One packer for the upload and for the glow coverage
        /// table's dirty pieces, so the two cannot describe different blocks.
        inline SegmentBlockData PackSegmentBlock(const std::vector<SegmentBoost> &segments)
        {
            SegmentBlockData block = {};
            const int count = std::min(static_cast<int>(segments.size()), int(MAX_SEGMENT_BOOSTS));
            block.count = count;
            for (int i = 0; i < count; ++i)
            {
                const SegmentBoost &seg = segments[i];
                const float invSigma = 1.0f / std::max(seg.length * 0.5f, 1e-3f);
                // .w = hasOwnStops flag; the shader reads its colour from row
                // `i` of the segment LUT atlas when set, else falls back to the
                // base gradient at that sample.
                const float hasStops = seg.colorStops.empty() ? 0.0f : 1.0f;
                block.segments[i] = glm::vec4(seg.position, invSigma, seg.boost, hasStops);
            }
            return block;
        }

        /// The std140 ArcBlock for @p arcs: vec4(start, length, intensity,
        /// flags) - .w the PackArcFlags bitmask, not just hasStops - capped at
        /// the block's size. Shared for the reason PackSegmentBlock is.
        inline ArcBlockData PackArcBlock(const std::vector<Arc> &arcs)
        {
            ArcBlockData block = {};
            const int count = std::min(static_cast<int>(arcs.size()), int(MAX_ARCS));
            block.count = count;
            for (int i = 0; i < count; ++i)
            {
                const Arc &arc = arcs[i];
                block.arcs[i] = glm::vec4(arc.start, arc.length, arc.intensity, PackArcFlags(arcs, i, count));
            }
            return block;
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

        /// @p src with `#define NAME` spliced in after its first line for each
        /// space-separated NAME in @p defines.
        ///
        /// How one shader file yields several programs. Every embedded source
        /// starts with the @GLSL_VERSION@ line, which must stay first, so the
        /// defines go immediately after it - ahead of everything else, the
        /// injected tuning header and neon-common.glsl included. Today that is
        /// neon.frag's one variant, the field bake's NEON_FIELD_BAKE; see
        /// @ref NeonRenderer::ensureFieldPrograms.
        inline std::string WithDefine(const char *src, const char *defines)
        {
            std::string lines;
            const std::string names(defines);
            size_t start = 0;
            while (start < names.size())
            {
                const size_t end = names.find(' ', start);
                const std::string name = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!name.empty())
                {
                    lines += "#define " + name + "\n";
                }
                start = (end == std::string::npos) ? names.size() : end + 1;
            }
            std::string out(src);
            const size_t eol = out.find('\n');
            out.insert(eol == std::string::npos ? out.size() : eol + 1, lines);
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
            // survive and the direct path's filament must not widen. Change one and
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

        /// The glow's own reach, in the px space of @p scale: @ref GetGlowMargin
        /// before its outside-cutoff cap, and the value neon.frag recomputes as
        /// `reach` to place its pedestals. Past it the filament and every
        /// straight's bloom are exactly 0 by construction; the halo and the
        /// corner arcs' bloom are not quite - see @ref GetGlowInnerReach.
        inline float GetGlowReach(const Config &config, float scale)
        {
            const float glowReach = config.neon.glowRadius * scale * float(GLOW_REACH_RADIUS_FACTOR) *
                                    (1.0f + config.neon.bloomStrength * config.neon.intensity);
            return std::max(glowReach, GetFilamentExtent(config, scale).reach);
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
            float margin = GetGlowReach(config, scale);

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

        /// An upper bound, per colour channel, on what neon.frag multiplies its
        /// summed halo and bloom by - emitGlow plus the per-piece corrections
        /// in glowFix, which together come to each piece's own coverage times
        /// that piece's term. Both hues are unit magnitude (at most 1 in every
        /// channel), an arc's coverage is at most the brightest arc's
        /// intensity A and a segment's at most the summed boosts S, so the
        /// multiplier is at most
        ///
        ///     intensity * A + S * max(A, min(S, 1))
        ///
        /// the second term being segmentGlow at those two maxima. Takes the
        /// arcs as the shader is handed them (the first MAX_ARCS) and the EFFECTIVE
        /// segments, already capped. A dark arc or a negative boost can only
        /// lower what the shader computes, so counting one high is safe.
        inline float GetGlowEmissionBound(const Config &config, const std::vector<SegmentBoost> &effectiveSegments)
        {
            float arcPeak = 0.0f;
            const size_t arcCount = std::min(config.neon.arcs.size(), static_cast<size_t>(MAX_ARCS));
            for (size_t i = 0; i < arcCount; ++i)
            {
                arcPeak = std::max(arcPeak, config.neon.arcs[i].intensity);
            }
            float boostSum = 0.0f;
            for (const SegmentBoost &segment : effectiveSegments)
            {
                boostSum += std::max(segment.boost, 0.0f);
            }
            return std::max(config.neon.intensity, 0.0f) * arcPeak +
                   boostSum * std::max(arcPeak, std::min(boostSum, 1.0f));
        }

        /// The linear value under which neon.frag's grade writes less than half
        /// an 8-bit level - so an RGBA8 target stores 0, and a blend leaves the
        /// destination exactly as it was. The inverse of
        /// `(x / (x + TONE_MAP_SHOULDER))^GAMMA_EXPONENT` at 0.5 / 255, about
        /// 3.9e-4. The shader's alpha is its brightest channel, so it rounds to
        /// 0 with them.
        inline float GetGlowInvisibleLevel()
        {
            const float q = std::pow(0.5f / 255.0f, 1.0f / static_cast<float>(GAMMA_EXPONENT));
            return static_cast<float>(TONE_MAP_SHOULDER) * q / (1.0f - q);
        }

        /// neon.frag's halo and bloom, reduced to what the CPU's bounds on them
        /// need: the two kernel widths, the corner radius, the arcs' shared
        /// pedestal, and the factor each summed term is scaled by on its way
        /// to the tone map - all in FULL-RES px, at the `reach` the pass at
        /// @p scale computes. ONE mirror for @ref GetGlowInnerReach and
        /// @ref GetCornerSkip, so a change to the shader's terms has one place
        /// to follow it.
        typedef struct GlowBoundTerms
        {
            float reach;       ///< neon.frag's `reach`, full-res px.
            float kh;          ///< Halo kernel width.
            float bw;          ///< Bloom kernel width.
            float r;           ///< Corner radius as the shader is handed it.
            float arcPedestal; ///< The corner arcs' shared bloom pedestal (0 at r = 0).
            float haloScale;   ///< Summed halo -> pre-tone-map light, at the brightest emission.
            float bloomScale;  ///< Summed bloom -> pre-tone-map light, likewise.
        } GlowBoundTerms;

        inline GlowBoundTerms GetGlowBoundTerms(const Config &config, float scale, float emission)
        {
            const NeonConfig &neon = config.neon;
            GlowBoundTerms t;
            t.reach = GetGlowReach(config, scale) / scale;
            t.kh = std::max(neon.glowRadius, static_cast<float>(EMISSION_MIN_WIDTH));
            t.bw = std::max(neon.glowRadius * static_cast<float>(BLOOM_REACH_TO_GLOW),
                            static_cast<float>(EMISSION_MIN_WIDTH));
            t.r = GeometryUtils::GetEffectiveCornerRadius(config.geometry);

            // The bloom's renormalisation and the arcs' shared pedestal, exactly
            // as neon.frag derives them from `reach`.
            const float pi = glm::pi<float>();
            const float bloomPeak = static_cast<float>(BLOOM_NORM_FACTOR) * pi;
            const float bloomPed = bloomPeak * t.bw / std::sqrt(t.reach * t.reach + t.bw * t.bw);
            const float bloomGain = bloomPeak / std::max(bloomPeak - bloomPed, 1e-6f);
            const float arcC = std::sqrt(t.reach * t.reach + t.bw * t.bw);
            const float arcLamPed = std::sqrt((t.reach + t.r) * t.r);
            t.arcPedestal = (t.r > 0.0f) ? t.r / arcLamPed * t.bw / arcC * 2.0f *
                                               std::atan(arcLamPed * glm::half_pi<float>() / (2.0f * arcC))
                                         : 0.0f;

            // glowGate, and the gains each sum is scaled by on its way out.
            const float gate = std::clamp(neon.glowRadius / static_cast<float>(GLOW_GATE_FADE_PX), 0.0f, 1.0f);
            t.haloScale = emission * gate * static_cast<float>(HALO_NORM_FACTOR * HALO_GAIN);
            t.bloomScale = emission * gate * static_cast<float>(BLOOM_NORM_FACTOR) * bloomGain *
                           std::max(neon.bloomStrength, 0.0f);
            return t;
        }

        /// An upper bound on the light ONE corner arc adds, pre tone map, at a
        /// fragment no point of whose development lies nearer than @p dist.
        /// The arc is weighted r / lam over a span lam * HALF_PI
        /// (arcTangentSegment), so lam cancels: its halo is at most
        /// pi r kh^2 / (2 c^3) and its bloom pi r bw / (2 c^2), c measured from
        /// @p dist, the bloom less the shared pedestal. 0 at r = 0.
        inline float GetCornerArcBound(const GlowBoundTerms &t, float dist)
        {
            if (t.r <= 0.0f)
            {
                return 0.0f;
            }
            const float pi = glm::pi<float>();
            const float ch2 = dist * dist + t.kh * t.kh;
            const float halo = pi * t.r * t.kh * t.kh / (2.0f * ch2 * std::sqrt(ch2));
            const float bloom = std::max(pi * t.r * t.bw / (2.0f * (dist * dist + t.bw * t.bw)) - t.arcPedestal, 0.0f);
            return t.haloScale * halo + t.bloomScale * bloom;
        }

        /// How deep inside the rect edge, in FULL-RES px, the glow can still
        /// write a non-zero pixel, for a glow whose emission is at most
        /// @p emission (@ref GetGlowEmissionBound): the interior counterpart
        /// of the quad's margin. CUTOFF_DISABLED_SIZE when that is the middle.
        ///
        /// WHY THE INTERIOR NEEDS THIS. With no inside cutoff, every fragment
        /// inside the rect used to run the full shader - the gather loop
        /// included - whatever the glow did there. On a screen-sized rect most
        /// of the frame is interior, and nearly all of it is further from the
        /// edge than the glow reaches: measured at 1920 x 1080, a 1840 x 1000
        /// rect at glowRadius 2 shaded the whole interior to write zeros, and
        /// an inside cutoff placed past where anything was lit - changing no
        /// pixel - took the frame from 6.0 ms to 2.8 ms (AMD Radeon Pro 5300M).
        /// docs/neon-perf-review.md section 6 had set this aside because the
        /// demo's rect is smaller than twice the glow reach.
        ///
        /// WHY NOT SIMPLY `reach`. Past neon.frag's `reach` the filament and
        /// every straight's bloom are exact zeros (the pedestals; bloomSegment
        /// falls monotonically with distance from its line). Two terms are not:
        ///
        ///   - the halo, which has no pedestal. Its 1/a^2 tail is invisible at
        ///     `reach` at intensity 1, but not at intensity 3 with bloom 0,
        ///     where `reach` stops growing with the brightness: measured lit to
        ///     1.9x `reach` inside the rect, up to 2/255.
        ///   - the corner arcs' bloom. Their ONE shared pedestal is exact for a
        ///     fragment outside an arc and too small on its concave side, where
        ///     arcTangentSegment develops the arc at the fragment's own radius:
        ///     measured lit to 1.3x `reach` inside a circle, up to 6/255.
        ///
        /// The outside does not see either - the quad-edge fade takes every
        /// term to 0 at the margin. The inside has no such fade, and adding one
        /// would change pixels, so the hole is cut where the glow is ALREADY
        /// below half a level instead: the smallest depth D at or past `reach`
        /// at which an upper bound on both terms falls under
        /// @ref GetGlowInvisibleLevel. Every bound holds for any fragment at
        /// least D inside the edge, which is every fragment the hole can hold:
        ///
        ///   - each straight is no brighter than its whole line,
        ///     2 kh^2 / (a^2 + kh^2), and every line is at least D away; the
        ///     two facing each other sum highest where one is as near as it can
        ///     be, so the pair is at most the near one at D and the far one at
        ///     the rect's width (height) less D;
        ///   - each corner arc is weighted r / lam over a span lam * HALF_PI
        ///     (arcTangentSegment), so lam cancels and its halo is at most
        ///     pi r kh^2 / (2 c^3), its bloom pi r bw / (2 c^2), c measured from
        ///     the arc - at least D away - and the bloom less the shader's
        ///     shared pedestal.
        ///
        /// The bound is loose by design - all four arcs at D, two lines at D -
        /// and lands ~20% past `reach` at the defaults on a 1840 x 1000 rect
        /// (368 px against 312, with the last lit pixel at 311). Never tighter
        /// than `reach`, which keeps the exact zeros above exact. Lengths
        /// enter as ratios, so one full-res solve serves every resolution
        /// scale; @p scale only places `reach` (the filament's Nyquist floor
        /// widens it below 1.0).
        ///
        /// A function of the geometry, the glow's shape and @p emission, all of
        /// which rebuild the quad - the first two through geometryDirty, the
        /// third through mGlowEmission (see @ref NeonRenderer::OnConfigChanged).
        inline float GetGlowInnerReach(const Config &config, float scale, float emission)
        {
            const GlowBoundTerms terms = GetGlowBoundTerms(config, scale, emission);
            const float reach = terms.reach; // neon.frag's `reach`, full-res px
            const float width = config.geometry.width;
            const float height = config.geometry.height;
            const float halfMin = std::min(width, height) * 0.5f;
            // No interior past the reach - nothing to bound, and the far lines
            // below assume a depth no larger than this.
            if (reach >= halfMin)
            {
                return reach;
            }

            const float kh = terms.kh;
            auto bound = [&](float depth) {
                const float ch2 = depth * depth + kh * kh;
                const float farW = width - depth;
                const float farH = height - depth;
                const float halo =
                    2.0f * kh * kh * (2.0f / ch2 + 1.0f / (farW * farW + kh * kh) + 1.0f / (farH * farH + kh * kh));
                return terms.haloScale * halo + 4.0f * GetCornerArcBound(terms, depth);
            };

            // Each term falls with depth over [reach, halfMin] - the far lines
            // rise, but never faster than their near partners fall - so the
            // bound is monotone and a bisection finds the crossing.
            const float level = GetGlowInvisibleLevel();
            if (bound(reach) <= level)
            {
                return reach;
            }
            if (bound(halfMin) > level)
            {
                return CUTOFF_DISABLED_SIZE;
            }
            float lo = reach;
            float hi = halfMin;
            while (hi - lo > 0.25f)
            {
                const float mid = 0.5f * (lo + hi);
                if (bound(mid) <= level)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            return hi;
        }

        /// How far outside a corner arc's circle, in the px space of @p scale,
        /// neon.frag may skip that arc altogether (uCornerSkip, addCornerPiece)
        /// for a glow whose emission is at most @p emission.
        ///
        /// Every point of an arc's development lies at least length(w) - r
        /// from the fragment, so @ref GetCornerArcBound bounds what the arc
        /// adds there. This is the smallest such distance at which that bound
        /// is under a QUARTER of @ref GetGlowInvisibleLevel: a fragment can be
        /// that far from all four arcs at once, and four skipped arcs together
        /// then add less than half an 8-bit level. The tone map is concave, so
        /// light that small moves no pixel by more than the rounding of the
        /// one it lands on - wherever it lands, over however much other light.
        ///
        /// The bloom term is what places it in practice: past this distance it
        /// is exactly 0, and the halo's 1/c^3 tail is already far under the
        /// budget. At the defaults it lands within a few px of `reach` (320 px
        /// against 312).
        ///
        /// A function of the glow's shape, the corner radius, @p emission and
        /// @p scale, all of which rebuild the quad, which is where it is taken.
        inline float GetCornerSkip(const Config &config, float scale, float emission)
        {
            const GlowBoundTerms terms = GetGlowBoundTerms(config, scale, emission);
            const float budget = 0.25f * GetGlowInvisibleLevel();
            // GetCornerArcBound falls monotonically with distance, so a
            // doubling search brackets the crossing and a bisection finds it.
            // r = 0 never enters the corner block, and a zero bound skips from
            // the arc itself.
            float lo = 0.0f;
            float hi = std::max(terms.reach, 1.0f);
            if (GetCornerArcBound(terms, lo) <= budget)
            {
                return 0.0f;
            }
            while (GetCornerArcBound(terms, hi) > budget)
            {
                lo = hi;
                hi *= 2.0f;
                if (hi > CUTOFF_DISABLED_SIZE)
                {
                    return CUTOFF_DISABLED_SIZE * scale;
                }
            }
            while (hi - lo > 0.25f)
            {
                const float mid = 0.5f * (lo + hi);
                if (GetCornerArcBound(terms, mid) <= budget)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            return hi * scale;
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

        /// The corner radius the shaders draw, in full-res px: the effective
        /// radius, clamped to half the shorter side as neon-common.glsl's
        /// rectPerimeter clamps it.
        inline float GetDrawnCornerRadius(const Config &config)
        {
            return std::clamp(GeometryUtils::GetEffectiveCornerRadius(config.geometry), 0.0f,
                              std::min(config.geometry.width, config.geometry.height) * 0.5f);
        }

        /// The rect's perimeter in full-res px, as neon-common.glsl's
        /// rectPerimeter measures it at scale 1.
        inline float GetPerimeter(const Config &config)
        {
            const float r = GetDrawnCornerRadius(config);
            return 2.0f * (config.geometry.width + config.geometry.height - 4.0f * r) + glm::two_pi<float>() * r;
        }

        /// Whether no neon.frag program will read the glow coverage table this
        /// frame: one arc over the whole ring and no segments, neon.frag's
        /// `uniformCover`, where every piece's coverage IS the gathered one and
        /// every read is skipped. The bake is then skipped too - under an
        /// animation of anything but the arcs and segments (intensity, colour,
        /// geometry, glow), that is every frame's bake.
        ///
        /// The ONLY test of it: neon.frag takes this decision as a uniform
        /// (uUniformCover) rather than making its own, so the bake it skips
        /// and the reads the shader skips cannot fall out of step. It reads
        /// the inputs packLightBlockData packs - the effective segments and
        /// the arcs - and keeps its threshold a hair above arcCoverContinuous's
        /// full-ring 1 - 1e-6, so a length between the two counts as partial:
        /// the table is baked and read, which is the exact path, rather than
        /// assumed.
        inline bool IsGlowCoverUnread(const std::vector<SegmentBoost> &effectiveSegments, const Config &config)
        {
            return effectiveSegments.empty() && config.neon.arcs.size() == 1 &&
                   config.neon.arcs[0].length >= 1.0f - 5e-7f;
        }

        /// Whether no neon.frag program reads the fragment's perimeter
        /// position (`uPerimeterUnread`): it feeds only the segment loop, the
        /// arc loop - where an arc over the whole ring returns before reading
        /// it, and an arc's own stops read it - and the gradient ring's alpha
        /// read, which a ring that is opaque at every texel answers with 1.0.
        /// So: no segments, every lit arc whole and without stops, and the
        /// ring as uploaded opaque (@p ringOpaque). Then the shader skips
        /// perimeterPosition and the alpha read, byte-identically.
        ///
        /// Like IsGlowCoverUnread it must never claim this where the shader
        /// would read: it tests the same arcs the block packs (capped at
        /// MAX_ARCS, dark ones skipped as the shader's loop skips them) and
        /// the length against 1 - 5e-7, a hair stricter than
        /// arcCoverContinuous's 1 - 1e-6.
        inline bool IsPerimeterUnread(const std::vector<SegmentBoost> &effectiveSegments, const Config &config,
                                      bool ringOpaque)
        {
            if (!effectiveSegments.empty() || !ringOpaque)
            {
                return false;
            }
            const int count = std::min(static_cast<int>(config.neon.arcs.size()), int(MAX_ARCS));
            for (int i = 0; i < count; ++i)
            {
                const Arc &arc = config.neon.arcs[i];
                if (arc.intensity <= 0.0f)
                {
                    continue;
                }
                if (arc.length < 1.0f - 5e-7f || !arc.colorStops.empty())
                {
                    return false;
                }
            }
            return true;
        }

        /// Whether neon.frag multiplies anything after its tone map - the
        /// one-sided cut or a cutoff. Without either every such multiply is by
        /// exactly 1.0 (a disabled cutoff's sentinel puts its smoothstep at an
        /// end). Never below 1.0: there neon-blit.frag applies the cut and the
        /// cutoffs (neon.frag's blitOwnsCut), so the shading multiplies by
        /// nothing after its tone map.
        inline bool MasksAfterGrade(const Config &config)
        {
            return GetClampedResolutionScale(config) >= 1.0f &&
                   (config.neon.glowSide != GlowSide::BOTH || config.neon.insideCutoff.enable ||
                    config.neon.outsideCutoff.enable);
        }

        /// Whether this config's shading can be factored into the hue-invariant
        /// field (pass 1f) and its composite (pass 1c): what neon.frag writes
        /// must be tonemap(col * Fa), with only the gathered hue col moving
        /// with time. Not with segments - their hue enters as a second
        /// product, and the field would need a channel for it - and not when
        /// time can move the colour-stop alpha neon.frag reads pointwise: a
        /// rotating hue over a ring that is not opaque at every texel
        /// (@p ringOpaque, as uploaded). And not at 1.0 with a one-sided glow
        /// or a cutoff (MasksAfterGrade): the field is Fa alone, one channel,
        /// and those configs shade directly - the mask channel they needed
        /// served no frame below 1.0, where the blit applies both.
        ///
        /// At 1.0 the composite stands in for pass 1, which draws onto the
        /// caller's framebuffer on every frame. Below it, it stands in for pass
        /// 1b, which mOffscreenCurrent already skips on every frame where
        /// nothing moved - so the only frames left for a field to serve are the
        /// ones where only the time moved, and so only under a rotating hue.
        /// Every other way a settled config redraws pass 1b (a cross-fade's
        /// ring upload, a viewport change) invalidates the field too. A field
        /// for a still ring below 1.0 would cost its memory and its compile
        /// and never be read. A function of the config and the ring's upload.
        inline bool IsFieldEligible(const std::vector<SegmentBoost> &effectiveSegments, const Config &config,
                                    bool ringOpaque)
        {
            const bool rotating = config.neon.hueRotationRate != 0.0f;
            return config.neon.enable && effectiveSegments.empty() && (!rotating || ringOpaque) &&
                   !MasksAfterGrade(config) && (GetClampedResolutionScale(config) >= 1.0f || rotating);
        }

        static_assert(GLOW_COVER_SHARED >= 2 * GLOW_COVER_MIN_INTERIOR,
                      "a band of the glow coverage table must leave columns for both its pieces");

        /// How many of each band's GLOW_COVER_SHARED columns go to its
        /// straight, the rest going to its corner (neon-pieces.glsl,
        /// glowCoverInner): .x for the two vertical straights' bands, .y for
        /// the two horizontal ones'. In proportion to the straight's and the
        /// quarter arc's lengths, so every piece gets columns in proportion to
        /// its length - a circle's corners take what its straights do not
        /// need - and each keeps GLOW_COVER_MIN_INTERIOR. Whole columns, and
        /// computed HERE once for the bake and every neon.frag program alike:
        /// the two shaders work in different units, and a split each rounded
        /// for itself could land a column apart and read the whole band from
        /// the wrong texels. A function of the geometry alone.
        inline glm::vec2 GetGlowCoverSplit(const Config &config)
        {
            const float r = GetDrawnCornerRadius(config);
            const float arc = glm::half_pi<float>() * r;
            const float shared = float(GLOW_COVER_SHARED);
            const float minInner = float(GLOW_COVER_MIN_INTERIOR);
            auto split = [&](float straight) {
                const float total = straight + arc;
                const float inner = (total > 0.0f) ? std::round(shared * straight / total) : std::round(shared * 0.5f);
                return std::clamp(inner, minInner, shared - minInner);
            };
            return glm::vec2(split(std::max(config.geometry.height - 2.0f * r, 0.0f)),
                             split(std::max(config.geometry.width - 2.0f * r, 0.0f)));
        }

        /// The glow coverage table's eight pieces, numbered as its bands hold
        /// them: piece 2b is band b's straight, piece 2b + 1 its corner. One
        /// bit each in NeonRenderer::mGlowCoverDirty.
        constexpr int GLOW_COVER_PIECES = 8;
        constexpr uint32_t GLOW_COVER_ALL_PIECES = (1u << GLOW_COVER_PIECES) - 1u;
        static_assert(GLOW_COVER_ALL_PIECES == 0xFFu,
                      "NeonRenderer::mGlowCoverDirty starts at 0xFF: every piece, which is this");

        /// Where one piece lies on the perimeter, in the same [0, 1) parameter
        /// the arcs and segments are placed in: it runs from @c start for
        /// @c length in the direction @c sign.
        typedef struct GlowCoverPieceSpan
        {
            float start;
            float sign;
            float length;
        } GlowCoverPieceSpan;

        /// neon-glow-cover.frag's pieceStart, verbatim.
        inline GlowCoverPieceSpan GlowCoverPieceStart(float cwStart, float cwSigma, float arcLen, float length,
                                                      Winding winding)
        {
            if (winding == Winding::CLOCKWISE)
            {
                return GlowCoverPieceSpan{cwStart, cwSigma, length};
            }
            const float s = 1.0f - arcLen - cwStart;
            return GlowCoverPieceSpan{s - std::floor(s), -cwSigma, length};
        }

        /// Every piece's span, from the same lengths renderGlowCoverPass hands
        /// the bake and through the same placement - straightStart and
        /// cornerStart in neon-glow-cover.frag, mirrored here. The table's
        /// dirty pieces are only exact while the two agree: change where the
        /// bake places a piece and this has to follow.
        inline std::array<GlowCoverPieceSpan, GLOW_COVER_PIECES> GetGlowCoverPieceSpans(const Config &config)
        {
            const float perimeter = std::max(GetPerimeter(config), 1e-3f);
            const float radius = GetDrawnCornerRadius(config);
            const float ws = std::max(config.geometry.width - 2.0f * radius, 0.0f) / perimeter;
            const float hs = std::max(config.geometry.height - 2.0f * radius, 0.0f) / perimeter;
            const float arcLen = glm::half_pi<float>() * (radius / perimeter);
            const Winding w = config.geometry.winding;
            std::array<GlowCoverPieceSpan, GLOW_COVER_PIECES> spans;
            // Bands 0 and 1 hold the vertical straights (length hs), 2 and 3
            // the horizontal ones (ws); band b's corner has signs
            // ((b == 1 || b == 3) ? +1 : -1, (b >= 2) ? +1 : -1).
            spans[0] = GlowCoverPieceStart(2.0f * ws + 3.0f * arcLen + hs, 1.0f, arcLen, hs, w);
            spans[2] = GlowCoverPieceStart(ws + arcLen + hs, -1.0f, arcLen, hs, w);
            spans[4] = GlowCoverPieceStart(2.0f * ws + 2.0f * arcLen + hs, -1.0f, arcLen, ws, w);
            spans[6] = GlowCoverPieceStart(0.0f, 1.0f, arcLen, ws, w);
            for (int band = 0; band < 4; ++band)
            {
                const float sx = (band == 1 || band == 3) ? 1.0f : -1.0f;
                const float sy = (band >= 2) ? 1.0f : -1.0f;
                const float cw = (sx > 0.0f) ? ws + arcLen + ((sy > 0.0f) ? 0.0f : hs)
                                             : 2.0f * ws + 3.0f * arcLen + hs + ((sy > 0.0f) ? hs : 0.0f);
                spans[2 * band + 1] = GlowCoverPieceStart(cw, -sx * sy, arcLen, arcLen, w);
            }
            return spans;
        }

        /// Slack added to every span tested against a piece, in perimeter
        /// fractions: float round-off between this mirror and the bake, where
        /// a support that only touches a piece's end adds nothing either way.
        constexpr float GLOW_COVER_SPAN_SLACK = 1e-3f;

        /// The pieces whose perimeter span meets [@p lo, @p hi] (perimeter
        /// fractions, any wrap; hi - lo below 1).
        inline uint32_t GlowCoverPiecesTouching(const std::array<GlowCoverPieceSpan, GLOW_COVER_PIECES> &spans,
                                                float lo, float hi)
        {
            lo -= GLOW_COVER_SPAN_SLACK;
            hi += GLOW_COVER_SPAN_SLACK;
            if (hi - lo >= 1.0f)
            {
                return GLOW_COVER_ALL_PIECES;
            }
            const float shift = std::floor(lo);
            lo -= shift;
            hi -= shift;
            uint32_t mask = 0;
            for (int i = 0; i < GLOW_COVER_PIECES; ++i)
            {
                const GlowCoverPieceSpan &p = spans[i];
                float p0 = (p.sign > 0.0f) ? p.start : p.start - p.length;
                p0 -= std::floor(p0);
                const float p1 = p0 + p.length;
                // Both in [0, 2); one lap either way covers every overlap.
                for (int lap = -1; lap <= 1; ++lap)
                {
                    if (lo + static_cast<float>(lap) <= p1 && p0 <= hi + static_cast<float>(lap))
                    {
                        mask |= 1u << i;
                        break;
                    }
                }
            }
            return mask;
        }

        /// Which pieces of the glow coverage table moved between the light
        /// blocks @p oldArcs / @p oldSegments and @p newArcs / @p newSegments,
        /// for a config whose shape, winding and glow radius did not change
        /// (those move every piece, and the caller says so itself).
        ///
        /// Exact because every texel of neon-glow-cover.frag integrates the
        /// arcs' and segments' coverage over ITS OWN piece and nothing else:
        /// pieceCover clips each arc's trapezoid and each segment's bell to
        /// [0, len] of the piece, and normalises by the kernel's mass over the
        /// same span. So a light whose support misses a piece leaves that
        /// piece's texels exactly as they were, and a changed light dirties
        /// the pieces its old support and its new support meet. Compared as
        /// PACKED, so a neighbour's move that changes an arc's abut flags -
        /// and with them its trapezoid - counts as a change to that arc.
        ///
        /// What reaches every piece instead, and so dirties all of them:
        ///   - the brightest arc's intensity, which arcsOnPiece clamps every
        ///     piece's coverage to (`most`, over arcs with intensity above 0
        ///     and length above 1e-6 - the same test, so an arc crossing either
        ///     threshold moves it too);
        ///   - an arc over the whole ring (length >= 1 - 1e-6), which lights
        ///     every piece without a trapezoid;
        ///   - a segment whose bell reaches round the whole ring (the bake's
        ///     reach, min(5 / (sqrt(2) invSigma), 0.5), at its cap of 0.5).
        /// The supports are the bake's own: an arc's trapezoid lies inside
        /// [start - tail feather, start + length + head feather], a segment's
        /// bell is cut at position +/- reach.
        inline uint32_t GetGlowCoverDirtyPieces(const ArcBlockData &oldArcs, const ArcBlockData &newArcs,
                                                const SegmentBlockData &oldSegments,
                                                const SegmentBlockData &newSegments, const Config &config)
        {
            const std::array<GlowCoverPieceSpan, GLOW_COVER_PIECES> spans = GetGlowCoverPieceSpans(config);
            const float perimeter = std::max(GetPerimeter(config), 1e-3f);
            const float tailFeather = static_cast<float>(TAIL_FEATHER_PX) / perimeter;
            const float headFeather = static_cast<float>(HEAD_FEATHER_PX) / perimeter;

            auto lights = [](const glm::vec4 &arc) { return arc.z > 0.0f && arc.y > 1e-6f; };
            auto brightest = [&](const ArcBlockData &block) {
                float most = 0.0f;
                for (int i = 0; i < block.count; ++i)
                {
                    if (lights(block.arcs[i]))
                    {
                        most = std::max(most, block.arcs[i].z);
                    }
                }
                return most;
            };
            if (brightest(oldArcs) != brightest(newArcs))
            {
                return GLOW_COVER_ALL_PIECES;
            }

            uint32_t mask = 0;
            auto arcSupport = [&](const glm::vec4 &arc) -> uint32_t {
                if (!lights(arc))
                {
                    return 0u;
                }
                if (arc.y >= 1.0f - 1e-6f)
                {
                    return GLOW_COVER_ALL_PIECES;
                }
                return GlowCoverPiecesTouching(spans, arc.x - tailFeather, arc.x + arc.y + headFeather);
            };
            const int arcCount = std::max(oldArcs.count, newArcs.count);
            for (int i = 0; i < arcCount; ++i)
            {
                const glm::vec4 none(0.0f);
                const glm::vec4 &before = (i < oldArcs.count) ? oldArcs.arcs[i] : none;
                const glm::vec4 &after = (i < newArcs.count) ? newArcs.arcs[i] : none;
                if (before != after)
                {
                    mask |= arcSupport(before) | arcSupport(after);
                }
            }

            auto segmentSupport = [&](const glm::vec4 &seg) -> uint32_t {
                if (seg.z <= 0.0f)
                {
                    return 0u;
                }
                if (seg.y <= 1e-6f)
                {
                    return GLOW_COVER_ALL_PIECES;
                }
                const float reach = std::min(5.0f * 0.7071067811865476f / seg.y, 0.5f);
                if (reach >= 0.5f)
                {
                    return GLOW_COVER_ALL_PIECES;
                }
                return GlowCoverPiecesTouching(spans, seg.x - reach, seg.x + reach);
            };
            const int segmentCount = std::max(oldSegments.count, newSegments.count);
            for (int i = 0; i < segmentCount; ++i)
            {
                const glm::vec4 none(0.0f);
                const glm::vec4 &before = (i < oldSegments.count) ? oldSegments.segments[i] : none;
                const glm::vec4 &after = (i < newSegments.count) ? newSegments.segments[i] : none;
                if (before != after)
                {
                    mask |= segmentSupport(before) | segmentSupport(after);
                }
            }
            return mask;
        }

        /// The scale the GATHER runs at, at every resolutionScale: as coarse as
        /// the gather's own smoothness allows, never finer than @p scale nor
        /// than GATHER_MAX_SCALE.
        ///
        /// The gather produces the perimeter hue and the two gathered coverages,
        /// each a Lorentzian-weighted mean over the whole perimeter with kernel
        /// width kc = perimeter * COLOR_BLEND_PERIM_FRAC (neon.frag's `kc`) -
        /// and nowhere narrower: at a distance d from the line the kernel is
        /// sqrt(d^2 + kc^2) wide. So a grid GATHER_TEXELS_PER_KERNEL texels per
        /// kc carries it through a bilinear read, which the edge ring already
        /// relies on, and the loop - ~95% of the neon's cost when it ran inline
        /// - runs on that grid instead of on every texel the shading needs; for
        /// anything but a small rect that is far below 1 (0.11 for a 640 x 360
        /// rect). Floored at GATHER_MIN_SCALE so a very large rect does not pin
        /// a viewport-sized gather to a handful of texels, and capped at
        /// GATHER_MAX_SCALE so a tiny one does not gather its whole glow quad
        /// at full resolution - see neon-tuning.h for both.
        ///
        /// A function of the geometry and @p scale only, so it moves only under
        /// geometryDirty, which rebuilds the gather pass's quad with it. It
        /// moves continuously under a size animation; GetBufferRegion is what
        /// keeps that from reallocating the buffer every frame.
        inline float GetGatherScale(const Config &config, float scale)
        {
            const float kc = std::max(GetPerimeter(config) * static_cast<float>(COLOR_BLEND_PERIM_FRAC),
                                      static_cast<float>(EMISSION_MIN_WIDTH));
            const float floorScale = std::min(static_cast<float>(GATHER_MIN_SCALE), scale);
            const float ceilScale = std::min(static_cast<float>(GATHER_MAX_SCALE), scale);
            return std::clamp(static_cast<float>(GATHER_TEXELS_PER_KERNEL) / kc, floorScale, ceilScale);
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

        /// The hue-invariant field (pass 1f) is used only where the glow quad
        /// fills at least this share of the box the field has to cover. The
        /// field is a box - its texels sit on the viewport's pixels - while the
        /// quad may be a thin frame: the production band's quad is ~7% of its
        /// box, so its field was ~8 MB for a frame that measured 1.02x, the
        /// shading there being too little for the field to save. At a half the
        /// field holds at most two texels per shaded pixel. The same share
        /// below 1.0, counted in the reduced buffer's texels.
        constexpr float FIELD_MIN_FILL = 0.5f;

        /// The box the field covers. At 1.0 the glow quad's, clipped to the
        /// viewport, on the viewport's pixel grid. Below it exactly the reduced
        /// buffer's region (the blit's read extent @p scaledOuter, as pass 1b
        /// draws it), so the field and @c mScaledBuffer share one texel grid
        /// and pass 1c writes each texel of the one from the same texel of the
        /// other.
        inline BufferRegion GetFieldRegion(const Config &config, const glm::vec2 &glowOuter,
                                           const glm::vec2 &scaledOuter, int viewportWidth, int viewportHeight)
        {
            const glm::vec2 centerFull(config.geometry.position.x + config.geometry.width * 0.5f,
                                       static_cast<float>(viewportHeight) - config.geometry.position.y -
                                           config.geometry.height * 0.5f);
            const float scale = GetClampedResolutionScale(config);
            if (scale < 1.0f)
            {
                return GetBufferRegion(scaledOuter, centerFull, viewportWidth, viewportHeight, scale, true);
            }
            return GetBufferRegion(glowOuter, centerFull, viewportWidth, viewportHeight, 1.0f, true);
        }

        /// Whether a glow quad of @p glowArea FULL-RES px, drawn at @p scale,
        /// fills enough of @p region for the field to be worth its memory
        /// (FIELD_MIN_FILL).
        inline bool FieldFillsRegion(float glowArea, const BufferRegion &region, float scale)
        {
            const float box = static_cast<float>(region.texels.x) * static_cast<float>(region.texels.y);
            return glowArea * scale * scale >= FIELD_MIN_FILL * box;
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
        // NOT the glow coverage table, although its size is also a pair of
        // compile-time constants: at 1 MB it is most of what this renderer
        // allocates, and a ring lit uniformly - the default - never reads it.
        // It is allocated on the first frame that bakes it; see
        // ensureGlowCoverBuffer.
        //
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

        // The atlas bakes and the glow quad's interior hole read the merged
        // transient+preserved view, which OnConfigChanged normally keeps
        // current; seed it here for the first, before either.
        SegmentUtils::FillEffectiveSegments(mCurrentConfig.neon, mEffectiveSegments);
        rebuildLoopSamples(mCurrentConfig);
        setupGeometry(mCurrentConfig);
        setupFillGeometry(mCurrentConfig);
        // After setupGeometry: the blit's outer frame reads the glow margin it
        // computes.
        setupRingGeometry(mCurrentConfig);
        bakeLUTs(mCurrentConfig);

        setupFullscreenQuad();
        mInitialized = true;
        return true;
    }

    void NeonRenderer::Update(float deltaTime, float, const Config &config)
    {
        // A fade frame re-uploads the ring the emission table is baked FROM,
        // without any config change to announce it. Nothing here has to say
        // so: the upload moves the ring's upload count, which is part of the
        // table's key (isEmissionTableStale), and so does anything else that
        // writes it.
        mGradientLUT.Tick(deltaTime);

        // Give the glow coverage table back once nothing has read it for
        // GLOW_COVER_RELEASE_SECONDS: a ring lit uniformly (or a disabled
        // layer, which OnConfigChanged already releases for) never samples it,
        // and I33 kept it allocated through a uniform stretch only because an
        // arc animation passes through one every loop. The delay keeps that
        // case; a ring that has settled frees its 0.5-1 MB. Here and not in
        // Render, which must not delete a framebuffer (Framebuffer::Release).
        // mEffectiveSegments is current: the effect refreshes the config, and
        // with it OnConfigChanged, before it forwards Update.
        if (!mGlowCoverBuffer.IsValid())
        {
            mGlowCoverUnreadSeconds = 0.0f;
        }
        else if (!config.neon.enable || IsGlowCoverUnread(mEffectiveSegments, config))
        {
            mGlowCoverUnreadSeconds += std::max(deltaTime, 0.0f);
            if (mGlowCoverUnreadSeconds >= GLOW_COVER_RELEASE_SECONDS)
            {
                // A fresh allocation sets every piece dirty, so the table that
                // replaces this one is baked whole on the frame it is next read.
                mGlowCoverBuffer.Release();
                mGlowCoverUnreadSeconds = 0.0f;
            }
        }
        else
        {
            mGlowCoverUnreadSeconds = 0.0f;
        }
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
        // frame degrades to the fill: no glow, nothing stale. The glow
        // coverage table's program and buffer likewise, but only on a ring
        // that reads the table. The gather is its own pass at every scale.
        bool glowReady = ensurePathPrograms(scaled) &&
                         (IsGlowCoverUnread(mEffectiveSegments, config) ||
                          (ensureGlowCoverProgram() && ensureGlowCoverBuffer(!mEffectiveSegments.empty())));

        // The render target this renderer was handed - framebuffer AND
        // viewport, saved as a pair because the offscreen phase has to put both
        // back. The framebuffer is not always the window's: an offscreen frame
        // capture (@ref OffscreenCapture) binds a real FBO, so a retargeting
        // pass must return to what was bound rather than assuming 0. Read
        // BEFORE any pass binds a target of its own - querying later would
        // capture that.
        //
        // Only on a frame whose offscreen phase draws anything: one that
        // reuses its offscreen buffers (mOffscreenCurrent) and bakes no field
        // never leaves the caller's framebuffer, so there is nothing to come
        // back to.
        //
        // @ref renderEmissionPass captures its own rather than being handed
        // this one: a pass restores what IT finds, which is what keeps it
        // correct wherever it is called from.
        RenderTargetState prevTarget;
        // Scaled rect-local px -> gather buffer uv, for pass 1b. Set with the
        // gather's region below.
        glm::vec2 gatherUVScale(0.0f);
        glm::vec2 gatherUVOffset(0.0f);
        // Full-res rect-local px -> buffer uv, for the passes on the caller's
        // framebuffer: the edge ring's (or, at 1.0, pass 1's) onto the gather
        // buffer and the blit's onto the reduced one. Set with
        // the regions below.
        glm::vec2 gatherUVFullScale(0.0f);
        glm::vec2 gatherUVFullOffset(0.0f);
        glm::vec2 blitUVScale(0.0f);
        glm::vec2 blitUVOffset(0.0f);

        // What the offscreen phase has to draw this frame. The light blocks
        // are packed first, because the emission table's staleness reads
        // their upload counts - the pack touches no framebuffer state, so the
        // target is still the caller's for the capture below.
        if (glowReady)
        {
            packLightBlocks(config);
        }
        // Pass 0 only when something it reads has actually moved - see
        // isEmissionTableStale. A still ring therefore costs one FBO bind,
        // eight uniform sets, three texture binds and a draw on the frame its
        // inputs change, and nothing on the frames after.
        const bool emissionStale = glowReady && isEmissionTableStale(time, config);
        // Pass 0b on a change to its inputs only. It never depends on time,
        // and nothing else writes the buffer. Nor on a ring lit uniformly,
        // which never reads it: the flag stays set, so the first change that
        // breaks the uniformity bakes it.
        const bool glowCoverStale = glowReady && mGlowCoverDirty != 0 && !IsGlowCoverUnread(mEffectiveSegments, config);
        // Passes 1a and 1b not at all when what their buffers already hold is
        // what they would draw - see mOffscreenCurrent. Then the frame never
        // leaves the caller's framebuffer, so there is no target to capture
        // either.
        const bool reuseOffscreen = glowReady && mOffscreenCurrent && !emissionStale &&
                                    !glowCoverStale && mOffscreenViewport == glm::ivec2(viewportWidth, viewportHeight);
        // The shading factored into the hue-invariant field (1f) and its
        // composite (1c), where the config allows it: in pass 1's place at
        // 1.0, in pass 1b's below it. The field outlives the frame; besides a
        // config change (OnConfigChanged), the viewport and a gradient ring
        // upload - a cross-fade frame - invalidate it here. It is baked only
        // once nothing invalidated it on the frame before, so an animation
        // that changes the config every frame keeps drawing the shading
        // directly and never pays a bake it cannot reuse.
        //
        // Except on the FIRST frame a config the field can serve is drawn,
        // which builds the field's two programs and bakes the field at once,
        // while still shading directly. Building alone is not enough: this
        // driver finishes a program's compile on its first DRAW, in the target
        // and blend state it draws with, so a bake program only linked here
        // left a ~125 ms stall on the bake frame after it (~155 ms all told,
        // AMD Radeon Pro 5300M). And the frame still shades directly so that
        // the shading program's first draw lands here too - compositing
        // instead deferred THAT stall to the first config change. The price is
        // one bake an animated config may not reuse, once per renderer. A
        // frame below 1.0 that reuses its offscreen buffers has no pass 1b for
        // the field to replace, and the first frame never does.
        bool useField = false;
        bool bakeField = false;
        const BufferRegion fieldRegion =
            GetFieldRegion(config, mGlowOuter, mScaledOuter, viewportWidth, viewportHeight);
        const bool fieldProgramsBuilt = mNeonFieldShader.IsValid() && mFieldCompositeShader.IsValid();
        if (glowReady && !mFieldUnavailable && IsFieldEligible(mEffectiveSegments, config, mGradientLUT.IsOpaque()) &&
            FieldFillsRegion(mGlowArea, fieldRegion, scale) && ensureFieldPrograms() && !(scaled && reuseOffscreen))
        {
            const glm::ivec2 viewport(viewportWidth, viewportHeight);
            if (viewport != mFieldViewport || mGradientLUT.GetUploadCount() != mFieldGradientUploads)
            {
                mFieldViewport = viewport;
                mFieldGradientUploads = mGradientLUT.GetUploadCount();
                mFieldCurrent = false;
                mFieldSettled = false;
            }
            if (mFieldCurrent)
            {
                useField = true;
            }
            else if (mFieldSettled)
            {
                bakeField = true;
                useField = true;
            }
            else if (!fieldProgramsBuilt)
            {
                bakeField = true;
            }
            else
            {
                mFieldSettled = true;
            }
        }
        if (!reuseOffscreen || bakeField)
        {
            prevTarget = RenderTargetState::Capture();
        }

        // ===== Offscreen phase ===============================================
        if (glowReady)
        {
            // --- Pass 0: per-sample emission table --------------------------
            if (emissionStale)
            {
                // A table write is not a composite: blending would mix this
                // frame's emission into last frame's. Every later pass sets its
                // own blend mode, so leaving this off changes nothing
                // downstream.
                glDisable(GL_BLEND);
                renderEmissionPass(viewportWidth, viewportHeight, time, config);
            }

            // --- Pass 0b: the glow coverage table, when glowCoverStale says.
            if (glowCoverStale)
            {
                glDisable(GL_BLEND);
                renderGlowCoverPass(config);
            }

            // How pass 1f runs, at either scale: bake the field over @p region
            // - the one its composite draws (GetFieldRegion) - through that
            // region's projection at this frame's scale, after the gather it
            // reads. Unblended: the buffer is data. Records how pass 1c finds
            // a fragment's texel: the region's origin and its texels per px,
            // both in the composite's vPos space (scaled px; at 1.0 the
            // field's own px, at exactly one texel per px). A failed bake
            // composites nothing: the frame shades directly.
            auto bakeFieldPass = [&](const BufferRegion &region, const glm::vec2 &uvScale,
                                     const glm::vec2 &uvOffset) {
                const bool baked = renderFieldPass(RegionProjection(region, scale), region.texels.x,
                                                   region.texels.y, uvScale, uvOffset, scale, time, config);
                useField = useField && baked;
                mFieldCurrent = baked;
                mFieldUnavailable = !baked;
                mFieldOrigin = region.origin * scale;
                mFieldTexelScale = glm::vec2(region.texels) / (region.size * scale);
            };

            // The offscreen buffers cover a REGION of the frame, not the
            // viewport (GetBufferRegion): each pass draws through an ortho
            // onto its region, in pass 1's scaled rect-local space - where
            // every scaled quad lives - and each reader maps its own
            // rect-local px onto the buffer's uv.
            //
            // --- Pass 1a: the gather, at its own coarse scale, into the
            // gather buffer. Unblended: the buffer is data. Read by pass 1b
            // from scaled px and by the ring from full-res px - or, at 1.0,
            // by pass 1 itself.
            const BufferRegion gatherRegion = GetBufferRegion(
                mGatherOuter, centerFull, viewportWidth, viewportHeight, GetGatherScale(config, scale), false);
            gatherUVOffset = -gatherRegion.origin / gatherRegion.size;
            gatherUVScale = glm::vec2(1.0f) / (gatherRegion.size * scale);
            gatherUVFullScale = glm::vec2(1.0f) / gatherRegion.size;
            gatherUVFullOffset = gatherUVOffset;
            // The region and its maps are computed either way: the passes
            // on the caller's framebuffer read through them.
            if (!reuseOffscreen)
            {
                glDisable(GL_BLEND);
                glowReady = renderGatherPass(RegionProjection(gatherRegion, scale), gatherRegion.texels.x,
                                             gatherRegion.texels.y, scale, config);
            }

            if (scaled)
            {
                // --- Pass 1b: the shading, at the reduced scale, into the
                // reduced buffer, from that gather. Still unblended: the buffer
                // was just cleared and the quad covers each texel once, so
                // premultiplied-over would only add zero, at the price of a
                // destination read per texel. A failed allocation in either
                // pass skips both composites below, so a failed frame degrades
                // to the fill rather than compositing a stale buffer from an
                // earlier frame. Read by the blit, from full-res px.
                //
                // Or, where the field holds - under a rotating hue, once the
                // config has held for a frame - pass 1c in its place, into the
                // same buffer: the field times this frame's gathered hue, with
                // pass 1f baking the field first on the frame that needs it.
                // The field's region IS this buffer's (GetFieldRegion), so each
                // composite fragment reads the field texel under it.
                if (glowReady)
                {
                    const BufferRegion scaledRegion =
                        GetBufferRegion(mScaledOuter, centerFull, viewportWidth, viewportHeight, scale, true);
                    blitUVScale = glm::vec2(1.0f) / scaledRegion.size;
                    blitUVOffset = -scaledRegion.origin / scaledRegion.size;
                    if (!reuseOffscreen)
                    {
                        const glm::mat4 scaledProj = RegionProjection(scaledRegion, scale);
                        if (bakeField)
                        {
                            bakeFieldPass(fieldRegion, gatherUVScale, gatherUVOffset);
                        }
                        if (useField)
                        {
                            glowReady = renderFieldCompositePass(scaledProj, scaledRegion.texels.x,
                                                                 scaledRegion.texels.y, true, gatherUVScale,
                                                                 gatherUVOffset);
                        }
                        else
                        {
                            glowReady = renderNeonPass(scaledProj, scaledRegion.texels.x, scaledRegion.texels.y,
                                                       true, gatherUVScale, gatherUVOffset, time, config);
                        }
                    }
                }
            }
            else if (bakeField && glowReady)
            {
                // --- Pass 1f at 1.0: over the glow quad's box at full
                // resolution, its texel grid on the viewport's pixel grid,
                // read back by pass 1c on the caller's framebuffer below.
                glDisable(GL_BLEND);
                bakeFieldPass(fieldRegion, gatherUVFullScale, gatherUVFullOffset);
            }

            if (!reuseOffscreen || bakeField)
            {
                // Back to the caller's target and viewport, both at once.
                // Unconditional: the pass may have bound its target before
                // failing, and leaving the caller on our buffer would silently
                // redirect every renderer after this one.
                prevTarget.Restore();
            }
        }
        // What the offscreen buffers hold from here on: this frame's passes 1a
        // and 1b, drawn or reused - unless a pass failed or the path has none.
        mOffscreenCurrent = glowReady;
        mOffscreenViewport = glm::ivec2(viewportWidth, viewportHeight);

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
                // --- Pass 1 (direct): the shading, composited onto the
                // target, reading pass 1a's result through its full-res map
                // (the same map the edge ring reads it through) - or, where
                // the field holds this frame, pass 1c in its place: the field
                // times the gathered hue.
                if (useField)
                {
                    renderFieldCompositePass(mvp, bufW, bufH, false, gatherUVFullScale, gatherUVFullOffset);
                }
                else
                {
                    renderNeonPass(mvp, bufW, bufH, false, gatherUVFullScale, gatherUVFullOffset, time, config);
                }
            }
            else
            {
                // --- Pass 2b / 2c: the blit and the edge ring. They cover
                // disjoint areas (setupRingGeometry), so their order between
                // themselves does not matter for the result.
                //
                // ONE full-res transform, built here and handed to both. Their
                // partition holds only if the edges they share land on
                // bit-identical pixel positions, which takes the same vertex
                // floats (setupRingGeometry), the same vertex stage (neon.vert's
                // invariant gl_Position) and the same uMVP. Two copies of one
                // expression gave the same bits too, until someone edited one
                // of them; a single matrix cannot drift. tools/neon-scale-check
                // `partition` tests the result.
                const glm::mat4 fullResMvp =
                    glm::ortho(0.0f, static_cast<float>(viewportWidth), 0.0f, static_cast<float>(viewportHeight),
                               -1.0f, 1.0f) *
                    glm::translate(glm::mat4(1.0f), glm::vec3(centerFull, 0.0f));
                renderBlitPass(fullResMvp, centerFull, blitUVScale, blitUVOffset, config);
                renderRingPass(fullResMvp, gatherUVFullScale, gatherUVFullOffset, time, config);
            }
        }

        // Restore a known blend state for following renderers.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void NeonRenderer::OnConfigChanged(const Config &config)
    {
        // The offscreen buffers were drawn from the config this replaces. Wide,
        // unlike the gates below - pass 1b reads most of the neon's config, and
        // re-drawing them costs one frame's offscreen phase - but no wider than
        // what this renderer reads at all: its own sub-config and the geometry
        // (Render reads debug.opaqueOnly too, and returns before any of this).
        // This call comes whenever the COMPOSITED config changed, another
        // layer's fields included, and an animation of one - the lens flare's
        // sun riding the perimeter, an AnimatableField - changes it every
        // frame. Cleared on that, the field never settled and the neon drew
        // pass 1 directly every frame: 4.9x with the hue rotating and 15x
        // still at 1.0, ~4x still at 0.5 (AMD Radeon Pro 5300M, 1920 x 1080),
        // for an image the field draws byte-identically.
        if (config.neon != mCurrentConfig.neon || config.geometry != mCurrentConfig.geometry)
        {
            mOffscreenCurrent = false;
            // And the hue-invariant field, for the same reason, and one more
            // frame before it is baked again: a config that changes every
            // frame (an animation) never settles, and keeps drawing pass 1
            // directly.
            mFieldCurrent = false;
            mFieldSettled = false;
        }

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
        const bool arcsDirty = config.neon.arcs != mCurrentConfig.neon.arcs;
        // The glow coverage table's inputs, exactly: the two light blocks
        // (segmentsDirty and arcsDirty, as for mLightBlocksDirty below) and
        // what renderGlowCoverPass reads off the config - the perimeter, the
        // drawn corner radius and the straights, all from width, height and
        // cornerRadius; the winding; and the glow radius. Not the rect's
        // POSITION: the table is in perimeter units, so a moving rect bakes
        // nothing. Add a uniform to that pass and it belongs here.
        //
        // The config-side half moves every piece of the table; the light
        // blocks move only the pieces a changed light reaches, worked out
        // below once the merged segment list is current
        // (GetGlowCoverDirtyPieces).
        const bool glowCoverShapeDirty = config.geometry.width != mCurrentConfig.geometry.width ||
                                         config.geometry.height != mCurrentConfig.geometry.height ||
                                         config.geometry.cornerRadius != mCurrentConfig.geometry.cornerRadius ||
                                         config.geometry.winding != mCurrentConfig.geometry.winding ||
                                         config.neon.glowRadius != mCurrentConfig.neon.glowRadius;
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
        // The segment block as the glow coverage table last saw it, packed
        // from the merged list before the refill below replaces it.
        const SegmentBlockData oldSegmentBlock = PackSegmentBlock(mEffectiveSegments);
        if (segmentsDirty)
        {
            SegmentUtils::FillEffectiveSegments(config.neon, mEffectiveSegments);
        }

        // The quad's interior hole is sized for the brightest emission the
        // glow can carry (GetGlowInnerReach), which moves with the arcs'
        // intensities and the segments' boosts - inputs geometryDirty does not
        // otherwise have. Gated on that one number, after the refill above,
        // rather than on the two lists: a segment travelling round the ring
        // changes its list every frame and the number never. Miss it and a
        // boost raised under a hole cut for a dimmer glow clips its halo.
        const bool emissionDirty = GetGlowEmissionBound(config, mEffectiveSegments) != mGlowEmission;

        // The emission table needs nothing from here. It is keyed on what its
        // pass binds - two uniforms by value, three LUTs and two light blocks
        // by upload count (isEmissionTableStale) - so a change that moves one
        // of those re-bakes it through that count, and a change that moves
        // none (intensity, bloom, the glow, the rect) re-bakes nothing.
        //
        // The glow coverage table is gated here instead, on the narrow,
        // visible set of inputs above. Gated wide, it re-ran every frame under
        // any animation at all - intensity, colour, or a field of another
        // layer entirely - for a table none of those move. And per PIECE: a
        // light that moved re-bakes only the pieces its old and new supports
        // reach, so a segment travelling along one straight re-bakes that
        // straight's band, not the whole table. Accumulated, for the reasons
        // given for mLightBlocksDirty just below.
        if (glowCoverShapeDirty)
        {
            mGlowCoverDirty = GLOW_COVER_ALL_PIECES;
        }
        else if (segmentsDirty || arcsDirty)
        {
            mGlowCoverDirty |= GetGlowCoverDirtyPieces(PackArcBlock(mCurrentConfig.neon.arcs),
                                                       PackArcBlock(config.neon.arcs), oldSegmentBlock,
                                                       PackSegmentBlock(mEffectiveSegments), config);
        }
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
        // compares against the arcs the first one already installed. A
        // narrow gate has to hold until the pack clears it.
        mLightBlocksDirty = mLightBlocksDirty || segmentsDirty || arcsDirty;

        mCurrentConfig = config;

        // Give the scaled buffers back the moment this config stops wanting
        // them - the layer switched off, or the scale returned to 1.0. With the
        // glow coverage table below they are the only allocations in this
        // renderer that are not a handful of KB: at 1920x1080 and scale 0.5
        // the reduced buffer is 2.1 MB of colour attachment, and nothing else
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
        }
        // The glow coverage table, 1 MB at RGBA16F, is given back only when the
        // layer is switched off - not when the ring turns uniform and stops
        // reading it, which an arc animation reaching length 1 does once per
        // loop, and which would then reallocate it on the way back. Released
        // or never allocated, ensureGlowCoverBuffer reallocates it, and marks
        // it for a bake, on the next frame that reads it. The gather buffer
        // likewise: every frame gathers into it at every scale, so only a
        // disabled layer has no use for it.
        if (!config.neon.enable)
        {
            mGlowCoverBuffer.Release();
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

        if (geometryDirty || emissionDirty)
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

        // The field - the renderer's largest buffer at 1.0 - goes with any
        // config that cannot use it: segments, or below 1.0 a hue that stopped
        // rotating. Not on the ring's opacity, which is the upload's rather
        // than the config's: a rotating hue over a ring that is not opaque
        // keeps a field it does not read until the next change.
        if (!IsFieldEligible(mEffectiveSegments, config, true) ||
            !FieldFillsRegion(mGlowArea,
                              GetFieldRegion(config, mGlowOuter, mScaledOuter, mFieldViewport.x, mFieldViewport.y),
                              GetClampedResolutionScale(config)))
        {
            mFieldBuffer.Release();
            mFieldCurrent = false;
        }
    }

    bool NeonRenderer::setupShaders()
    {
        // Only the two programs both resolution paths draw with. The neon.frag
        // programs and the blit are per PATH and are built the first time that
        // path renders - see ensurePathPrograms - and the glow coverage bake
        // the first time a table is needed - see ensureGlowCoverProgram.
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

    bool NeonRenderer::ensureGlowCoverProgram()
    {
        // Built on first use rather than in Initialize, and for a reason with
        // a number on it: it is the largest program the renderer compiles
        // before a frame, ~10 ms on an AMD Radeon Pro 5300M - the compiler
        // there builds every function in a source whether main() reaches it
        // or not - and a host whose ring is lit uniformly never bakes the
        // table at all (IsGlowCoverUnread), so never needs it. Initialize
        // drops from 8.8 ms (V20's bake, built there) to ~6 ms; a host with a
        // partly lit ring pays the compile on its first such frame, where
        // ensurePathPrograms already builds the path's own programs.
        if (mGlowCoverShader.IsValid())
        {
            return true;
        }
        if ((mFailedPrograms & PROGRAM_GLOW_COVER) != 0)
        {
            return false;
        }
        // The neon vertex shader over the NDC quad, the fragment shader keyed
        // off gl_FragCoord - the emission pre-pass's arrangement.
        mGlowCoverShader = ShaderProgram(ShaderSource::NEON_VERT_SRC,
                                         ShaderSource::NEON_GLOW_COVER_FRAG_SRC,
                                         "NeonRenderer.GlowCover");
        if (!mGlowCoverShader.IsValid())
        {
            mFailedPrograms |= PROGRAM_GLOW_COVER;
            LOG_E("NeonRenderer: the glow coverage bake failed to compile/link - a partly lit ring will draw no glow.");
            return false;
        }
        mGlowCoverShader.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        mGlowCoverShader.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        return true;
    }

    bool NeonRenderer::buildNeonProgram(ShaderProgram &program, const char *fragSrc, const char *define,
                                        const char *name, unsigned int programBit, bool gathers, bool shades)
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

        const std::string source = define ? WithDefine(fragSrc, define) : std::string(fragSrc);
        program = ShaderProgram(ShaderSource::NEON_VERT_SRC, source.c_str(), name);
        if (!program.IsValid())
        {
            mFailedPrograms |= programBit;
            LOG_E("NeonRenderer: %s failed to compile/link - that resolution path will draw no glow.", name);
            return false;
        }
        // Every neon program reads the segment block (neon-common.glsl), at
        // the same binding. Only the ones that run the gather have the sample
        // block, and only the ones that shade have the arc block - the gather
        // pass sees arcs only through the emission table. Binding a block a
        // program does not declare logs an error, hence the two flags.
        program.SetUniformBlockBinding("SegmentBlock", SEGMENT_BLOCK_BINDING);
        if (shades)
        {
            program.SetUniformBlockBinding("ArcBlock", ARC_BLOCK_BINDING);
        }
        if (gathers)
        {
            program.SetUniformBlockBinding("LoopSamplesBlock", LOOP_SAMPLES_BLOCK_BINDING);
        }
        return true;
    }

    bool NeonRenderer::ensurePathPrograms(bool scaled)
    {
        // The direct path draws with the gather pass and the edge ring's
        // program, which shades the whole glow quad from the gather buffer.
        // The ring's program is the one that fits: it already draws the
        // caller's framebuffer, blended, at full resolution, from that buffer,
        // and the two paths never both draw in one frame - so it still draws
        // one target in one blend state per frame (see below).
        if (!scaled)
        {
            return buildNeonProgram(mNeonGatherShader, ShaderSource::NEON_GATHER_FRAG_SRC, nullptr,
                                    "NeonRenderer.Gather", PROGRAM_GATHER, true, false) &&
                   buildNeonProgram(mNeonRingShader, ShaderSource::NEON_FRAG_SRC, nullptr, "NeonRenderer.Ring",
                                    PROGRAM_RING, false, true);
        }

        // The scaled path draws with four, two of them the direct path's: the
        // gather pass (neon-gather.frag), the shading that reads it back -
        // TWICE, one program object for pass 1 at the reduced scale and the
        // edge ring's at full resolution - and the composite. So a host that
        // never leaves 1.0 never compiles pass 1b's program or the blit.
        //
        // Why the shading is compiled twice from one source: each program
        // object then draws ONE render target, in one blend state, per frame.
        // One program drawing two differently configured targets in a frame -
        // an offscreen buffer unblended and the caller's framebuffer blended -
        // is the shape that measured 93-624x slower on the AMD macOS driver
        // (docs/neon-resolution-scale-plan.md section 7, build AB). A second
        // compile on the first scaled frame is the price of never finding out
        // which half of that difference the driver keys on.
        if (!buildNeonProgram(mNeonGatherShader, ShaderSource::NEON_GATHER_FRAG_SRC, nullptr, "NeonRenderer.Gather",
                              PROGRAM_GATHER, true, false) ||
            !buildNeonProgram(mNeonShadeShader, ShaderSource::NEON_FRAG_SRC, nullptr, "NeonRenderer.Shade",
                              PROGRAM_SHADE, false, true) ||
            !buildNeonProgram(mNeonRingShader, ShaderSource::NEON_FRAG_SRC, nullptr, "NeonRenderer.Ring",
                              PROGRAM_RING, false, true))
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
        // collapses the hole below and gives back the plain quad - so every
        // config that lights its own interior is untouched.
        // MUTUALLY EXCLUSIVE, mirroring the cutoff neutralisation in neon.frag:
        // under GlowSide::OUTSIDE the inside cutoff is subsumed by the cut and
        // the shader ignores it, so taking a min() with it here would be worse
        // than pointless. It would shrink the hole to the cutoff's reach while
        // the shader still lights out to the guard band, and the quad would
        // clip what the blit reconstructs the cut from - the same dark seam by
        // a second route. With insideCutoff size 0 at scale 0.25 that put the
        // hole at 1.25 buffer px against a 2.0 px guard.
        //
        // A THIRD bound, which needs no setting at all: the glow's own reach.
        // Deeper than GetGlowInnerReach the glow writes 0 whatever the
        // cutoffs say, so a rect larger than twice that has an interior the
        // quad never needed - on a screen-sized rect, most of the frame. It
        // ANDs with the inside cutoff like the two discards do, so it is a
        // min() with it; under OUTSIDE the cut is nearer than either. No
        // guard band on this one, unlike the cutoff's: the blit only needs
        // lit texels past a boundary it applies a mask at, and the texels in
        // this hole are 0 in truth as well as in the cleared buffer. The
        // default 800 x 600 rect, and the demo's half-viewport one, are
        // smaller than twice it at the default glow, so both still draw the
        // plain quad.
        mGlowEmission = GetGlowEmissionBound(config, mEffectiveSegments);
        mGlowInnerReach = GetGlowInnerReach(config, scale, mGlowEmission);
        // Where neon.frag may skip a corner arc outright, for pass 1 and for
        // the edge ring - from the same emission bound, so under the same gate.
        mCornerSkip = GetCornerSkip(config, scale, mGlowEmission);
        mRingCornerSkip = GetCornerSkip(config, 1.0f, mGlowEmission);
        float innerReach = CUTOFF_DISABLED_SIZE;
        if (config.neon.glowSide == GlowSide::OUTSIDE)
        {
            innerReach = sideCullPx;
        }
        else
        {
            if (config.neon.insideCutoff.enable)
            {
                // neon.frag discards at dIn < -(inHalf + cutGuard), i.e. past the
                // end of the inside fade - the point GetCutoffEnd computes - plus
                // the scaled path's guard band.
                innerReach = GetCutoffEnd(config.neon.insideCutoff, CUTOFF_FLOOR_PX) + cutGuardPx;
            }
            innerReach = std::min(innerReach, mGlowInnerReach);
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
        // half-extents to 0. One past only the SHORTER half-extent drives that
        // one alone to 0, which is a hole of no area just the same - see the
        // test below.
        const float radius = GeometryUtils::GetEffectiveCornerRadius(config.geometry) * scale;
        const float holeRadius = std::max(radius - innerMargin, 0.0f);
        const float cornerInset = holeRadius * CORNER_INSET_FACTOR;
        const float iw = std::max(halfW - innerMargin - cornerInset, 0.0f);
        const float ih = std::max(halfH - innerMargin - cornerInset, 0.0f);

        // The quad in FULL-RES px, for the gather pass's quad to cover - see
        // setupRingGeometry. A hole exists only when both extents are positive.
        mGlowOuter = glm::vec2(ow, oh) / scale;
        mGlowHole = (iw > 0.0f && ih > 0.0f) ? glm::vec2(iw, ih) / scale : glm::vec2(0.0f);
        mGlowArea = 4.0f * (mGlowOuter.x * mGlowOuter.y - mGlowHole.x * mGlowHole.y);

        // No hole to cut: emit the plain quad, byte for byte the geometry this
        // method has always produced. Kept as its own arm rather than letting
        // the ring degenerate into it so that every config without an inner
        // bound - which includes the default - is provably unchanged by this,
        // rather than relying on the rasteriser's fill rule to make eight
        // triangles land exactly where two did.
        //
        // EITHER extent at 0, not both: a hole with no height is no hole, and
        // the glow's own reach routinely lands between the two half-extents -
        // at the defaults it is 312 px, past the default rect's 300 px
        // half-height and short of its 400 px half-width. Tested with `&&`, that
        // sent the default config through the eight-triangle ring - two strips
        // meeting at y = 0 - for no fill saved, and moved the pixels the
        // interpolation across the new diagonals rounds differently.
        if (iw <= 0.0f || ih <= 0.0f)
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
        // there is nothing to partition - only the gather pass's quad to
        // build. Keyed on the clamped scale rather than on UsesScaledBuffer,
        // because `enable` is not in this method's dirty gate and nothing here
        // depends on it.
        const float scale = GetClampedResolutionScale(config);
        if (scale >= 1.0f)
        {
            mRingVertexCount = 0;
            mBlitVertexCount = 0;
            mScaledOuter = glm::vec2(0.0f);
            // Pass 1 reads the gather at every pixel of the glow quad and
            // nothing else, so the gather covers that quad, grown by the
            // bilinear footprint at both edges - the same construction as
            // below, with no ring to union in.
            const float pad = FOOTPRINT_TEXELS / GetGatherScale(config, 1.0f) + 1.0f;
            const glm::vec2 outer = mGlowOuter + glm::vec2(pad);
            const glm::vec2 hole = (mGlowHole.x > 0.0f && mGlowHole.y > 0.0f)
                                       ? glm::max(mGlowHole - glm::vec2(pad), glm::vec2(0.0f))
                                       : glm::vec2(0.0f);
            std::vector<float> verts;
            verts.reserve(48);
            PushAnnulus(verts, outer, hole);
            mGatherOuter = outer;
            mGatherVertexArray.SetVertexData(verts.data(), verts.size() * sizeof(float), GL_DYNAMIC_DRAW);
            mGatherVertexCount = static_cast<int>(verts.size() / 2);
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
        //     plus a pixel past the margin is that footprint. The same holds
        //     INWARD past mGlowInnerReach, where every texel is under half a
        //     level - stored as 0 - drawn by pass 1 or not.
        //
        // Never inside the ring's own boxes, so the two arrays still tile.
        const EdgeExtent lit = GetLitExtent(config);
        const float glowBound = (mQuadMargin + FOOTPRINT_TEXELS) / scale + 1.0f;
        const float glowInnerBound = mGlowInnerReach + FOOTPRINT_TEXELS / scale + 1.0f;
        const float blitOut = std::max(ring.out, std::min(glowBound, lit.out + LIT_EDGE_SAFETY));
        const float blitIn = std::max(ring.in, std::min(glowInnerBound, lit.in + LIT_EDGE_SAFETY));
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
        // mEffectiveSegments is NOT refilled here. OnConfigChanged refills it
        // whenever either segment pool changes, and this runs once per frame
        // from Render - so on a frame where the pools did not move the merged
        // view is already current, and on a frame where they did,
        // OnConfigChanged has already run (Update -> refreshActiveConfig
        // precedes Render). Refilling here would be a second
        // FillEffectiveSegments of the same frame.
        const SegmentBlockData segBlock = PackSegmentBlock(mEffectiveSegments);
        mSegmentBlock.SetData(&segBlock, sizeof(segBlock));

        // The arcs, with .w the PackArcFlags bitmask: it picks between the
        // winner arc's own atlas row and the base gradient in the shader's
        // winner-take-all branch, and carries the abut bits.
        const ArcBlockData arcBlock = PackArcBlock(config.neon.arcs);
        mArcBlock.SetData(&arcBlock, sizeof(arcBlock));
    }

    NeonRenderer::EmissionInputs NeonRenderer::currentEmissionInputs(const Config &config) const
    {
        // Exactly what renderEmissionPass uploads and binds, besides uTime.
        // Add an input to that pass and it belongs here, or the table goes
        // stale whenever that input alone moves.
        EmissionInputs inputs;
        inputs.hueRotationRate = config.neon.hueRotationRate;
        inputs.numSamples = GetClampedNumSamples(config);
        inputs.gradientUploads = mGradientLUT.GetUploadCount();
        inputs.segmentAtlasUploads = mSegmentLUT.GetUploadCount();
        inputs.arcAtlasUploads = mArcLUT.GetUploadCount();
        inputs.segmentBlockUploads = mSegmentBlock.GetUploadCount();
        inputs.arcBlockUploads = mArcBlock.GetUploadCount();
        return inputs;
    }

    bool NeonRenderer::isEmissionTableStale(float time, const Config &config) const
    {
        if (!mEmissionBaked || currentEmissionInputs(config) != mEmissionInputs)
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
        mEmissionInputs = currentEmissionInputs(config);
        mEmissionBaked = true;
        mEmissionTime = time;
    }

    bool NeonRenderer::ensureGlowCoverBuffer(bool segments)
    {
        // Allocated is enough: the table's size is fixed, so a live buffer has
        // nothing to resize to, and this runs every frame a partly lit ring
        // draws - unless segments have appeared on a two-channel table, which
        // has nowhere to put them. The other way round keeps what it has: a
        // four-channel table with no segments reads its .b / .a as zeros like
        // a two-channel one, and dropping back to two would reallocate the
        // table every time a transient segment came and went.
        if (mGlowCoverBuffer.IsValid())
        {
            const GLint format = mGlowCoverBuffer.GetInternalFormat();
            const bool twoChannel = (format == GL_RG16F || format == GL_RG8);
            if (!segments || !twoChannel)
            {
                return true;
            }
        }

        // The gather buffer's walk, over GLOW_COVER_FORMATS (or its RG twin),
        // with a linear filter: the consumer interpolates between neighbouring
        // positions round each piece. Resumed from mGlowCoverFormat, because
        // this buffer is released with the layer and cannot record a refused
        // format in its own attachment - so a driver that refused RGBA16F once
        // is not asked again on every re-enable, and logs once.
        for (; mGlowCoverFormat < std::size(GLOW_COVER_FORMATS); ++mGlowCoverFormat)
        {
            const TargetFormat &f =
                segments ? GLOW_COVER_FORMATS[mGlowCoverFormat] : GLOW_COVER_FORMATS_RG[mGlowCoverFormat];
            if (mGlowCoverBuffer.Resize(GLOW_COVER_WIDTH, GLOW_COVER_HEIGHT,
                                        f.internalFormat, f.format, f.type, GL_LINEAR))
            {
                // Undefined texels until a bake writes them - whatever the
                // flag said about the buffer this one replaces.
                mGlowCoverDirty = GLOW_COVER_ALL_PIECES;
                return true;
            }
            if (mGlowCoverFormat + 1 < std::size(GLOW_COVER_FORMATS))
            {
                LOG_E("NeonRenderer: %s glow coverage target unavailable, falling back to %s. "
                      "The halo and bloom coverage will be stored at 8 bits.",
                      f.name,
                      (segments ? GLOW_COVER_FORMATS : GLOW_COVER_FORMATS_RG)[mGlowCoverFormat + 1].name);
            }
        }
        // Every candidate refused: leave the index on the last one, so the next
        // frame that needs the table retries the format most likely to
        // allocate, as resizeGatherBuffer does.
        mGlowCoverFormat = std::size(GLOW_COVER_FORMATS) - 1;
        return false;
    }

    void NeonRenderer::renderGlowCoverPass(const Config &config)
    {
        // Hand back the framebuffer and viewport exactly as found, and keep a
        // host scissor off the table's texels - both for the reasons
        // renderEmissionPass gives.
        const RenderTargetState prevTarget = RenderTargetState::Capture();
        GLUtils::NoScissorScope noScissor;

        // Binds the FBO and sets the viewport to the table. No clear: the NDC
        // quad covers every texel.
        mGlowCoverBuffer.Bind();

        // Every length as a fraction of the full-res perimeter, which is what
        // lets the reduced-scale shading and the full-res ring share the
        // table: their px lengths all scale together. The widths are
        // neon.frag's kh and bw at scale 1, the straights its 2 * straight.
        const float perimeter = std::max(GetPerimeter(config), 1e-3f);
        const float glowRadius = config.neon.glowRadius;
        const float haloWidth = std::max(glowRadius, static_cast<float>(EMISSION_MIN_WIDTH));
        const float bloomWidth = std::max(glowRadius * static_cast<float>(BLOOM_REACH_TO_GLOW),
                                          static_cast<float>(EMISSION_MIN_WIDTH));
        const float radius = GetDrawnCornerRadius(config);
        const glm::vec2 straights(std::max(config.geometry.width - 2.0f * radius, 0.0f),
                                  std::max(config.geometry.height - 2.0f * radius, 0.0f));
        mGlowCoverShader.Use();
        mGlowCoverShader.SetUniform("uMVP", glm::mat4(1.0f));
        mGlowCoverShader.SetUniform("uHeadFeather", static_cast<float>(HEAD_FEATHER_PX) / perimeter);
        mGlowCoverShader.SetUniform("uTailFeather", static_cast<float>(TAIL_FEATHER_PX) / perimeter);
        mGlowCoverShader.SetUniform("uHaloWidth", haloWidth / perimeter);
        mGlowCoverShader.SetUniform("uBloomWidth", bloomWidth / perimeter);
        mGlowCoverShader.SetUniform("uStraightSize", straights / perimeter);
        mGlowCoverShader.SetUniform("uRadius", radius / perimeter);
        mGlowCoverShader.SetUniform("uWinding", static_cast<int>(config.geometry.winding));
        const glm::vec2 split = GetGlowCoverSplit(config);
        mGlowCoverShader.SetUniform("uGlowCoverSplit", split);
        if (mGlowCoverDirty == GLOW_COVER_ALL_PIECES)
        {
            mFullscreenVertexArray.DrawArrays(GL_TRIANGLES, 6);
        }
        else
        {
            // Only the dirty pieces, each band's straight and corner as one
            // rectangle where both are dirty. Bounded by GEOMETRY, not a
            // scissor: the bake keys every texel off gl_FragCoord, so the NDC
            // quad drawn through a matrix onto a texel-aligned rectangle writes
            // exactly that rectangle's texels and no others (its edges sit on
            // whole texels, and every centre is half a texel inside one), and
            // nothing of the host's scissor state - box included, which
            // NoScissorScope does not restore - is touched.
            const float width = static_cast<float>(GLOW_COVER_WIDTH);
            const float height = static_cast<float>(GLOW_COVER_HEIGHT);
            for (int band = 0; band < 4; ++band)
            {
                const bool straight = (mGlowCoverDirty & (1u << (2 * band))) != 0;
                const bool corner = (mGlowCoverDirty & (1u << (2 * band + 1))) != 0;
                if (!straight && !corner)
                {
                    continue;
                }
                // The band's straight is the columns left of the split, its
                // corner the rest - neon-glow-cover.frag's own test.
                const float inner = (band < 2) ? split.x : split.y;
                const float splitColumn = static_cast<float>(2 * GLOW_COVER_OVERHANG) + inner;
                const float x0 = straight ? 0.0f : splitColumn;
                const float x1 = corner ? width : splitColumn;
                const float y0 = static_cast<float>(band * GLOW_COVER_ROWS);
                const float y1 = y0 + static_cast<float>(GLOW_COVER_ROWS);
                const glm::mat4 rect =
                    glm::translate(glm::mat4(1.0f), glm::vec3((x0 + x1) / width - 1.0f, (y0 + y1) / height - 1.0f, 0.0f)) *
                    glm::scale(glm::mat4(1.0f), glm::vec3((x1 - x0) / width, (y1 - y0) / height, 1.0f));
                mGlowCoverShader.SetUniform("uMVP", rect);
                mFullscreenVertexArray.DrawArrays(GL_TRIANGLES, 6);
            }
        }
        mGlowCoverShader.Unuse();

        prevTarget.Restore();
        mGlowCoverDirty = 0;
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
                                          float time, float quadMargin, float cornerSkip, const Config &config)
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
        shader.SetUniform("uPerimeterUnread",
                          IsPerimeterUnread(mEffectiveSegments, config, mGradientLUT.IsOpaque()) ? 1 : 0);
        shader.SetUniform("uUniformCover", IsGlowCoverUnread(mEffectiveSegments, config) ? 1 : 0);

        // The three LUT atlases are no longer read by the gather (the emission
        // pre-pass consumes them instead), but the pointwise path still samples
        // them for the colour-stop alpha - see the alpha reads in neon.frag.
        mGradientLUT.Bind(0);
        shader.SetUniform("uGradientLUT", 0);
        mSegmentLUT.Bind(1);
        shader.SetUniform("uSegmentLUT", 1);
        mArcLUT.Bind(2);
        shader.SetUniform("uArcLUT", 2);
        // The glow coverage table from pass 0b, on its own unit - 3 and 4 are
        // the emission table or the gather buffer, depending on the path.
        //
        // On a ring lit uniformly it may never have been allocated, and the
        // shader's uniformCover branch then never reads it - IsGlowCoverUnread,
        // which skips the allocation, is the stricter of the two tests. The
        // unit still gets a COMPLETE texture rather than 0: Apple's driver logs
        // a sampler bound to an unloadable texture at draw time whether or not
        // it is read, and the direct path drew without that line before the
        // table became lazy. The gradient ring stands in - already bound on
        // unit 0 for this same program, so baked, and never a render target,
        // so no feedback loop with any pass. Never sampled through this unit.
        if (mGlowCoverBuffer.IsValid())
        {
            mGlowCoverBuffer.BindTexture(5);
        }
        else
        {
            mGradientLUT.Bind(5);
        }
        shader.SetUniform("uGlowCover", 5);
        shader.SetUniform("uGlowCoverSplit", GetGlowCoverSplit(config));
        shader.SetUniform("uQuadMargin", quadMargin);
        shader.SetUniform("uCornerSkip", cornerSkip);
    }

    bool NeonRenderer::renderNeonPass(const glm::mat4 &mvp, int bufWidth, int bufHeight, bool scaled,
                                      const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset, float time,
                                      const Config &config)
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

        // Both paths shade from the gather pass's result: the scaled path
        // with its own program, the direct path with the edge ring's, which
        // draws the same target in the same blend state (see
        // ensurePathPrograms). Every other uniform is the same.
        ShaderProgram &shader = scaled ? mNeonShadeShader : mNeonRingShader;
        shader.Use();
        uploadNeonUniforms(shader, mvp, scale, time, mQuadMargin, mCornerSkip, config);
        bindGatherBuffer(shader, gatherUVScale, gatherUVOffset);

        // Tight glow quad in both modes - opaque's far region is covered by the
        // fill pass, so the gather never runs fullscreen.
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, mGlowVertexCount);
        shader.Unuse();
        return true;
    }

    bool NeonRenderer::ensureFieldPrograms()
    {
        // The bake is neon.frag reading the gather (as the shading does at
        // either scale) with NEON_FIELD_BAKE: hue 1 in, field out. It shades,
        // so it takes the arc block; it does not gather, so not the sample
        // block. One object for both scales: it only ever draws the field
        // buffer, unblended, and the composite draws one target per frame.
        if (!buildNeonProgram(mNeonFieldShader, ShaderSource::NEON_FRAG_SRC, "NEON_FIELD_BAKE",
                              "NeonRenderer.Field", PROGRAM_FIELD, false, true))
        {
            mFieldUnavailable = true;
            return false;
        }
        if (mFieldCompositeShader.IsValid())
        {
            return true;
        }
        if ((mFailedPrograms & PROGRAM_FIELD_COMPOSITE) != 0)
        {
            mFieldUnavailable = true;
            return false;
        }
        mFieldCompositeShader = ShaderProgram(ShaderSource::NEON_VERT_SRC, ShaderSource::NEON_FIELD_FRAG_SRC,
                                              "NeonRenderer.FieldComposite");
        if (!mFieldCompositeShader.IsValid())
        {
            mFailedPrograms |= PROGRAM_FIELD_COMPOSITE;
            mFieldUnavailable = true;
            LOG_E("NeonRenderer: the field composite failed to compile/link - the glow is shaded directly.");
            return false;
        }
        return true;
    }

    bool NeonRenderer::renderFieldPass(const glm::mat4 &mvp, int width, int height, const glm::vec2 &gatherUVScale,
                                       const glm::vec2 &gatherUVOffset, float scale, float time,
                                       const Config &config)
    {
        // An offscreen target: the host's scissor is in the wrong coordinates
        // here - see renderNeonPass.
        GLUtils::NoScissorScope noScissor(true);

        // Half float, one channel: Fa. No 8-bit fallback: Fa runs well past 1
        // and the tone map is steep near 0, so 8 bits would move whole levels
        // - a driver that cannot render to half float shades directly instead.
        // NEAREST: the composite reads one texel per fragment, its own.
        if (!mFieldBuffer.Resize(width, height, GL_R16F, GL_RED, GL_HALF_FLOAT, GL_NEAREST))
        {
            LOG_E("NeonRenderer: no half-float target for the hue-invariant field - the glow is shaded directly.");
            return false;
        }
        // Cleared to 0: a texel neon.frag discards composites to nothing.
        mFieldBuffer.Bind();
        mFieldBuffer.ClearBuffer();

        // Every uniform the shading takes at this scale - pass 1 at 1.0, pass
        // 1b below it - from the same upload, so the bake shades exactly what
        // that pass would, with the hue at 1.
        mNeonFieldShader.Use();
        uploadNeonUniforms(mNeonFieldShader, mvp, scale, time, mQuadMargin, mCornerSkip, config);
        bindGatherBuffer(mNeonFieldShader, gatherUVScale, gatherUVOffset);
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, mGlowVertexCount);
        mNeonFieldShader.Unuse();
        return true;
    }

    bool NeonRenderer::renderFieldCompositePass(const glm::mat4 &mvp, int bufWidth, int bufHeight, bool scaled,
                                                const glm::vec2 &gatherUVScale, const glm::vec2 &gatherUVOffset)
    {
        // Below 1.0 pass 1b's target, set up as renderNeonPass sets it up -
        // the host's scissor lifted, the reduced buffer sized, bound and
        // cleared - for the same reasons, given there. At 1.0 the caller's
        // framebuffer, as pass 1 draws it.
        GLUtils::NoScissorScope noScissor(scaled);
        if (scaled)
        {
            if (!mScaledBuffer.Resize(bufWidth, bufHeight, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR))
            {
                return false;
            }
            mScaledBuffer.Bind();
            mScaledBuffer.ClearBuffer();
        }

        // The glow quad the shading draws, through its transform: the same
        // fragments, each reading its own field texel.
        mFieldCompositeShader.Use();
        mFieldCompositeShader.SetUniform("uMVP", mvp);
        mFieldBuffer.BindTexture(6);
        mFieldCompositeShader.SetUniform("uField", 6);
        mFieldCompositeShader.SetUniform("uFieldOrigin", mFieldOrigin);
        mFieldCompositeShader.SetUniform("uFieldTexelScale", mFieldTexelScale);
        mGatherBuffer.BindTexture(3, 0);
        mFieldCompositeShader.SetUniform("uGather", 3);
        mFieldCompositeShader.SetUniform("uGatherUVScale", gatherUVScale);
        mFieldCompositeShader.SetUniform("uGatherUVOffset", gatherUVOffset);
        mGlowVertexArray.DrawArrays(GL_TRIANGLES, mGlowVertexCount);
        mFieldCompositeShader.Unuse();
        return true;
    }

    void NeonRenderer::bindGatherInputs(ShaderProgram &shader, const Config &config)
    {
        // Loop sample positions come from the LoopSamplesBlock UBO (see
        // neon-gather.frag) - raw float32 vec4[N], .xy holds the perimeter point in
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
        // Attachment 1 exists only with segments. Without them the shader's
        // uSegmentCount branch never reads uGatherSeg, but the unit still gets
        // a COMPLETE texture - attachment 0 again - rather than texture 0:
        // Apple's driver logs a sampler bound to an unloadable texture at draw
        // time whether or not it is read (the I33 stand-in, for unit 5, has
        // the same reason). Attachment 0 is this same frame's input, never a
        // target of the pass that reads it, so there is no feedback loop.
        mGatherBuffer.BindTexture(3, 0);
        shader.SetUniform("uGather", 3);
        mGatherBuffer.BindTexture(4, mGatherBuffer.GetAttachmentCount() > 1 ? 1 : 0);
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
        // shading, which neon-gather.frag does not have, and ShaderProgram
        // logs an error for each uniform it is handed that the program lacks.
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

    void NeonRenderer::renderBlitPass(const glm::mat4 &mvp, const glm::vec2 &centerFull, const glm::vec2 &uvScale,
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

        // FULL-RES geometry, under the full-res transform Render built for
        // this pass and the ring together - not Render's scaled one; the same
        // y mirror as @ref renderOpaqueFill, the other always-full-res pass.
        // uGlowSideSoftness goes up UNSCALED for the same reason: this pass
        // measures in destination pixels, the gather measures in buffer ones.
        //
        // The area this pass covers is everything outside the edge ring that
        // can still be lit (setupRingGeometry) - mBlitVertexArray, in full-res
        // rect-local px. vPos is then rect-local px rather than NDC, and maps
        // onto the reduced buffer through @p uvScale / @p uvOffset - its
        // region's map (GetBufferRegion), which Render derives with the region.
        // The ring finds its gather texels the same way.
        mBlitShader.SetUniform("uMVP", mvp);
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

    void NeonRenderer::renderRingPass(const glm::mat4 &mvp, const glm::vec2 &gatherUVScale,
                                      const glm::vec2 &gatherUVOffset, float time, const Config &config)
    {
        if (mRingVertexCount == 0)
        {
            return;
        }

        // FULL resolution, on the caller's framebuffer, through the very
        // matrix the blit drew with (@p mvp, built once in Render), so the
        // ring lands exactly in the hole the blit area leaves.

        // Every shading uniform exactly as the DIRECT path uploads it - scale
        // 1.0, so the Nyquist floor is off and the cut and cutoffs are applied
        // in the shader - fading against the direct path's margin rather than
        // the scaled one. The segment and arc blocks are still bound from
        // packLightBlocks; the LUTs are bound by the upload.
        mNeonRingShader.Use();
        uploadNeonUniforms(mNeonRingShader, mvp, 1.0f, time, mRingQuadMargin, mRingCornerSkip, config);

        // What the gather pass produced, in place of running the loop here -
        // the same buffer pass 1 shaded from, through the same region, here
        // mapped from full-res px.
        bindGatherBuffer(mNeonRingShader, gatherUVScale, gatherUVOffset);

        mRingVertexArray.DrawArrays(GL_TRIANGLES, mRingVertexCount);
        mNeonRingShader.Unuse();
    }
}
