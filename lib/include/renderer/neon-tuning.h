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
//     start + TAIL_FEATHER_PX and ends at start + length - HEAD_FEATHER_PX);
//     the inset is imperceptible on typical arcs and only matters if the arc
//     is shorter than about (HEAD + TAIL) pixels. Values are pixel-space
//     spans, divided by the current perimeter at the call site. ---
#define HEAD_FEATHER_PX           14.0
#define TAIL_FEATHER_PX           14.0

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
//     gather had to floor its kernel at HALO_SPACING_FLOOR * sampleSpacing to
//     stop 128 discrete samples beading into dots, and sampleSpacing is
//     perimeter / NEON_MAX_LOOP_SAMPLES - so halo width, and via the gate its
//     brightness, both tracked the rect size, and glowRadius did nothing at
//     all until it exceeded the floor (~56 px on a 1920x1080 rect, i.e. most
//     of its usable range). An analytic profile cannot bead at any radius, so
//     no floor is needed and glowRadius sets the width directly. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43

// --- Anti-bead floor for the COLOUR gather kernel only. The perimeter colour
//     blend is still a 128-sample sum, so its weight keeps the floor. It
//     appears in both the numerator and the denominator of acc/wsum, so it
//     sets how far colours blend along the perimeter and can no longer affect
//     brightness or reach. ---
#define HALO_SPACING_FLOOR        1.2

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

// --- Far early-out (CPU-side quad sizing). The draw quad is sized to
//     rect + max(glowRadius * RADIUS_FACTOR, sampleSpacing * SPACING_FACTOR),
//     which must stay >= a few * the bloom reach so the quad never clips
//     visible glow. Used only by the renderers' setupGeometry now (the
//     shaders no longer discard). ---
#define EARLY_OUT_RADIUS_FACTOR   48.0
#define EARLY_OUT_SPACING_FACTOR  32.0

#endif // _EDGE_LIGHTING_NEON_TUNING_H_
