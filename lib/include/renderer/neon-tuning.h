#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants - single source of truth.
//
// Consumed by BOTH:
//   - the single-pass neon shaders (neon.frag, neon-optimized.frag), where
//     CMake text-injects this file via @NEON_TUNING@ in shaders.h.in, and
//   - the C++ renderers (neon-renderer.cpp, neon-optimized-renderer.cpp),
//     which #include it for the early-out quad-sizing factors.
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

// --- Continuous-arc filament gate feathers (neon.frag / neon-optimized.frag).
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
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Anti-bead floor for the COLOUR gather kernel only. The perimeter colour
//     blend is still a discrete sum over the loop samples, so its weight keeps
//     a floor; without one the kernel collapses between samples at small
//     glowRadius and the blend beads.
//
//     A FIXED pixel span, deliberately NOT sampleSpacing-derived. It appears in
//     both the numerator and the denominator of acc/wsum, so it cannot change
//     the colour's magnitude inside a uniformly lit stretch - but it does set
//     the distance over which `col` decays to black approaching an unlit one,
//     and `col` multiplies the filament, halo and bloom alike. Tied to
//     sampleSpacing that roll-off scaled with the rect (~25 px at 800x600,
//     ~56 px at 1920x1080) and, in NeonOptimizedRenderer, with the numSamples
//     slider too - so the two renderers disagreed by 2x at their defaults.
//     The value matches the old 800x600 default floor to within a few percent,
//     so stock geometry looks unchanged.
//
//     Must stay >= about one sample spacing at the smallest geometry in use,
//     or the gather beads. At NEON_MAX_LOOP_SAMPLES = 128 that holds for any
//     perimeter up to ~3000 px; past that the samples are sparser than this
//     floor and the blend leans on glowRadius to stay smooth.
//
//     NOTE: full-res pixel span. neon-optimized.frag compares it against FBO-px
//     quantities, so its copy multiplies by uResolutionScale - keep the two in
//     step when tuning. ---
#define COLOR_BLEND_MIN_PX        24.0

// --- Emission on/off ramp. glowRadius = 0 must read as "filament only", but
//     an analytic profile at radius 0 is a sub-pixel spike of full height
//     rather than nothing, so the halo and bloom fade in over
//     glowRadius = [0, this]. A FIXED pixel width: gating against the
//     sampleSpacing-derived floor instead would re-couple brightness to the
//     rect size, which is the whole thing this design removes.
//
//     NOTE: full-res pixel span. neon-optimized.frag compares it against a
//     glowRadius already scaled into FBO px, so its copy multiplies by
//     uResolutionScale - keep the two in step when tuning. ---
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

// --- Perimeter gather-loop upper bound. Sizes the LoopSamplesBlock UBO in
//     both shaders. NeonRenderer runs the full loop at compile-time-fixed
//     count; NeonOptimizedRenderer's shader iterates only uNumSamples of them
//     (its numSamples slider), so this is a ceiling, not a fixed cost. ---
#define NEON_MAX_LOOP_SAMPLES     128

// --- Grading ---
#define TONE_MAP_SHOULDER         0.6
#define GAMMA_EXPONENT            0.85

// --- Epsilons ---
#define SIDE_SOFT_EPSILON         1e-5
#define WSUM_EPSILON              1e-6

// --- Far early-out (quad sizing). The draw quad is sized to
//     rect + glowRadius * RADIUS_FACTOR * (1 + bloomStrength * intensity).
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
#define EARLY_OUT_RADIUS_FACTOR   48.0

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
