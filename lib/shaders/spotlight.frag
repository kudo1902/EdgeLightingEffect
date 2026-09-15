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
// Output is premultiplied with alpha 0, so under the house
// GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend it is pure addition. Light only adds.

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

    fragColor = vec4(vColor * vP0.w * (cone + bloom), 0.0);
}
