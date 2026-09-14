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

/// The droplet field's repeat period, in the shader's own `t` units, and the
/// matching vertical cell count.
///
/// The field was the one place in the tree that used time as an unbounded
/// SCROLL rather than as a phase: `uv.y += t * 0.75` feeds `floor(uv * grid)`
/// into a per-cell hash, so `t` grew forever and the float carrying it stopped
/// resolving a frame of drop motion at roughly 39 hours of uptime - the rain
/// slowed and then stood still. Every other shader could simply have whole
/// turns removed (see TimeUtils::WrapHueTime); this one had no period to
/// remove them at, so it was given one.
///
/// The two numbers are locked together and neither is free:
///   - the CPU reduces `t` modulo PERIOD before it becomes a float uniform, so
///     the value stays small and exact however long the process has run;
///   - the hash reduces `id.y` modulo CYCLE_CELLS, so the cells either side of
///     a wrap carry the SAME drops.
/// CYCLE_CELLS must therefore be exactly the number of cells the scroll covers
/// in one period: PERIOD * 0.75 (the scroll rate) * 2 (grid.y) = 1.5 * PERIOD.
/// Change one without the other and the rain visibly reshuffles every period.
///
/// PERIOD must also be a whole number, because `fract(t + n.z)` sets each
/// drop's fall phase in both layers and only stays continuous across the wrap
/// if `t` wraps on an integer.
///
/// 512 buys about 42 minutes at the default speed before the field repeats -
/// imperceptible for rain - while keeping `t * 0.75` under 384, where a float
/// still resolves a slow drop's per-frame motion with room to spare.
#define DROPLET_PHASE_PERIOD 512.0
#define DROPLET_CYCLE_CELLS  768.0

// clang-format on

#endif // _EDGE_LIGHTING_DROPLETS_TUNING_H_
