precision highp float;

// Plain composite of a reduced-resolution buffer back over the caller's
// framebuffer: SpotlightRenderer's and LensFlareRenderer's scaled path. Drawn
// with neon.vert over an NDC quad at an identity uMVP, so vPos is NDC.
//
// Bilinear upsampling of premultiplied colour + coverage alpha is fringe-free,
// and the caller's premultiplied-over blend does the rest.
//
// Both layers used to compile neon-blit.frag for this and upload
// GlowSide::BOTH to switch off the neon's one-sided cut, which neither has a
// rect to apply. That made every neon-only addition to that shader something
// two unrelated renderers had to remember to disable. This is exactly what
// neon-blit.frag computes with its cut off - the same uv, and src * 1.0 is src
// - so moving them here changed no pixel. Keep it this small: a layer that
// needs more than a composite wants its own blit, as the neon has.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    fragColor = texture(uSource, uv);
}
