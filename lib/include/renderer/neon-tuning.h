#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the neon shaders (neon.frag, neon-emission.frag), where CMake
//     text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderer (neon-renderer.cpp), which #includes it for the
//     glow-reach quad-sizing factors.
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
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
//
//     FILAMENT_MIN_HALF_WIDTH is a STATED-width floor: it is what makes
//     lineWidth 0 mean "no line" rather than a single bright dot, and it is
//     paired with lineGate, which fades the filament in over the same span. It
//     describes the width the caller asked for, so it is a FULL-RES px constant
//     and the shader converts it with uResolutionScale like every other one.
//
//     The SAMPLING floor is the pair below, and the distinction from the
//     stated floor is the whole point of having two of them. Below
//     resolutionScale 1.0 the gather rasterises into a reduced buffer and
//     neon-blit.frag bilinearly upsamples it, so a filament the buffer cannot
//     sample is not reconstructed - it is resampled, and what comes out
//     depends on where the rect edge happens to fall between buffer texel
//     centres. That is a property of the BUFFER, so these are in BUFFER px and
//     are NOT converted with uResolutionScale.
//
//     Getting that backwards is what made a 1 px line at scale 0.5 disagree
//     with the same line at 1.0. sigma was floored only by the converted
//     stated constant, which at scale 0.5 is 0.25 buffer px, so the line sat
//     under the buffer's Nyquist limit and its peak swung with the rect's
//     sub-pixel position - 217 on a texel centre, 180 half a texel off,
//     against a 1.0 reference that holds 230-241 at every position.
//
//     THE FLOOR IS NOT A FIXED HALF WIDTH. It was one - a flat 0.5 buffer px -
//     and a fixed half width asks the wrong question. What survives the blit
//     is not decided by the profile's width at half maximum; it is decided by
//     how much signal the NEIGHBOURING buffer texel still carries, because
//     that is what the bilinear filter rebuilds the peak from. So the floor
//     says exactly that: the profile must still be at
//     FILAMENT_NYQUIST_MIN_SHARE of its peak, FILAMENT_NYQUIST_SAMPLE_PX out.
//     Inverting the generalized Gaussian for sigma gives
//
//         sigma >= SAMPLE_PX / pow(log2(1 / MIN_SHARE), 1/N)
//
//     which neon.frag and NeonRenderer::setupGeometry both evaluate.
//
//     0.0625 and 1.0 are today's behaviour restated, not a retune: at the
//     DEFAULT falloff (N = 2) the expression is exactly 0.5 buffer px, the
//     constant it replaces, so nothing at or near the default moves.
//
//     The two questions diverge at a SOFT falloff, and that is what the change
//     is for. sigma does not only set the core - it multiplies reachSigmas, so
//     it sets the whole tail. At filamentFalloff 0.27 (N = 0.54) reachSigmas
//     clamps at FILAMENT_REACH_MAX_SIGMAS and the tail is 64 sigmas, so a flat
//     0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and past
//     120 px at 0.25 - to buy 5 levels of peak accuracy (max per-phase error
//     13 -> 8) on a profile 31 px wide that the buffer was sampling perfectly
//     well. The share form asks for 0.082 buffer px there, below what the
//     caller already supplies, so it does not engage.
//
//     Where it DOES still engage it is doing the work it was added for: at the
//     default falloff, without any floor, the peak collapses to 153 against a
//     235 reference, and at filamentFalloff 1.5 to 24. The known cost of the
//     share form is the band around filamentFalloff 0.4 to 0.5, where it stops
//     engaging while the unfloored peak error is still 12 to 18 levels;
//     tightening MIN_SHARE to cover that would stop the default case being
//     bit-identical, so the crossover sits here deliberately. See
//     docs/corner-crease-and-filament-nyquist.md section 2.8.
//
//     The line does read WIDER at a reduced scale wherever the floor engages,
//     and that part is not a defect to tune away: a line the buffer cannot
//     sample cannot be reconstructed from it. The floor buys a stable,
//     correctly-bright line of the narrowest width the buffer can actually
//     carry. Deliberately NOT paired with an amplitude compensation to
//     conserve the line's integral - the grade at the end of neon.frag
//     tonemaps per-fragment INSIDE the buffer and FILAMENT_GAIN puts the core
//     deep in saturation, so scaling the linear amplitude barely moves the
//     8-bit value while it does measurably dim the peak (error 15-23/255
//     against 0-10 without it). See
//     docs/corner-crease-and-filament-nyquist.md section 2.
//
//     GATED to the scaled path in both consumers. At scale 1.0 the gather
//     already runs at the destination rate, there is no blit to survive, and
//     the direct path has to stay bit-identical to the full-res renderer it
//     replaced. The old flat constant was a no-op at 1.0 by arithmetic
//     coincidence - it equalled the converted stated floor; this expression
//     would not be, above N = 2, so the gate is explicit now. ---
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

// --- Cap on each feather, as a share of the arc's own length.
//
//     The feathers above are a fixed pixel span while an arc's length is a
//     FRACTION of the perimeter, so the same arc config is a different pixel
//     length on every rect. Below ~(HEAD + TAIL) px the two ramps overlapped
//     and ate into the arc's peak: an L = 0.02 arc peaked at 1.00 on an
//     800x600 rect but only 0.21 on a 200x150 one - the last place rect size
//     still reached brightness. Capping each feather at a share of the arc
//     length holds the peak at exactly 1.0 for any length at any size; a short
//     arc gets a proportionally shorter ramp instead of a truncated top.
//
//     0.4 leaves a 0.2 * length plateau at full brightness between the two
//     ramps. Up to 0.5 also holds the peak, but with no plateau the arc reads
//     as a spike rather than a flat-topped segment. Arcs longer than
//     (HEAD + TAIL) / 0.4 px never reach the cap, so the normal case is
//     bit-identical to before. ---
#define ARC_FEATHER_MAX_SHARE     0.4

// --- Halo (sharp coloured glow).
//
//     The halo and bloom are evaluated ANALYTICALLY from the rounded-box SDF
//     distance, not summed over the perimeter gather. For a locally straight
//     emitter of unit density the old sums converge exactly to:
//
//       sum g*sqrt(g) * spacing*kh^2*HALO_NORM  -> HALO_NORM  * 2*kh^2/(ad^2 + kh^2)
//       sum 1/(dd+bw^2) * spacing*bw*BLOOM_NORM -> BLOOM_NORM * PI*bw/sqrt(ad^2 + bw^2)
//
//     so the NORM factors keep their meaning and their calibration: peak
//     values at ad = 0 are unchanged (0.86 and 1.005 respectively).
//
//     The closed form is what makes the effect geometry-independent. The
//     gather had to floor its kernel at a multiple of sampleSpacing to stop
//     128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly.
//
//     Those two limits are of an INFINITE emitter, and evaluating one of them
//     at ad = the nearest-edge distance is what used to produce the interior
//     medial-axis creases - the dark wedges running in from each corner, plus
//     the central spine. A fragment on a corner diagonal has two edges equally
//     near and was lit by exactly one of them: measured against a fragment with
//     a single edge at the same distance, the ratio was 1.000 at every distance
//     tested, where physics says roughly 2.
//
//     neon.frag now sums the FINITE-SEGMENT form of the same two integrals over
//     the four straight edges instead (haloSegment / bloomSegment). Integrating
//     the same kernels from t1 to t2 along a segment at perpendicular distance
//     a gives elementary antiderivatives, and as t1 -> -inf, t2 -> +inf each one
//     reduces to exactly the limit above - so this is a strict generalisation
//     and the NORM factors keep the calibration they already had. The corner
//     sweep went from a V kinked at exactly 45 degrees (21 levels deep at
//     r = 160) to a smooth basin of 12, which is genuine falloff rather than a
//     crease. Cost is ~1.06-1.10x of the neon frame; the gather loop is still
//     the overwhelming majority of it.
//
//     The CREASE is what the four-segment form was adopted for. It is not the
//     largest thing it moves. The old expressions were functions of ad and of
//     nothing else, so the old profile was SYMMETRIC about the line - a
//     fragment 120 px inside the edge and one 120 px outside were lit
//     identically, by construction. Outside, the emitter recedes; inside, it
//     wraps around you. The segment sum knows the difference, so the exterior
//     now falls off slightly faster (mid-edge peak itself moves by 2/255) and
//     the INTERIOR stops falling off past roughly one rect-half and settles on
//     a floor. On the 1000x500 / glowRadius 60 probe the centre goes 162 ->
//     186 and the interior mean 174.5 -> 188.8; at glowRadius 30 on 800x400 it
//     is 81 -> 113. The glow reads as bigger, and the part that grew is the
//     interior - the exterior mean goes slightly DOWN.
//
//     Intended, and not tunable back out: a real rectangular tube does light
//     its own interior, and HALO_GAIN cannot be lowered to undo it without
//     dimming the line with it. The knobs for a perimeter-hugging glow are the
//     ones that always meant that - insideCutoff, and glowSide OUTSIDE.
//
//     THE CORNERS ARE A FIFTH THROUGH EIGHTH SEGMENT, not an extension of the
//     four. The straights are trimmed to their TANGENT POINTS, and each quarter
//     arc contributes one more segment: the arc DEVELOPED onto its own tangent
//     at whichever arc point is nearest the fragment, carrying the arc's full
//     length and split about that point, so it abuts the straights exactly in
//     arclength and the emitter is continuous. See neon.frag's
//     arcTangentSegment.
//
//     DEVELOPED AT RATE sqrt(rho * min(rho, r)), NOT AT RATE r, with the
//     measure put back by scaling the segment by r/rate - `rho` being the
//     fragment's distance from the arc centre. Laying an arc out one unit of
//     tangent per unit of arc is only right for a fragment ON it; the exact
//     distance to the point dphi away is a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,
//     whose slope at the foot is sqrt(rho*r). Rate r cost two things:
//
//       - at the CENTRE OF CURVATURE the whole arc ran off to one side of the
//         foot, and both kernels peak at t = 0, so it under-counted - to 54%
//         of the true value on a circle, where all four centres coincide at
//         the middle of the shape;
//       - and it CREASED, because the arc's endpoints slide at the tangent
//         rate on one side of the clamp and at rate 1 on the other, which
//         agree only where the rate is length(w). Every other point of the two
//         lines through an arc centre carried a C1 kink: an L-shaped seam per
//         corner, and on a circle an unmistakable dark cross. See
//         docs/review-findings.md V12.
//
//     rho == r gives rate r and weight 1, so a fragment on the tube is
//     bit-identical to the rate-r form and this calibration is untouched. The
//     whole emitter's worst error against a numerically integrated perimeter
//     falls on every geometry tested (16.5 -> 9.8% on a 600x400 r=40,
//     37.3 -> 26.3% on a circle).
//
//     The straights used to run to the SHARP corner instead, on the grounds
//     that over-extending them past the tangent point stood in for the arc that
//     has no elementary closed form. It does not stand in for it. The
//     over-extension is a phantom emitter a few px from a fragment that is tens
//     of px from the real tube: on an 800x400 rect at cornerRadius 40, a point
//     20 px outside the arc on the diagonal sits 2.4 px from EACH of the two
//     phantoms, and rendered 98 -> 146 across the commit that introduced them.
//     Against a numerically integrated rounded-rect perimeter it was 40 levels
//     too dark ON the tube and 50 too bright just outside it, which is what
//     made a rounded corner read square at a narrow glowRadius.
//
//     The developed arc costs one extra segment per corner - one atan and a
//     length on top - and takes the worst error over a quadrant from 76 to 6
//     levels at glowRadius 5, 112 to 6 at cornerRadius 120, 130 to 8 at 200.
//     A full circle (cornerRadius == halfMin) is still the residual case,
//     since its perimeter is four developed arcs and no straights at all.
//
//     Gated on uCornerRadius > 0, which is a UNIFORM - safe branching, and the
//     reason a sharp rect pays nothing and stays bit-identical (nine scenes
//     cmp-equal, not argued, and still cmp-equal after the rate fix above).
//     Rounded costs 1.19x of the neon pass against no arcs at all, and the rate
//     fix adds 1.03x on top - but the sharp path pays 1.026x of THAT for code
//     it never runs, because what decides this block is REGISTER PRESSURE, not
//     structure: with a per-arc bloom pedestal it was heavy enough to cost the
//     sharp path 1.14x. Re-time a cornerRadius 0 scene as well as a rounded one
//     after touching this. See docs/corner-crease-and-filament-nyquist.md
//     sections 1.7 to 1.9, and docs/review-findings.md V4, V10 and V12. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Width of the COLOUR gather kernel, as a FRACTION OF THE PERIMETER.
//
//     The perimeter colour blend is the one part of the shader that is still a
//     discrete sum over the loop samples, and it is the only place a length has
//     to be expressed this way rather than in pixels. The reason is that the
//     signal it filters - the gradient LUT - is itself parameterised by
//     perimeter fraction: sample i contributes LUT(i / NEON_MAX_LOOP_SAMPLES).
//     A kernel measured in pixels therefore covers a DIFFERENT span of the
//     gradient on every geometry, and the same colour stops render washed out
//     on a small rect and crisp on a large one. Measured on the stock 4-stop
//     ring, the colour sampled exactly on the red stop ran (0.95, 0.33, 0.06)
//     at 200x150 against (0.96, 0.04, 0.00) at 2800x2200 - roughly 8x the hue
//     bleed from geometry alone. Scaling the kernel with the perimeter is what
//     makes the gradient read identically at any size.
//
//     Two further properties fall out of the same choice:
//
//       - A constant anti-bead margin. The kernel has to stay >= about one
//         sample spacing, or it collapses between samples and the blend beads
//         into dots. Spacing is perimeter / NEON_MAX_LOOP_SAMPLES, so a
//         perimeter fraction pins the ratio at
//         COLOR_BLEND_PERIM_FRAC * NEON_MAX_LOOP_SAMPLES = 1.13 spacings on
//         every geometry, where the previous fixed 24 px span met the bound
//         only up to a ~3000 px perimeter and was down to 0.31 spacings by
//         9900 px. (No beading was actually measurable there - a DFT of the
//         hue around the perimeter put the 128-cycle component at the noise
//         floor - because the gradient varies slowly over one spacing on a
//         rect that large. The margin is a guarantee for dense colour stops,
//         not a fix for an observed artifact.)
//
//       - Sample-count independence. The kernel comes from the
//         NEON_MAX_LOOP_SAMPLES-based fraction rather than from the runtime
//         sample count, so the numSamples knob does not move the colour
//         blend. (An earlier sampleSpacing-derived floor divided by the live
//         count and landed 2x wider at reduced counts.)
//
//     Deliberately NOT coupled to glowRadius. The gather produces colour only -
//     the halo and bloom have been closed-form since they stopped riding on it -
//     so a wide glow has no reason to desaturate the gradient, and the old
//     max(glowRadius, floor) reintroduced exactly the pixel-space dependence
//     this constant exists to remove.
//
//     0.0088 puts the kernel at 24.0 px on the stock 800x600 / radius 40
//     geometry, matching the previous fixed span, so default-sized output is
//     unchanged.
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
//     arcTangentSegment). The frame is built by normalising the fragment's
//     offset from the arc centre, clamped into the quarter the arc occupies;
//     that offset is the zero vector only for a fragment sitting exactly on an
//     arc centre, where the direction is undefined and atan(0, 0) is undefined
//     with it. Below this the frame falls back to the nearer of the two tangent
//     points, which is the correct clamp for the whole region the guard covers.
//
//     It floors `rho` in the same function, for the same fragment: the
//     development rate divides into the segment's weight, so an exactly-zero
//     rho would be a division by zero. The floored value is nowhere near a
//     pixel and the limit it lands on is the exact answer anyway, so this only
//     keeps a NaN out.
//
//     Unit-free: compared against a length in the shader's own px space, and
//     small enough that nothing but the exact degeneracy reaches it. ---
#define ARC_FRAME_EPSILON         1e-6

// --- Bloom (wide background spill). See the halo note above for the closed
//     form; BLOOM_SPACING_FLOOR is gone with the gather. ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_NORM_FACTOR         0.32

// --- Travelling-segment array size (shared by C++ vector cap + GLSL uniform array) ---
#define MAX_SEGMENT_BOOSTS        8

// --- Arc range array size (shared by C++ vector cap + GLSL uniform array).
//     Each arc gates a slice of the perimeter with its own colour + intensity.
//     Overlap resolves winner-take-all (max mask*intensity wins). ---
#define MAX_ARCS                  8

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO and
//     the shader's array. The gather iterates only uNumSamples of them
//     (NeonConfig::numSamples, which defaults to this), so it is a ceiling,
//     not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- Epsilons ---
#define SIDE_SOFT_EPSILON         1e-5
#define WSUM_EPSILON              1e-6

// --- Cutoff anti-aliasing floor, in BUFFER pixels.
//
//     The odd one out in this file: every other px constant here is stated in
//     FULL-RES px and converted with uResolutionScale at the point of use.
//     This one is already in the space the gather rasterises into, and must
//     NOT be converted - the whole point is to be a fixed fraction of the
//     buffer's own pixel, whatever that pixel is worth on screen.
//
//     A cutoff with softness 0 is a step function. On the scaled path the
//     gather samples it at buffer-pixel centres and the blit bilinearly
//     upsamples, so the boundary snaps to the buffer grid and reconstructs as
//     a 2-3 px ramp instead of the ~0.8 px one the direct path gives. Half a
//     buffer pixel of feather lets the one sample nearest the boundary carry
//     a fractional value, which the blit can then place sub-texel.
//
//     What it buys, measured on 1280x720 at cutoff 30, softness 0, as the
//     error between the stated cutoff and where the coverage actually ends:
//
//       scale        0.50   0.55   0.60   0.65   0.70   0.75   0.80   0.90
//       without    -0.06  +0.82  -0.08  -0.75  -0.09  +0.16  -0.08  -0.10
//       with       -0.06  +0.43  -0.08  +0.33  -0.09  +0.29  -0.08  -0.10
//
//     Spread 1.57 px -> 0.53 px. Note scale 0.50 does not move, and that is
//     not a defect in this constant: at exactly one half, integer geometry
//     puts the boundary either exactly ON a buffer texel centre or exactly
//     BETWEEN two, and a symmetric feather one texel wide or narrower gives
//     the identical sample pattern in both cases. Widening past 1.0 does not
//     recover it either - it only softens the edge and biases it outward
//     (measured +0.83 at 1.25). The residual +-0.5 px there is information the
//     half-res buffer does not contain; a cutoff that must be pixel-exact
//     wants resolutionScale 1.0, and one that must merely LOOK clean wants a
//     real softness, where both paths already agree to 0.08 px.
//
//     Applied only when uResolutionScale < 1.0 - see neon.frag's softFloor and
//     the matching cap in NeonRenderer::setupGeometry.
#define CUTOFF_SOFT_FLOOR_PX      0.5

// --- Glow reach (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
//
//     Named for the reach, not for an early-out: the per-fragment
//     `ad > earlyOut -> discard` this constant was originally calibrated for
//     no longer exists. Geometry culls the far region instead, which is
//     tiler-friendly, so what the factor sets is how far the glow is allowed to
//     reach before the quad stops covering it. The locals it feeds already say
//     so - `glowReach` in both setupGeometry implementations, `reach` in both
//     shaders.
//
//     glowRadius only: the companion sampleSpacing * SPACING_FACTOR term is
//     gone. sampleSpacing is perimeter / NEON_MAX_LOOP_SAMPLES, so it won on
//     any reasonably large rect at default glowRadius and made both the
//     distance the bloom got truncated at and the brightness it still had
//     there track the rect size. The bloom's reach is a function of glowRadius
//     alone, so that is all that sizes the quad.
//
//     Used by the renderers' setupGeometry AND by the shaders, which recompute
//     the same expression to place the bloom pedestal that lets this margin
//     stay tight without the truncation showing. Keep the two in step. ---
#define GLOW_REACH_RADIUS_FACTOR  48.0

// --- Where the shaders' quad-edge fade begins, as a FRACTION of the quad
//     margin. The emission ramps to zero over [FRAC * margin, margin], so the
//     bloom's 1/D tail - still ~10% of peak out at the quad edge - never clips
//     as a hard rectangle.
//
//     A fraction, not a pixel span, because the margin is proportional to
//     glowRadius (see GLOW_REACH_RADIUS_FACTOR) and so is the bloom profile it
//     hides. A fraction keeps the ramp at a constant proportion of the bloom's
//     reach, so the fade reads the same at every glow radius; a fixed px ramp
//     would vanish on a wide glow and dominate a narrow one. At the stock
//     glowRadius 5 / bloom 0.3 / intensity 1 the margin is 312 px, giving a
//     62 px ramp.
//
//     Nothing derives 0.8 - anything leaving a ramp wide enough to hide the
//     clip behaves the same. What it DOES assume is that the margin was set by
//     the glow, where 20% of it is a long distance. When outsideCutoff clamps
//     the margin instead (to size + softness + 1), 20% of ~13 px is 2.6 px and
//     the ramp lands INSIDE the cutoff band, dimming the band's outer edge
//     ahead of the cutoff mask - and only on the exterior, since the fade keys
//     on positive d. Both shaders therefore floor the ramp's start at the
//     cutoff boundary whenever that boundary falls inside the quad; see the
//     fadeStart block in neon.frag. ---
#define QUAD_FADE_START_FRAC      0.8

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
//     which is 1.9 sigmas at falloff 2.0, 3.5 at 1.0, 12.6 at 0.5, 67.8 at 0.3
//     and 558 at 0.2 - it diverges as falloff -> 0. A fixed 12 (calibrated for
//     falloff 0.5) left the low end badly under-sized: the interior is
//     unbounded because the quad always covers it, so a soft filament flooded
//     inward while the exterior was chopped at 12 sigmas. That asymmetry is
//     the "no outer glow at glowRadius 0" report.
//
//     MAX_SIGMAS bounds the fill cost where the formula diverges; the shaders
//     pedestal-subtract the core at exactly this reach, so hitting the cap
//     shortens the glow symmetrically on BOTH sides instead of showing a cliff
//     on the outside. MIN_SIGMAS keeps a little room at very high falloff.
//     Keep the expression in step across neon-tuning.h, both setupGeometry
//     implementations, and both fragment shaders. ---
#define FILAMENT_CUTOFF           0.002
#define FILAMENT_REACH_MIN_SIGMAS 2.0
#define FILAMENT_REACH_MAX_SIGMAS 64.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_
