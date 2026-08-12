precision highp float;

// Final pass of the scaled-resolution neon path (NeonConfig::resolutionScale
// below 1): bilinear composite of the scaled neon FBO (premultiplied colour +
// coverage alpha) onto the backbuffer. The opaque-mode silhouette is handled
// entirely by the black-rect fullscreen pass drawn just before this blit in
// NeonRenderer::Render - the fill quad's analytic SDF anti-aliasing lands
// cleanly on rounded corners regardless of softness, whereas the old
// per-fragment discard here stair-stepped at any corner radius > 0.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    fragColor = texture(uSource, uv); // premultiplied colour + coverage alpha
}
