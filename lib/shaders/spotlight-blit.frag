precision highp float;

// SpotlightRenderer's composite of its reduced-resolution buffer back over the
// caller's framebuffer, below resolutionScale 1.0. Drawn with neon.vert over an
// NDC quad at an identity uMVP, so vPos is NDC. The renderer's screen blend
// (`dst + src * (1 - dst)`, colour and alpha) is still in force.
//
// Its own program rather than blit.frag, because it does one thing more: it is
// where the scaled path DITHERS. spotlight.frag dithers its own output at 1.0,
// but on this path it writes a buffer the bilinear read below filters, and a
// filtered read averages a per-texel dither into blotches - while an RGBA8
// buffer has already rounded the dark tail into one-level plateaus that no
// later noise can recover. So the buffer is half float and undithered, and the
// noise goes on here, once per destination pixel, after the filter: the same
// split the neon makes between mScaledBuffer and neon-blit.frag (R7). Measured
// in SpotlightRenderer.cpp beside SCALED_FORMATS.
//
// The same offset on colour and coverage, as spotlight.frag adds it - the
// layer's alpha screens exactly as its colour does, so the two take the same
// noise and the premultiplication is unchanged.
//
// On a driver that will not render to half float the buffer falls back to
// RGBA8 and spotlight.frag dithers into it as well (uDither 1); this pass
// still dithers on top, which joins the plateaus' rounding to the target's.
//
// TWO THINGS HERE ARE FOR SPEED, and both were measured, because this pass
// covers the whole viewport and is the fixed cost that decides whether the
// scale pays at all (AMD Radeon Pro 5300M, 1920 x 1080, one lamp at 0.25,
// against the undithered blit):
//
//   - the pixel position comes from vPos and uViewportSize, NOT gl_FragCoord.
//     Reading gl_FragCoord in this pass alone cost +0.05 ms (1.29x the frame);
//     the same noise from the interpolated position costs 1.06x. Same pixel
//     centres to well under the noise's own precision.
//   - an EMPTY texel writes an exact 0, not max(0 + noise, 0). The result is
//     the same - under half a level screened onto anything rounds back to it -
//     but a zero source is a write the blend can skip, and most of the
//     viewport is empty.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

/// The caller's viewport in pixels - what the blit's NDC quad covers - so
/// `uv * uViewportSize` is the destination pixel the dither is keyed on.
uniform vec2 uViewportSize;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 c = texture(uSource, uv);
    float dither = spotDitherOffset(uv * uViewportSize);
    fragColor = (c == vec4(0.0)) ? vec4(0.0) : max(c + dither, 0.0);
}
