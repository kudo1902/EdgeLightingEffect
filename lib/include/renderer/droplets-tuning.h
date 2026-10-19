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

/// The droplet field's time, uDropTime, is clock time * DROPLET_TIME_SCALE *
/// DropletsConfig::speed wrapped into [0, DROPLET_TIME_PERIOD) - on the CPU,
/// in double (droplets-renderer.cpp). Unwrapped it grew without bound, and a
/// float time loses the frame step after enough uptime (the effect may run
/// for days); wrapped, it never exceeds what the first ~2.8 h at speed 1
/// already reached.
///
/// The field is not periodic in time on its own: the trickling layers scroll
/// their cell grid down by `t * 0.75` uv, two rows per uv (DropLayer's
/// `grid.y`), and every cell hashes its row, so a wrap would re-roll every
/// drop at once. So the period is chosen to scroll by a WHOLE number of rows
/// in both layers - 0.75 * 2 * DROPLET_TIME_PERIOD rows, in each layer's own
/// cells, since the fine layer applies the scroll after its 1.85x scale - and
/// the shader hashes the row modulo exactly that count. Then the wrap moves
/// every cell onto one with the same hash and the same sub-cell offset, and
/// every `fract(t + n.z)` cycle, the period being a whole number, lines up:
/// the seam is invisible. The modulo is an identity until a row on screen
/// reaches DROPLET_HASH_ROWS - late in the first period - so before that the
/// field is the unwrapped one.
///
/// The hashed rows repeat on screen only if one layer shows more than
/// DROPLET_HASH_ROWS rows at once: the fine layer has 2 * 1.85 rows per uv
/// and a uv is CELL_UV (12) cell widths of at least a pixel each, so a 4320 px
/// tall viewport shows at most ~1330. Change either constant only together.
#define DROPLET_TIME_SCALE 0.2
#define DROPLET_TIME_PERIOD 2048.0
#define DROPLET_HASH_ROWS 3072.0 // 0.75 * 2 * DROPLET_TIME_PERIOD

// clang-format on

#endif // _EDGE_LIGHTING_DROPLETS_TUNING_H_
