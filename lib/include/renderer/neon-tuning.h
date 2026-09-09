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
#define FILAMENT_MIN_HALF_WIDTH   0.5
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
//     no floor is needed and glowRadius sets the width directly. ---
//     KNOWN LIMITATION - interior medial-axis creases. Both terms are closed
//     forms of ad = abs(SDF distance). Inside the shape the rounded-box SDF's
//     GRADIENT is discontinuous along the medial axis (the diagonals running in
//     from each corner, plus the central spine), so halo and bloom inherit a C1
//     crease there and the interior glow reads as a mitred picture frame. The
//     gather this replaced summed over perimeter samples and was smooth; a
//     nearest-distance profile cannot be. Subtle at the default glowRadius 5,
//     unmistakable at 30 and above.
//
//     Accepted, not overlooked: the trade bought geometry-independent glow
//     width, no beading at any radius, and no sample-spacing floor, which is
//     the whole reason the analytic form exists. Softening ad near the axis
//     would need a second distance field, and blending the two would put the
//     rect-size dependence straight back. See docs/review-findings.md V4. ---
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
