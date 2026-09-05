#ifndef _EDGE_LIGHTING_LENS_FLARE_TUNING_H_
#define _EDGE_LIGHTING_LENS_FLARE_TUNING_H_

// ---------------------------------------------------------------------------
// Shared ghost-term constants - single source of truth.
//
// Consumed by BOTH:
//   - the flare shader (lens-flare.frag), where CMake text-injects this file
//     via @LENS_FLARE_TUNING@ in shaders.h.in, and
//   - the C++ renderer (lens-flare-renderer.cpp), which #includes it to derive
//     the two support bounds it uploads as uniforms.
//
// Only the constants both sides must agree on live here. Everything else that
// shapes a ghost - the 50.0 / 3.0 / 0.05 term magnitudes, the 5.0 hex scale,
// the procedural palette - stays in lens-flare.frag: the renderer never
// reasons about those, and a constant with one consumer does not need two
// homes. Same split as droplets-tuning.h.
//
// WHY THE RENDERER NEEDS THEM AT ALL
//   circle() evaluates three terms per ghost per fragment, and two of them are
//   compactly supported - provably zero outside a radius. The shader skips
//   each where it cannot contribute, but the bounds are pure functions of
//   uGhostSize, so they are solved once on the CPU instead of in every one of
//   the viewport's fragments:
//
//   BLOOM   c > 0  <=>  pow(lq, size * FLARE_BLOOM_EXP) < FLARE_BLOOM_CUT
//                  <=>  lq < FLARE_BLOOM_CUT^(1 / (size * FLARE_BLOOM_EXP))
//           which is uBloomRadius. Exact, not conservative: pow is strictly
//           increasing in lq for a positive exponent, so the implication runs
//           both ways.
//
//   RING    c1 > 0 <=>  sin(l * 30) > pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP)
//                                     - FLARE_RING_BIAS
//           l = length(...) + size * FLARE_RING_L_BIAS, so l is never below
//           size * FLARE_RING_L_BIAS, and pow is increasing, so the whole
//           right-hand side is never below
//             pow(size * FLARE_RING_L_BIAS - FLARE_RING_SHIFT, FLARE_RING_EXP)
//             - FLARE_RING_BIAS
//           which is uRingFloor. Testing the (much cheaper) sin against that
//           first errs only in the safe direction: it can run the term where
//           the term turns out to be zero, never skip one that is not.
//
//   Change a constant here without rebuilding the matching derivation in
//   lens-flare-renderer.cpp and the gate starts clipping ghost pixels that
//   should be lit - the flare quietly loses ring arcs or bloom edges, with
//   nothing in the log to say so.
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

/// Bloom term: the constant it is subtracted from, and the per-ghost falloff
/// exponent's scale. `max(FLARE_BLOOM_CUT - pow(lq, size * FLARE_BLOOM_EXP), 0)`.
#define FLARE_BLOOM_CUT     0.01
#define FLARE_BLOOM_EXP     1.4

/// Ring term: the bias it is added to, the shift inside its pow, and that
/// pow's exponent.
/// `max(FLARE_RING_BIAS - pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) + sin(l * 30), 0)`.
#define FLARE_RING_BIAS     0.001
#define FLARE_RING_SHIFT    0.3
#define FLARE_RING_EXP      (1.0 / 40.0)

/// The share of ghostSize that biases the ring term's radius:
/// `l = length(...) + size * FLARE_RING_L_BIAS`. This is what puts a floor
/// under l, and so under the pow above, which is the whole basis of the ring
/// gate - the renderer cannot derive uRingFloor without it.
#define FLARE_RING_L_BIAS   0.5

/// Number of ghost groups scattered along the sun axis. Sizes the shader's
/// GhostBlock array AND the renderer's GhostBlockData that fills it, so the
/// two must agree or the upload disagrees with the std140 layout the shader
/// was compiled against.
#define FLARE_GHOST_COUNT   10

// clang-format on

#endif // _EDGE_LIGHTING_LENS_FLARE_TUNING_H_
