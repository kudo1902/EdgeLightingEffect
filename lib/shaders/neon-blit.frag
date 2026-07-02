precision highp float;

// Pass 2 of the optimized neon: bilinear composite of the half-res neon FBO
// (premultiplied colour + coverage alpha) onto the backbuffer. The opaque-mode
// silhouette is handled entirely by the black-rect fullscreen pass drawn just
// before this blit in NeonOptimizedRenderer::Render — the black quad's
// analytic SDF anti-aliasing lands cleanly on rounded corners regardless of
// softness, whereas the old per-fragment discard here stair-stepped at any
// corner radius > 0.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    fragColor = texture(uSource, uv); // premultiplied colour + coverage alpha
}
