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
//     The continuous gate reads the arc at the fragment's own (geometrically
//     recovered) perimeter position, so its feathers are PIXEL-space spans,
//     not sample-spacing multiples - a sample-scaled tail would be 40+ px at
//     64 samples and wrap most of the 63 px corner arc an arc starting at 0
//     sits right after. HEAD_FEATHER_PX softens the arc end; TAIL_FEATHER_PX
//     antialiases the start edge while staying small enough that it never
//     visibly lights the corner curve before the start. Both are divided by
//     the current perimeter at the call site. ---
#define HEAD_FEATHER_PX           10.0
#define TAIL_FEATHER_PX           2.0

// --- Halo (sharp coloured glow). Kernel uses g * sqrt(g) ~ p = 1.5. The sum
//     is normalised by kg^2 to recover unit-density brightness. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43
#define HALO_SPACING_FLOOR        1.2

// --- Filament tip taper (perpendicular width). Independent of the along-edge
//     brightness gate (HEAD_FEATHER_PX / TAIL_FEATHER_PX): those are tiny (2
//     and 10 px) to keep the tail wrap from lighting the corner curve before
//     the arc start and to keep the brightness fall-off sharp. This knob
//     shrinks the filament's PERPENDICULAR sigma symmetrically at each
//     endpoint, from INSIDE the arc, so the leading and trailing heads
//     visibly narrow to a point instead of being chopped off flat. Applied
//     over the last TIP_TAPER_PX along the perimeter at each end. Divided by
//     the current perimeter at the call site.
//     A big value produces a long ogive/spire tip; small produces a stub.
//     Bounded per-arc by length/2 so short arcs still peak in the middle. ---
#define TIP_TAPER_PX              40.0

// --- Filament sigma floor at the tip. Sigma = max(halfWidth * tipTaper,
//     TIP_SIGMA_FLOOR_PX). Kept small so the tip reaches an actual point
//     rather than a constant-width 1 px nub. Not zero because pow(ad/0, N) is
//     undefined at ad = 0 on some drivers; 0.05 keeps the on-axis pixel lit
//     while making the off-axis Gaussian collapse to sub-pixel width. ---
#define TIP_SIGMA_FLOOR_PX        0.05

// --- Halo/bloom fragment-level arc-cover feathers. The per-sample arcInside
//     gate turns samples off past the arc endpoints but does nothing to stop
//     still-lit samples from bleeding their halo/bloom kernels along a shared
//     edge into the gap - a fragment on the same top edge as the last-lit
//     samples receives their full colinear halo tail, showing as a thin
//     horizontal streak past the arc end. These feathers are consumed by the
//     fragment-level arcCoverContinuous gate on glow/bloom (mirroring what the
//     filament already does), sized wider than the filament feathers so the
//     halo still tapers naturally past the arc endpoint over a short distance,
//     then goes dark. Divided by the current perimeter at the call site. ---
// Sized so the gate transition into the arc gap reads as a smooth halo
// fall-off, not a hard step - closes the visible dark wedge that otherwise
// forms right next to a corner when the arc ends near it.
#define HALO_HEAD_FEATHER_PX      60.0
#define HALO_TAIL_FEATHER_PX      30.0

// --- Bloom (wide background spill) ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_SPACING_FLOOR       4.0
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
