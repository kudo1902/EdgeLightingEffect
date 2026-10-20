#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - every neon shader (neon.frag, neon-gather.frag, neon-emission.frag,
//     neon-glow-cover.frag, neon-glow-cover-fill.frag, neon-blit.frag,
//     neon-field.frag), where CMake text-injects this file via @NEON_TUNING@
//     in shaders.h.in, and
//   - the C++ side (neon-renderer.cpp and the headers and tools that size
//     arrays and quads from it), which #includes it.
//
// The px constants below are written in FULL-RESOLUTION pixels. Where the
// renderer draws at a reduced NeonConfig::resolutionScale, the shader converts
// each one with uResolutionScale at the point of use; the notes on the
// individual constants say which need it and which do not.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a `const float X = 0.9f;` cannot
// be written once and stay valid in both languages. Plain #define is the only
// form that compiles identically as GLSL and as C++ from a single file.
// (For C++-only constants, prefer constexpr - macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
//
// The measurements, calibrations and history behind these values are in
// docs/design/neon-tuning-notes.md, one section per block below. Read a
// block's section there before retuning its value.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is the STATED-width floor: it makes lineWidth 0
//     mean "no line" rather than a bright dot, paired with lineGate, which
//     fades the filament in over the same span. FULL-RES px, converted with
//     uResolutionScale like the other px constants.
//
//     FILAMENT_NYQUIST_* is the SAMPLING floor, a property of the reduced
//     BUFFER, so it is in BUFFER px and NOT converted with uResolutionScale.
//     The profile must still be at MIN_SHARE of its peak SAMPLE_PX out, the
//     neighbouring texel the blit's bilinear filter rebuilds the peak from:
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     evaluated by neon.frag and GetFilamentExtent in neon-renderer.cpp - keep
//     the two in step. GATED to scales below 1.0 in both: at 1.0 the blit copies 1:1 and
//     the ring always shades at 1.0. At the default falloff (N = 2) it is
//     exactly 0.5 buffer px, so retuning either constant moves the default.
//
//     Where it engages the line reads wider at a reduced scale. Intended: a
//     line the buffer cannot sample cannot be rebuilt from it. Do not pair it
//     with an amplitude compensation - FILAMENT_GAIN saturates the core in the
//     tone map, so scaling the amplitude only dims the peak. See
//     docs/design/corner-crease-and-filament-nyquist.md section 2. ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_NYQUIST_SAMPLE_PX 1.0
#define FILAMENT_NYQUIST_MIN_SHARE 0.0625
#define FILAMENT_GAIN             12.0

// --- Continuous-arc filament gate feathers (neon.frag).
//     INWARD FEATHER: the smooth ramp sits INSIDE the arc's own perimeter
//     span, so nothing outside the arc gets lit -> no corner bleed regardless
//     of width, no perpendicular spike, and the profile is a plain smoothstep
//     that reads the same on straight edges and at corners. Trade-off: the
//     visible arc appears inset by these widths (arc lights up at
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX).
//     Values are pixel-space spans, divided by the current perimeter at the
//     call site, and capped per-arc by ARC_FEATHER_MAX_SHARE below. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

// --- Cap on each feather, as a share of the arc's own length. The feathers
//     are a fixed px span on an arc whose length is a fraction of the
//     perimeter, so on a short arc the two ramps would overlap and cut its
//     peak; capped, the peak holds at exactly 1.0 at any length on any rect.
//     Up to 0.5 holds the peak; 0.4 leaves a 0.2 * length plateau, so a short
//     arc reads flat-topped rather than as a spike. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are CLOSED FORM in the fragment's distance, summed
//     over the emitter's pieces (neon.frag's haloSegment / bloomSegment): the
//     four straights, trimmed to their tangent points, and the four quarter
//     arcs, each developed onto its own tangent (arcTangentSegment). For an
//     infinite straight emitter of unit density the two terms are
//
//       HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     and the NORM factors are calibrated against those limits (peaks at
//     ad = 0: 0.86 and 1.005). Every finite piece reduces to them as its ends
//     go to infinity, so the calibration holds for the sum.
//
//     An arc is developed at rate sqrt(rho * min(rho, r)), its weight scaled
//     by r/rate (rho: the fragment's distance from the arc centre), NOT at
//     rate r, which under-counts near the centre of curvature and creases the
//     lines through it. On the tube (rho == r) the two agree.
//
//     The sum lights the rect's INTERIOR to a floor rather than decaying to 0.
//     Intended: HALO_GAIN cannot take it back out without dimming the line.
//     The knobs for a perimeter-hugging glow are insideCutoff and glowSide
//     OUTSIDE.
//
//     The corner pieces are gated on uCornerRadius > 0 (a uniform), but what
//     the block costs is register pressure, which the sharp path pays too:
//     after touching it, re-time a cornerRadius 0 scene as well as a rounded
//     one. See docs/design/corner-crease-and-filament-nyquist.md sections 1.7
//     to 1.9. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The colour blend filters the gradient LUT, which is parameterised by
//     perimeter fraction (sample i reads LUT(i / NEON_MAX_LOOP_SAMPLES)), so a
//     kernel in pixels would cover a different span of the gradient on every
//     geometry. As a fraction the gradient reads the same at any size, and the
//     kernel stays at COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13
//     sample spacings - it must stay at about one or more, or the blend beads.
//
//     Taken from NEON_MAX_LOOP_SAMPLES, not the live sample count, so
//     numSamples does not move the blend; and NOT coupled to glowRadius - the
//     gather is colour only. 0.0088 is 24 px on the stock 800x600 / radius 40
//     rect.
//
//     NOTE: unit-free, unlike the px constants around it. The shader
//     multiplies it by a perimeter that is already in scaled px, so it needs
//     no uResolutionScale correction - applying one would double-apply. ---
#define COLOR_BLEND_PERIM_FRAC    0.0088

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span, compared against a uGlowRadius that arrives
//     already scaled, so the shader multiplies this by uResolutionScale at
//     the point of use. Identity at scale 1.0. ---
#define GLOW_GATE_FADE_PX         2.0

// --- Lower bound on the analytic emission widths. Guards the divides only;
//     anything this small is already multiplied out by GLOW_GATE_FADE_PX. ---
#define EMISSION_MIN_WIDTH        1e-3

// --- Degenerate guard for the corner arc's tangent frame (neon.frag,
//     arcTangentSegment). The frame normalises the fragment's offset from the
//     arc centre, which is zero only for a fragment exactly on the centre:
//     below this the frame falls back to the nearer tangent point, the right
//     clamp there, and `rho` is floored so the development rate never divides
//     by zero. It only keeps a NaN out. Unit-free, in the shader's px. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). Closed form: see the halo note above. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by the C++ vector cap and the
//     std140 SegmentBlock's array bound) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by the C++ vector cap and the std140
//     ArcBlock's array bound). Each arc gates a slice of the perimeter with
//     its own colour + intensity. Overlap resolves winner-take-all (max
//     mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Glow coverage table (neon-glow-cover.frag): for each piece of the
//     emitter and each fragment position round it, the arcs' and the
//     segments' coverage of that piece as the halo and the bloom see it. Laid
//     out in neon-pieces.glsl: four bands of ROWS rows, each holding one
//     straight at its left and one corner at its right, sharing the band's
//     columns in proportion to their lengths (uGlowCoverSplit,
//     NeonRenderer::GetGlowCoverSplit). The straight has OVERHANG columns past
//     each end, the corner CORNER_OVERHANG on each side of its arc and a guard
//     texel at each end of its block, and the columns left over (the layout's
//     `shared`) are split between the two interiors, each keeping at least
//     MIN_INTERIOR. Half of ROWS lie within one halo width of a straight, and
//     a corner's are split evenly inside and outside its arc.
//
//     ROWS and HEIGHT are constants. The WIDTH is a parameter of the layout
//     (NeonRenderer's GlowCoverLayout, the shaders' uGlowCoverLayout), from
//     MIN_WIDTH to MAX_WIDTH in steps of WIDTH_STEP: a thin halo on a large
//     rect needs all of MAX_WIDTH. ---
#define GLOW_COVER_MAX_WIDTH      1024
#define GLOW_COVER_MIN_WIDTH      256
#define GLOW_COVER_WIDTH_STEP     64
#define GLOW_COVER_ROWS           32
#define GLOW_COVER_OVERHANG       64
#define GLOW_COVER_CORNER_OVERHANG 32
#define GLOW_COVER_MIN_INTERIOR   16
#define GLOW_COVER_HEIGHT         (4 * GLOW_COVER_ROWS)

// --- The glow coverage table's width (NeonRenderer's GetGlowCoverWidth):
//     enough interior columns that the longest band - its straight plus its
//     quarter arc, L full-res px - gets one every S px, S growing with the
//     halo width kh (max(glowRadius, EMISSION_MIN_WIDTH), px):
//
//         S    = clamp(PX_PER_COLUMN_PER_KH * kh, MIN_PX_PER_COLUMN, MAX_PX_PER_COLUMN)
//         need = 2 + 2 OVERHANG + 2 CORNER_OVERHANG + ceil(L / S)
//         W    = clamp(need rounded up to WIDTH_STEP, MIN_WIDTH, MAX_WIDTH)
//
//     In full-res px because the edge ring reads the table at full
//     resolution; lengths enter the layout only as ratios, so every scale
//     shares one table. Calibrated against the 1024-wide table to the
//     glow-cover plan's criterion (max 2 levels, 99.9% of lit pixels within
//     1), with margin under the widest spacing that met it. ---
#define GLOW_COVER_PX_PER_COLUMN_PER_KH 0.8
#define GLOW_COVER_MIN_PX_PER_COLUMN    1.6
#define GLOW_COVER_MAX_PX_PER_COLUMN    5.0

// --- The segment table (NeonRenderer's GetGlowCoverSegmentWidth,
//     neon-glow-cover-fill.frag): the segments' two channels of the glow
//     coverage table, baked at a width of their own and copied in. A bell's
//     standard deviation is 0.3536 x its length x the perimeter, far wider
//     than an arc's feather, so it needs far fewer columns than the arcs:
//
//         sigma = 0.3536 x the shortest lit segment's length x perimeter   (full-res px)
//         need  = 2 + 2 SEG_OVERHANG + 2 SEG_CORNER_OVERHANG + ceil(L / (sigma / SEG_COLUMNS_PER_SIGMA))
//         W     = clamp(need rounded up to WIDTH_STEP, SEG_MIN_WIDTH, the main table's width)
//
//     with L the longest band, as for the main table. Where W would be at
//     least SEG_DIRECT_SHARE of the main table's width (very short segments)
//     the separate table saves nothing, and the segments are baked into the
//     main table directly. COLUMNS_PER_SIGMA 4 is one step of margin over the
//     least that met the glow-cover plan's criterion. ---
#define GLOW_COVER_SEG_COLUMNS_PER_SIGMA 4.0

// --- How far the glow coverage bake integrates a segment's bell either side
//     of its centre, in units of 1 / invSigma (the bell is exp(-(d invSigma)^2),
//     so 3 is 4.24 standard deviations, where it has fallen to 1.2e-4 of its
//     peak). The bake (neon-glow-cover.frag, segmentsOnPiece) cuts the bell
//     here and NeonRenderer's GetGlowCoverDirtySegmentPieces mirrors it to
//     find the pieces a moved segment reaches: change one and the other
//     follows, or a piece the bell reaches is not re-baked. ---
#define GLOW_COVER_BELL_REACH            3.0
#define GLOW_COVER_SEG_OVERHANG          16
#define GLOW_COVER_SEG_CORNER_OVERHANG   8
#define GLOW_COVER_SEG_MIN_WIDTH         128
#define GLOW_COVER_SEG_DIRECT_SHARE      0.75

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- The output dither (R7), peak to peak in 8-bit levels: neonDither
//     (neon-grade.glsl) adds noise in +/- half this to every colour the neon
//     writes to the caller's framebuffer - the blit, the edge ring and the
//     ring's field composite, never an offscreen buffer. The halo and bloom
//     are slow gradients that cross a level only every few px, up to tens of
//     px in the dark tail, so an undithered write draws concentric contour
//     rings around the rect. 1.0 is the least that removes them; 0 turns the
//     dither off. It assumes an 8-bit target. ---
#define OUTPUT_DITHER_LSB         1.0

// --- Epsilons ---
#define WSUM_EPSILON              1e-6

// --- One-sided cut guard band, in BUFFER pixels. Pass 1b and its field only.
//
//     In BUFFER px, like BLIT_CUTOFF_GUARD_PX below: it describes the buffer's
//     own sampling, so it must NOT be converted with uResolutionScale.
//
//     For pass 1b neon-blit.frag applies the one-sided cut at destination
//     resolution; neon.frag only culls the dark side, and must cull this far
//     PAST the cut, because the blit rebuilds each destination pixel from the
//     2x2 texels around it - a texel culled at the cut would rebuild the first
//     lit pixel partly from black, a dark seam inside the glow's edge. 2.0 is
//     one texel of filter reach plus room for the diagonal (fwidth of an SDF is
//     sqrt(2) there) and for the phase to the nearest texel centre. None of the
//     band is visible: the blit's mask is zero past the cut.
//
//     NOT used by the edge ring, which owns its own cut (uBlitOwnsCut 0). See
//     neon.frag's sideCull and the post-grade cut block. ---
#define BLIT_SIDE_GUARD_PX        2.0

// --- Cutoff guard band, in BUFFER pixels. Pass 1b and its field only.
//
//     BLIT_SIDE_GUARD_PX's argument, applied to the inside/outside cutoffs:
//     for pass 1b neon-blit.frag applies the cutoff masks at destination
//     resolution and neon.frag only culls, this far past the end of each ramp,
//     so the blit's filter rebuilds every boundary from lit texels. Same value
//     as the side guard, for the same reasons. Do not move the masks back into
//     the buffer: a buffer-resolution mask softens the boundary across a texel
//     and snaps it to the buffer grid. See
//     docs/design/neon-resolution-scale-plan.md, step 2.
//
//     NOT used by the edge ring, whose masks and culls are its own. See
//     neon.frag's cutGuard and NeonRenderer::setupGeometry's cutGuardPx. ---
#define BLIT_CUTOFF_GUARD_PX      2.0

// --- Edge ring width. CPU only.
//
//     At every scale, NeonRenderer redraws a ring around the rect edge at
//     FULL resolution (neon.frag's ring program), reading only the gather's
//     result from the gather buffer. The ring reaches R full-res px either
//     side of the edge (GetRingWidth in neon-renderer.cpp):
//
//         R = max(reach(1.0), reach(scale)) + RING_GUARD_TEXELS / scale
//
//     where reach(s) is the filament's reach as neon.frag computes it at scale
//     s, in full-res px. BOTH, because inside R the reduced pass's filament -
//     floored to what its buffer can sample, so wider than the real one
//     whenever the line is thin - is replaced by the real one, and R has to
//     clear whichever reaches further. Uncapped: the reach is where a filament
//     has fallen to FILAMENT_CUTOFF of its gain, about 2/255 after the grade,
//     which is exactly how far its error can show. RING_GUARD_TEXELS adds the
//     reduced buffer's bilinear footprint past that; 1.0 is the smallest that
//     held 2/255 against the 1.0 render at scales 0.5 and 0.25 over the
//     calibration sweep.
//
//     The gather pass's quad (built in setupRingGeometry) must cover the
//     ring, or the ring reads gather texels nothing wrote. ---
#define RING_GUARD_TEXELS         1.0

// --- Gather resolution. CPU only.
//
//     At every resolutionScale the gather runs in a pass of its own
//     (neon-gather.frag) at its own scale, and pass 1 and the edge ring shade
//     from its result. Its scale is (GetGatherScale in neon-renderer.cpp)
//
//         clamp(GATHER_TEXELS_PER_KERNEL / kc, GATHER_MIN_SCALE,
//               min(resolutionScale, GATHER_MAX_SCALE))
//
//     with kc the colour kernel's width in full-res px, perimeter *
//     COLOR_BLEND_PERIM_FRAC: the gather's outputs are Lorentzian-weighted
//     means whose kernel is never narrower than kc, so this many texels per kc
//     carries them through a bilinear read.
//
//     2.0 is the knee of the calibration: lower saves nothing measurable,
//     higher costs more for no visible gain. With the RGBA8 fallback gather
//     buffer it reads 3/255. GATHER_MIN_SCALE barely matters and is kept as a
//     cheap guard: at the floor the gather buffer is at most 128 x 80 texels
//     at 1080p. See docs/design/neon-resolution-scale-plan.md section 13. ---
#define GATHER_TEXELS_PER_KERNEL  2.0
#define GATHER_MIN_SCALE          0.0625

// --- The gather's ceiling. CPU only.
//
//     GetGatherScale never gathers finer than this, at any resolutionScale. It
//     binds only on a rect whose perimeter is under ~450 px (kc under 4 px),
//     which would otherwise gather near full resolution and size the gather
//     buffer to the whole glow quad's box at that resolution. The check
//     suite's smallest rect (perimeter 478 px) is not touched. ---
#define GATHER_MAX_SCALE          0.5

// --- Stand-in distance for a cutoff that glowSide has already subsumed.
//
//     neon.frag hands this to the band distance on the side the one-sided cut
//     culls, so the discard and the mask there no-op through the arithmetic
//     exactly as a DISABLED cutoff does - same mechanism, same sentinel shape,
//     no extra branch downstream.
//
//     Matches CUTOFF_DISABLED_SIZE in neon-renderer.cpp, which is what a
//     disabled cutoff actually arrives as. It is a distance in the shader's own
//     px space and only has to be far enough that no realistic geometry reaches
//     it; see the note on that constant for the same reasoning. ---
#define CUTOFF_NEUTRALISED        1.0e6

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity),
//     glowRadius alone: the bloom's reach depends on nothing else. What the
//     factor sets is how far the glow may reach before the quad stops
//     covering it - geometry culls the far region, not a discard.
//
//     Used by GetGlowReach in neon-renderer.cpp AND by neon.frag, which
//     recomputes the same expression to place the bloom pedestal that lets
//     this margin stay tight without the truncation showing. Keep the two in
//     step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail never clips as a hard rectangle. A fraction, not a px
//     span, because the margin is proportional to glowRadius and so is the
//     bloom it hides; a fixed span would vanish on a wide glow and dominate a
//     narrow one. Nothing derives 0.8 - any ramp wide enough to hide the clip
//     behaves the same. The bloom itself ends earlier, by its own fade
//     (BLOOM_FADE_START_FRAC, below).
//
//     It assumes the margin was set by the glow. When outsideCutoff clamps the
//     margin instead, 20% of it is a few px and the ramp would land INSIDE the
//     cutoff band, dimming its outer edge ahead of the cutoff mask - so
//     neon.frag floors the ramp's start at the cutoff boundary whenever that
//     boundary falls inside the quad (its fadeStart block). ---
#define QUAD_FADE_START_FRAC      0.8

// --- Where each piece's bloom starts fading out, as a FRACTION of `reach`
//     (neon.frag's bloom end fade, V23).
//
//     The pedestal takes the bloom's 1/a tail to exactly 0 at `reach` with its
//     slope still on, which reads as a crease (a dark rectangle `reach` inside
//     a large rect). So each piece's bloom is multiplied by
//     1 - smoothstep(FRAC * reach, reach, a) of its own distance, on both
//     sides. 0.5 is about the latest start that does not steepen the tail into
//     a shoulder; an earlier one costs more of the tail.
//
//     Keep the fade a factor of position alone and at most 1: the bounds in
//     GetGlowInnerReach, the coverage table's ratios and the glow field's
//     factorisation all rely on it. ---
#define BLOOM_FADE_START_FRAC     0.5

// --- Filament reach for the same quad sizing, expressed in sigmas.
//
//     glowRadius = 0 means "filament only" - the halo and bloom are gated off
//     by GLOW_GATE_FADE_PX - but the filament itself is still there, sized by
//     lineWidth, so the quad has to cover it or the exterior gets clipped.
//
//     The reach is NOT a constant, because the profile is
//     exp2(-(ad/sigma)^N) with N = 2 * filamentFalloff and the slider runs
//     down to falloff 0. Solving core * FILAMENT_GAIN < FILAMENT_CUTOFF gives
//
//         reach = sigma * log2(FILAMENT_GAIN / FILAMENT_CUTOFF) ^ (1/N)
//
//     which diverges as falloff -> 0, so it cannot be a fixed count of sigmas:
//     the quad always covers the interior, and a fixed reach chopped only the
//     exterior of a soft filament.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; neon.frag
//     pedestal-subtracts the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step between neon.frag and GetFilamentExtent in
//     neon-renderer.cpp, which the quad and the edge ring are both sized
//     from. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_
