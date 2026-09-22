precision highp float;

// Spotlight fragment stage: one lamp's cone plus its aperture bloom, evaluated
// in that lamp's own frame.
//
// THE FALLOFF, in full, because spotlight-renderer.cpp inverts exactly this to
// decide where to stop drawing:
//
//   halfW(a) = apertureWidth + max(a, 0) * tanHalfBeam
//
//   cone = exp(-(c / halfW)^2 * softK)                 // soft across the beam
//        * smoothstep(-apertureWidth,
//                     SPOT_NEAR_FADE * apertureWidth, a)  // fade in at the lamp
//        * exp(-max(a, 0) / throwLength)               // fade along the throw
//        * (apertureWidth / halfW)                     // energy spreads as it widens
//
//   bloom = bloomStrength * r^2 / (a^2 + c^2 + r^2)    // inverse-square core
//         * (1 - smoothstep(S * SPOT_BLOOM_WINDOW_INNER, S, d))   // windowed off
//
// Three of those terms are not obvious:
//
//   The lateral falloff is a GAUSSIAN, not a smoothstep cut. The look this
//   renderer targets has no visible beam edge anywhere, so softness sets how
//   broad the gaussian is and there is deliberately no setting that produces
//   an edge.
//
//   `apertureWidth / halfW` is what stops a wide beam reading as brighter than
//   a narrow one at equal intensity. Drop it and beamAngle becomes a second,
//   non-linear brightness control.
//
//   The bloom's window has no visual job at all - the inverse-square core has
//   no natural end, so without it the term's support is the whole framebuffer
//   and there is no finite strip to draw. See spotlight-tuning.h.
//
// This shader never reads gl_FragCoord: everything arrives interpolated in the
// lamp's frame - or, for the clip area, in app space through vApp. That is why
// the y-flip in Render is free, why SpotlightConfig::resolutionScale still
// needs no uniform of its own, and why the sub-viewport caveat in
// BaseRenderer's doc comment does not apply here.
//
// THE CLIP, and why it multiplies rather than reshaping anything. A lamp with
// SpotLight::clipped set is cut off by a rounded-rectangle area in app
// coordinates (SpotlightConfig::clipArea), on whichever side the mode names. That
// cut is COVERAGE applied to the finished shading - `lit *= mask` - not a term
// folded into the falloff, so a clipped beam is the same beam with part of it
// missing: moving the area cannot make the light that survives brighter, dimmer
// or a different shape. The alpha follows for free, because the coverage below
// is derived from `lit` after the multiply rather than alongside it.
//
// The mask is applied BEFORE the dither, which is the order that matters and
// is safe for exactly the reason the dither comment gives: the offset is
// strictly under half a destination step, so a fragment the clip took to zero
// still rounds to zero and the cut region stays black rather than acquiring a
// speckle. Dithering after the mask also means the clip's own soft edge - one
// more shallow gradient - gets the same treatment as the falloff it crosses.
//
// Output is premultiplied colour plus a COVERAGE ALPHA, the same
// max-of-channels rule neon.frag and lens-flare.frag use. The renderer pairs
// it with a separate-alpha blend (GL_ONE / GL_ONE on both channels), so the
// colour is still pure addition - light only adds, and lamp order still cannot
// change the image - while the alpha channel accumulates a record of where
// light was written.
//
// That alpha is NOT decoration and this shader is the reason the whole layer
// once vanished on a device. Every other layer here writes a coverage alpha;
// this one wrote a literal 0.0, which is invisible on a desktop demo (the
// window is opaque, so nothing ever reads the framebuffer's alpha back) and
// fatal on an embedded surface that a compositor or hardware video plane
// blends: `out = ui.rgb * ui.a + video * (1 - ui.a)` multiplies every lit
// spotlight pixel by zero. Measured offscreen at 640x360 over a transparent
// clear, a single lamp lit 72,615 pixels of colour and exactly 0 pixels of
// alpha, and composited to nothing at all over a background. See
// SpotlightRenderer::Render.

in vec2 vLocal;      ///< (along, across) px in this lamp's frame.
in vec2 vApp;        ///< App px, top-left origin, +y down. Clip area only.
flat in vec4 vP0;    ///< tanHalfBeam, throwLength, softK, intensity.
flat in vec4 vP1;    ///< apertureWidth, bloom, bloomRadius, bloomSupport.
flat in vec3 vColor; ///< Linear RGB.
flat in float vClipWeight; ///< 1 where this lamp honours the clip area, else 0.

/// The clip area, in APP coordinates: centre.xy, half extent.xy. Already
/// collapsed from ClipArea's top-left + size on the CPU, because a
/// rounded-box SDF wants a centred box and the conversion is the same two
/// adds every fragment would otherwise repeat.
uniform vec4 uClipRect;

/// cornerRadius px, edgeSoftness px, and 1 for KEEP_INSIDE / 0 for
/// KEEP_OUTSIDE. The mode is a LERP WEIGHT rather than a branch: the two
/// answers are each other's complement, so mixing between them is one
/// instruction and costs nothing in divergence.
uniform vec3 uClipParams;

out vec4 fragColor;

/// Signed distance to a rounded box centred at the origin, negative inside.
/// @p b is the half extent, @p r the corner radius (already clamped on the CPU
/// to at most the shorter half extent, so the `- r` below cannot invert the
/// box).
float spotClipSDF(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

/// Coverage the clip area leaves at @p app, in [0, 1].
///
/// Independent of vClipWeight on purpose - the caller mixes this against 1.0
/// with that weight, so an unclipped lamp costs the same arithmetic instead of
/// a branch that would diverge inside a draw call covering both kinds of lamp.
float spotClipMask(vec2 app)
{
    float d = spotClipSDF(app - uClipRect.xy, uClipRect.zw, uClipParams.x);
    // Half the feather either side of the boundary, so edgeSoftness is the
    // full width of the fade and 0 collapses to a hard step. The floor keeps
    // smoothstep's two edges apart at edgeSoftness 0, where equal edges are
    // undefined rather than a step on some drivers.
    float h = max(uClipParams.y, 1.0e-4) * 0.5;
    float inside = 1.0 - smoothstep(-h, h, d);
    return mix(1.0 - inside, inside, uClipParams.z);
}

/// Interleaved gradient noise, in [-0.5, 0.5]. One fract, one dot: the whole
/// dither costs about as much as the bloom's sqrt.
///
/// Deliberately NOT the fract(sin(dot(...))) hash. That one feeds sin an
/// argument in the tens of thousands, and how much of it survives is a
/// precision question every GPU answers differently - on a part that rounds
/// it coarsely the "noise" degenerates into a second set of bands, which is
/// the artefact this function exists to remove.
///
/// @p p is in PIXELS, and vLocal is the right pixel-valued thing to hand it:
/// the lamp frame is a rotation plus a translation of the framebuffer, so a
/// step of one fragment is a step of one unit here too, whatever the lamp's
/// angle. That also keeps the promise below - the pattern is anchored to the
/// lamp, so under an animation it travels WITH the beam instead of crawling
/// across it, and gl_FragCoord still never appears in this shader.
///
/// NOT vApp, which is also in pixels but is anchored to the SCREEN: feeding it
/// here would give every lamp the same pattern in the same place, so two
/// overlapping beams would dither in lockstep and correlate exactly where the
/// noise is meant to be independent.
float spotDither(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}

void main() {
    float along = vLocal.x;
    float across = vLocal.y;

    // The same floors the renderer's solve applies, so the two agree about
    // what a degenerate lamp means rather than disagreeing near zero.
    float nearW = max(vP1.x, 1.0);
    float thr = max(vP0.y, 1.0);
    float halfW = nearW + max(along, 0.0) * vP0.x;

    float lat = across / halfW;
    float cone = exp(-lat * lat * vP0.z);
    cone *= smoothstep(-nearW, SPOT_NEAR_FADE * nearW, along);
    cone *= exp(-max(along, 0.0) / thr);
    cone *= nearW / halfW;

    float d2 = along * along + across * across;
    float r2 = vP1.z * vP1.z;
    float sup = max(vP1.w, 1.0);
    float bloom = vP1.y * r2 / (d2 + r2);
    bloom *= 1.0 - smoothstep(sup * SPOT_BLOOM_WINDOW_INNER, sup, sqrt(d2));

    vec3 lit = vColor * vP0.w * (cone + bloom);

    // The cut. mix rather than an `if`, so a draw call carrying both clipped
    // and unclipped lamps shades them at the same cost - vClipWeight is flat,
    // so this is one lerp against a value constant across the triangle.
    lit *= mix(1.0, spotClipMask(vApp), vClipWeight);

    // Coverage = brightest channel, exactly as in neon.frag and
    // lens-flare.frag: a bright aperture core reads as solid to whatever
    // composites this surface, the dim spill stays as good as additive, and an
    // unlit fragment leaves the alpha it found alone (the blend adds, so 0
    // contributes nothing). Read from `lit` AFTER the clip, so a cut fragment
    // records no coverage either - light that was removed must not go on
    // claiming the surface it would have lit.
    float cov = clamp(max(max(lit.r, lit.g), lit.b), 0.0, 1.0);

    // DITHER, and the reason this layer is the one that needs it.
    //
    // Everything above is a smooth function, and every bit of that smoothness
    // is then thrown away by an 8-bit framebuffer. That is normally harmless
    // because a gradient crossing a step in a pixel or two quantises into an
    // edge nobody can see. The outer cone is the opposite case: it is the
    // flattest gradient in this library, crossing one step every 13 to 50 px
    // at default settings, so the rounding lays down a handful of very wide
    // bands - and because the cone's iso-contours are near-straight rays, the
    // band edges are long straight lines. THAT is what reads as a hard-edged
    // strip sitting on top of whatever is behind this layer, and it is not a
    // clipped strip, a seam, or a blend mode: it is the last few 1/255 steps
    // of the falloff, drawn exactly.
    //
    // Half a step of noise, added BEFORE the framebuffer rounds, decorrelates
    // that rounding from the signal: a pixel near a band edge lands on either
    // side of it with a probability that follows the true value, so the edge
    // spreads across the whole width of the band and stops being an edge. The
    // noise itself is under one destination step and invisible.
    //
    // Rectangular +/- half a step, not the triangular +/- one step a
    // convolution textbook would reach for, for one reason that matters here:
    // `fract` returns strictly under 1, so this offset is strictly under half
    // a step, and GL's round-to-nearest therefore cannot take a fragment the
    // falloff left at ZERO up to 1. This pass draws over a strip that is
    // mostly dark - the region behind the lamp, the corners the bloom disc
    // does not fill - and speckle across all of it would be a worse artefact
    // than the banding. Fragments that DO light up where an undithered pass
    // rounded them away are the ones carrying real sub-step signal, which is
    // exactly how the outermost band edge stops being an edge.
    //
    // Same offset on all three channels and on the coverage, so the dither
    // moves the value and never the hue or the premultiplication.
    float dither = spotDither(vLocal) * (SPOT_DITHER_STEPS / 255.0);

    fragColor = vec4(max(lit + dither, 0.0), max(cov + dither, 0.0));
}
