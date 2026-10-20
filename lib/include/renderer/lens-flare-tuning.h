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
// shapes a ghost - the 50.0 / 3.0 / 0.05 term magnitudes, the hex's 0.9
// offset and its axes, the procedural palette - stays in lens-flare.frag: the
// renderer never reasons about those, and a constant with one consumer does
// not need two homes. Same split as droplets-tuning.h.
//
// WHY THE RENDERER NEEDS THEM AT ALL
//   circle() evaluates three terms per ghost per fragment, and all three are
//   compactly supported - provably zero outside a disc. The shader skips each
//   where it cannot contribute, testing a squared distance against a squared
//   radius, so a fragment outside all three pays three dot products per ghost
//   and no sqrt, pow, sin or smoothstep. The bounds are pure functions of
//   uGhostSize and the buffer height, so they are solved once on the CPU
//   instead of in every one of the viewport's fragments:
//
//   BLOOM   c > 0  <=>  pow(lq, size * FLARE_BLOOM_EXP) < FLARE_BLOOM_CUT
//                  <=>  lq < FLARE_BLOOM_CUT^(1 / (size * FLARE_BLOOM_EXP))
//           which is the bloom radius (uBloomRadiusSq, squared). Exact, not
//           conservative: pow is strictly increasing in lq for a positive
//           exponent, so the implication runs both ways.
//
//   RING    c1 > 0 <=>  sin(l * 30) > pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP)
//                                     - FLARE_RING_BIAS
//           Two bounds come out of this, one per side.
//
//           The disc. sin is at most 1, so c1 > 0 needs
//             pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) < FLARE_RING_BIAS + 1
//             <=>  l < FLARE_RING_SHIFT + (FLARE_RING_BIAS + 1)^(1 / FLARE_RING_EXP)
//           which is 0.3 + 1.001^40 = 1.34080. With
//           l = lr + size * FLARE_RING_L_BIAS, the term lives where
//             lr < max(1.34080 - size * FLARE_RING_L_BIAS, 0)
//           - the ring radius (uRingRadiusSq, squared): 0.2408 at the default
//           ghostSize 2.2, and nothing at all past ghostSize 2.6816. Only
//           necessary, not sufficient, and it is the cheap test that runs on
//           every fragment. Where l - FLARE_RING_SHIFT is negative (ghostSize
//           below 0.6, I13) lr is inside the disc anyway, so the gate passes
//           that undefined pow through exactly as before.
//
//           The floor. l is never below size * FLARE_RING_L_BIAS and pow is
//           increasing, so the right-hand side is never below
//             pow(size * FLARE_RING_L_BIAS - FLARE_RING_SHIFT, FLARE_RING_EXP)
//             - FLARE_RING_BIAS
//           which is uRingFloor. Inside the disc, testing the (much cheaper)
//           sin against that first errs only in the safe direction: it can
//           run the term where the term turns out to be zero, never skip one
//           that is not.
//
//   HEX     The sprite's argument is q = FLARE_HEX_SCALE * (p + sunUV * dist)
//           + 0.9, and its coverage is 1 - smoothstep(A - w, A + w, d), with
//           A = FLARE_HEX_APOTHEM and d the hexagon's support distance: |q|
//           projected onto the nearest of its six edge normals. That is zero
//           wherever d >= A + w. The normals are 60 degrees apart, so the
//           nearest is never more than 30 degrees from q and d >= |q| cos 30;
//           the sprite therefore lives where
//             |q| < (A + w_max) / cos 30
//           - the hex radius (uHexRadiusSq, squared): the hexagon's
//           circumradius, widened by the edge.
//
//           w is the edge's half width, which used to be fwidth(d) and is now
//           its closed form, because a derivative inside the gate above would
//           be taken in non-uniform control flow - undefined in GLSL. p steps
//           1 / uResolution.y per pixel in BOTH x and y (the shader multiplies
//           uv.x by the aspect), so q steps uHexPx = FLARE_HEX_SCALE /
//           uResolution.y, and on the winning normal n
//             fwidth(d) = |dFdx(d)| + |dFdy(d)| = uHexPx * (|n.x| + |n.y|)
//           which is the closed form, floored at FLARE_HEX_EDGE_FLOOR as
//           fwidth was. |n.x| + |n.y| <= sqrt(2) for a unit n, so
//             w_max = max(uHexPx * FLARE_HEX_SPAN_MAX, FLARE_HEX_EDGE_FLOOR)
//           The closed form differs from the hardware's fwidth only on a 2x2
//           quad that straddles a change of winning normal (a hex vertex):
//           there the finite difference mixes two normals' slopes. One level,
//           on a handful of pixels per frame.
//
//   Change a constant here without rebuilding the matching derivation in
//   lens-flare-renderer.cpp and the gate starts clipping ghost pixels that
//   should be lit - the flare quietly loses ring arcs, bloom edges or hex
//   corners, with nothing in the log to say so.
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

/// Hex sprite: the scale from the ghost's position to the sprite's argument
/// `q`, and the apothem - the inradius, where the edge's smoothstep is
/// centred. The scale also sets how far `q` steps per pixel (uHexPx).
#define FLARE_HEX_SCALE      5.0
#define FLARE_HEX_APOTHEM    0.5

/// Floor under the hex edge's half width, so the smoothstep never collapses to
/// a step on a buffer tall enough to make uHexPx vanish.
#define FLARE_HEX_EDGE_FLOOR 1.0e-4

/// Upper bound on |n.x| + |n.y| for the hex's unit edge normals, which is what
/// the closed-form edge width multiplies uHexPx by: sqrt(2). The three normals
/// in use reach 1.4115 at most, so this is a hair loose, and holds for any
/// aperture roll.
#define FLARE_HEX_SPAN_MAX   1.41421356

/// Number of ghost groups scattered along the sun axis. Sizes the shader's
/// GhostBlock array AND the renderer's GhostBlockData that fills it, so the
/// two must agree or the upload disagrees with the std140 layout the shader
/// was compiled against.
#define FLARE_GHOST_COUNT   10

// clang-format on

#endif // _EDGE_LIGHTING_LENS_FLARE_TUNING_H_
