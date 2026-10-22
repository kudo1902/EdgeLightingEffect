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
// noise can recover. So the buffer holds ten bits, undithered, and the noise
// goes on here, once per destination pixel, after the filter: the split the
// neon (R7) and the spotlight (V27) make.
//
// The buffer is RGB10_A2 (I96), whose two alpha bits cannot hold the coverage,
// so this pass REBUILDS it: lens-flare.frag writes alpha = max(r, g, b), and
// the brightest channel of the filtered colour stands in for it, as
// neon-blit.frag's does for the neon's.
//
// Two things are kept from spotlight-blit.frag, where both were measured for
// speed (this pass covers the whole viewport): the pixel position comes from
// vPos and uViewportSize, not gl_FragCoord, and an EMPTY texel writes an exact
// 0, so the blend can skip it and a host target that is not 8 bits gets no
// noise where the flare drew nothing.

in vec2 vPos;
out vec4 fragColor;

// HIGHP, not the default. GLSL ES 3.00 predeclares `precision lowp
// sampler2D`, and a texture lookup returns its sampler's precision: lowp
// guarantees only 2^-8, so a driver that honours it rounds this buffer's ten
// bits back to eight on the read - the one-level plateaus V32's dither exists
// to break up, which no noise added after the read can recover. Desktop GL
// gives precision qualifiers no meaning, so nothing on a Mac shows it (I97).
uniform highp sampler2D uSource;

/// The caller's viewport in pixels - what the blit's NDC quad covers - so
/// `uv * uViewportSize` is the destination pixel the dither is keyed on.
uniform vec2 uViewportSize;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec3 c = texture(uSource, uv).rgb;
    if (c == vec3(0.0)) {
        fragColor = vec4(0.0);
        return;
    }
    float a = max(c.r, max(c.g, c.b)); // the coverage, rebuilt - see above
    // Colour only; see lens-flare-dither.glsl.
    fragColor = vec4(max(c + flareDitherOffset(uv * uViewportSize), 0.0), a);
}
