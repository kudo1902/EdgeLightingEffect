precision highp float;

// LensFlareRenderer's composite of its reduced-resolution buffer back over the
// caller's framebuffer, below resolutionScale 1.0. Drawn with neon.vert over an
// NDC quad at an identity uMVP, so vPos is NDC. The renderer's premultiplied
// over (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) is in force.
//
// Its own program rather than a plain texture read, because it is where the
// scaled path DITHERS (V32). lens-flare.frag dithers its own output at 1.0, but
// on this path it writes a buffer the bilinear read below filters, and a
// filtered read averages a per-texel dither into blotches - while an RGBA8
// buffer has already rounded the haze into one-level plateaus that no later
// noise can recover. So the buffer is half float and undithered, and the noise
// goes on here, once per destination pixel, after the filter: the split the
// neon (R7) and the spotlight (V27) make.
//
// On a driver that will not render to half float the buffer falls back to
// RGBA8 and lens-flare.frag dithers into it as well (uDither 1); this pass
// still dithers on top.
//
// Two things are kept from spotlight-blit.frag, where both were measured for
// speed (this pass covers the whole viewport): the pixel position comes from
// vPos and uViewportSize, not gl_FragCoord, and an EMPTY texel writes an exact
// 0, so the blend can skip it and a host target that is not 8 bits gets no
// noise where the flare drew nothing.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

/// The caller's viewport in pixels - what the blit's NDC quad covers - so
/// `uv * uViewportSize` is the destination pixel the dither is keyed on.
uniform vec2 uViewportSize;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 c = texture(uSource, uv);
    if (c == vec4(0.0)) {
        fragColor = vec4(0.0);
        return;
    }
    // Colour only; see lens-flare-dither.glsl.
    fragColor = vec4(max(c.rgb + flareDitherOffset(uv * uViewportSize), 0.0), c.a);
}
