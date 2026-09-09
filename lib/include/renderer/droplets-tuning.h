#ifndef _EDGE_LIGHTING_DROPLETS_TUNING_H_
#define _EDGE_LIGHTING_DROPLETS_TUNING_H_

// ---------------------------------------------------------------------------
// Shared droplet band constants - single source of truth.
//
// Consumed by BOTH:
//   - the droplet shader (droplets.frag), where CMake text-injects this file
//     via @DROPLETS_TUNING@ in shaders.h.in, and
//   - the C++ renderer (droplets-renderer.cpp), which #includes it to size the
//     draw quad.
//
// Only constants that BOTH sides must agree on live here. The droplet field's
// own look constants (CELL_UV, TRAIL_FLAT_SPAN, TRAIL_FLAT_DROPS) stay in
// droplets.frag: the renderer never reasons about them, and a constant with
// one consumer does not need two homes.
//
// Why macros and not const/constexpr: GLSL ES 3.00 has no constexpr and
// rejects the 'f' float-literal suffix, so a single definition that compiles
// as both GLSL and C++ has to be a plain #define. Same reasoning as
// neon-tuning.h.
//
// ASCII only: keep this file free of non-ASCII so every GLSL compiler accepts
// the injected text.
// ---------------------------------------------------------------------------

// clang-format off

/// How far past each band boundary the shader still shades, in band widths.
///
/// The band coordinate runs 0 at the inner boundary to 1 at the outer, and
/// droplets.frag's early bail keeps [-GUARD, 1 + GUARD] rather than [0, 1]:
/// BandFade needs a drop's neighbourhood, not just the drop, to fade a whole
/// drop by where its centre sits.
///
/// This is the OUTER bound on anything the pass can write, which is exactly
/// what the renderer's draw quad has to clear - so the two read it from here.
/// Widen the bail without widening this and the quad clips the band's outer
/// edge into a straight line along all four sides.
#define DROPLET_BAND_GUARD 0.25

// clang-format on

#endif // _EDGE_LIGHTING_DROPLETS_TUNING_H_
