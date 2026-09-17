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
// lamp's frame. That is why the y-flip in Render is free, and why the
// sub-viewport caveat in BaseRenderer's doc comment does not apply here.
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
flat in vec4 vP0;    ///< tanHalfBeam, throwLength, softK, intensity.
flat in vec4 vP1;    ///< apertureWidth, bloom, bloomRadius, bloomSupport.
flat in vec3 vColor; ///< Linear RGB.

out vec4 fragColor;

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

    // Coverage = brightest channel, exactly as in neon.frag and
    // lens-flare.frag: a bright aperture core reads as solid to whatever
    // composites this surface, the dim spill stays as good as additive, and an
    // unlit fragment leaves the alpha it found alone (the blend adds, so 0
    // contributes nothing).
    fragColor = vec4(lit, clamp(max(max(lit.r, lit.g), lit.b), 0.0, 1.0));
}
