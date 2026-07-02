#ifndef _EDGE_LIGHTING_NEON_TUNING_H_
#define _EDGE_LIGHTING_NEON_TUNING_H_

// ---------------------------------------------------------------------------
// Shared neon tuning constants — single source of truth.
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
// (For C++-only constants, prefer constexpr — macros are used here strictly
// because these values must also live inside the shaders.)
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off
// --- Filament (the sharp bright line) ---
#define FILAMENT_MIN_HALF_WIDTH   0.5
#define FILAMENT_FALLOFF          2.0
#define FILAMENT_GAIN             12.0
// Constant-width soft edge of the flat-top filament, in pixels. Independent of
// lineWidth so the line's soft edge does NOT grow as the line thickens — all
// spreading glow comes from glowRadius instead.
#define FILAMENT_EDGE_SOFTNESS    1.5

// --- Halo (sharp coloured glow). Kernel uses g * sqrt(g) ~ p = 1.5. The sum
//     is normalised by kg^2 to recover unit-density brightness. ---
#define HALO_GAIN                 0.90
#define HALO_NORM_FACTOR          0.43
#define HALO_SPACING_FLOOR        1.2

// --- Bloom (wide background spill) ---
#define BLOOM_REACH_TO_GLOW       6.0
#define BLOOM_SPACING_FLOOR       4.0
#define BLOOM_NORM_FACTOR         0.32

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
