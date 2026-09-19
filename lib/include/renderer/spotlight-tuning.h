#ifndef _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_
#define _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_

// ---------------------------------------------------------------------------
// Shared spotlight constants - single source of truth.
//
// Consumed by BOTH:
//   - the spotlight shader (spotlight.frag), where CMake text-injects this
//     file via @SPOTLIGHT_TUNING@ in shaders.h.in, and
//   - the C++ renderer (spotlight-renderer.cpp), which #includes it to solve
//     each lamp's support and size the strip it draws.
//
// WHICH SIDE READS WHAT
//   Unlike droplets-tuning.h and lens-flare-tuning.h, the two sides here do
//   not read the same set. The split is deliberate and worth knowing:
//
//     SPOT_NEAR_FADE, SPOT_BLOOM_WINDOW_INNER   shader only - they shape terms
//                                               the CPU only has to bound, and
//                                               it bounds them conservatively.
//     SPOT_DITHER_STEPS                         shader only - it perturbs the
//                                               output by less than one
//                                               destination step, which is
//                                               below everything the CPU
//                                               bounds.
//     SPOT_BLOOM_SUPPORT                        CPU only - it fixes where the
//                                               bloom window ends, and that
//                                               number reaches the shader as a
//                                               per-vertex attribute rather
//                                               than as this macro.
//     SPOT_SOFT_MIN / SPOT_SOFT_MAX             CPU only - the renderer maps
//                                               softness to the gaussian
//                                               exponent and ships the result
//                                               as an attribute.
//     SPOT_MAX_LIGHTS, SPOT_STRIP_SEGMENTS,
//     SPOT_VISIBILITY_FLOOR                     CPU only - geometry sizing.
//
//   They all live here anyway because they describe ONE falloff, and a
//   maintainer changing the shape needs to see the whole shape in one place.
//
// THE INVARIANT THIS HEADER CANNOT ENFORCE
//   The renderer solves, in closed form, where the shader's own falloff drops
//   below one 8-bit step, and draws a strip that stops there. That solve
//   inverts the WHOLE expression spotlight.frag evaluates - not just these
//   constants. Change the falloff's SHAPE in the shader (swap the gaussian,
//   drop the 1/halfW spread term, add a factor) without redoing the solve in
//   SolveConeAcross, and the strip starts clipping lit pixels: the cone gets a
//   straight edge where the geometry ends, with nothing in the log to say so.
//
//   The guard for that is not this file - it is the offscreen diff described
//   in docs/spotlight-renderer-plan.md, which renders the same rig through the
//   solved strip and through an oversized quad and requires the two to be
//   byte-identical.
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

/// Aperture widths over which the beam fades in at the lamp itself.
/// `smoothstep(-apertureWidth, SPOT_NEAR_FADE * apertureWidth, along)` - what
/// stops the cone from painting backwards out of the lamp.
///
/// The CPU does not read this: it starts the strip at
/// -max(2 * apertureWidth, bloomBound), which covers this fade for any value
/// at or below 2.0. Raise it past 2.0 and that bound stops being conservative.
#define SPOT_NEAR_FADE            1.6

/// Where the aperture bloom's window begins, as a fraction of its end.
/// `1 - smoothstep(S * SPOT_BLOOM_WINDOW_INNER, S, d)` with S the per-lamp
/// support the renderer uploads. Shader only - the CPU bounds the bloom by S
/// itself, which is where the window reaches zero whatever this is.
#define SPOT_BLOOM_WINDOW_INNER   0.55

/// Bloom radii at which that window closes, i.e. S = bloomRadius * this.
///
/// The bloom is inverse-square and so has NO natural end - without a window
/// its support is the whole framebuffer and there is no strip to draw. Same
/// problem, same fix, as GetGhostBloomRadius in the lens flare. Renderer only:
/// it multiplies this out and ships S per vertex.
#define SPOT_BLOOM_SUPPORT        8.0

/// Gaussian exponent across the beam at softness 0 and softness 1:
/// `exp(-(across / halfWidth)^2 * softK)`, softK lerped between these.
/// Renderer only - it lerps and ships softK per vertex.
///
/// Note there is no setting that produces a hard beam edge, by design: the
/// look this renderer targets has no visible cone boundary anywhere, so the
/// control is how broad the gaussian is, never whether it is one.
#define SPOT_SOFT_MAX             3.40
#define SPOT_SOFT_MIN             0.85

/// Ceiling on lamps drawn in one pass. Sizes the VBO allocation ONLY - it is
/// not an array bound in any shader, because per-lamp data rides on vertex
/// attributes rather than in a uniform block. Entries past it are ignored.
///
/// Raising it costs one recompile and a larger (still small) buffer:
/// SPOT_MAX_LIGHTS * SPOT_STRIP_SEGMENTS * 6 * 60 bytes.
#define SPOT_MAX_LIGHTS           8

/// Quads per lamp along the beam. The strip approximates a curved support with
/// this many straight chords; each sample is widened to cover its neighbours'
/// midpoints so a chord can only ever bulge outward, never cut inside.
///
/// More segments means a tighter fit and more vertices, and nothing else - the
/// drawn image is identical at any value, because everything the strip adds or
/// removes is a region where the shader writes zero.
#define SPOT_STRIP_SEGMENTS       12

/// Width of the ordered dither spotlight.frag adds before the framebuffer
/// quantises its output, peak to peak, in destination steps. 1.0 is therefore
/// a rectangular +/- HALF a step - the classic dither for an 8-bit target: it
/// decorrelates the rounding error from the signal without adding visible
/// noise, and its peak is exactly the rounding threshold, so it cannot round
/// a black fragment up to 1.
///
/// WHY THIS LAYER NEEDS IT AND THE OTHERS MOSTLY DO NOT. A spotlight's outer
/// falloff is the flattest gradient in this library: at default settings the
/// cone crosses one 8-bit step every 13 to 50 px, so RGBA8 turns it into a
/// handful of wide bands with long, almost perfectly straight edges - the
/// cone's iso-contours are near-straight rays, so the quantisation contours
/// are too. Straight edges are exactly what the eye finds, which is why the
/// banding reads as a hard-edged "strip" laid over whatever is behind it
/// rather than as a smooth pool of light. Neon and the flare quantise the
/// same way but their gradients are steep enough that a band is a pixel or
/// two wide.
///
/// Shader only: the strip solve does not read it. See SPOT_VISIBILITY_FLOOR
/// for what the dither costs that solve.
///
/// 0.0 disables it and restores the exact pre-dither output.
#define SPOT_DITHER_STEPS         1.0

/// HALF an 8-bit step - the level below which a value quantises to zero,
/// and therefore the budget the strip bound is solved against.
///
/// Half, not whole, because GL rounds float-to-unorm to NEAREST: a
/// contribution of 0.9/255 still lands on 1, so cutting at a full step clips
/// pixels that were about to be lit. That is not a theoretical worry - cutting
/// at 1/255 differed from an unbounded reference by exactly 1 LSB across tens
/// of thousands of pixels in every scene of the offscreen diff.
///
/// The renderer divides this by the number of ENABLED lamps before solving, so
/// the whole rig's clipped remainder stays inside one half-step however many
/// lamps overlap: N lamps each under floor/N sum to under floor. A single-lamp
/// rig therefore pays nothing for the sharing, and an eight-lamp rig draws the
/// larger strips it actually needs.
///
/// SPOT_DITHER_STEPS softens what this bound means, and improves what it is
/// worth. A dithered fragment carries up to half a step of noise, so a pixel
/// just inside the strip whose signal this solve cut can round to 1 where an
/// undithered one could not. That does not make the boundary MORE visible: the
/// pixels that light up are a sparse scatter rather than a line, which is the
/// same reason the dither is here. The contour the boundary used to hide
/// behind - the last 1/255 band edge - was the visible artefact, and it is the
/// one the dither removes.
#define SPOT_VISIBILITY_FLOOR     (0.5 / 255.0)

// clang-format on

#endif // _EDGE_LIGHTING_SPOTLIGHT_TUNING_H_
