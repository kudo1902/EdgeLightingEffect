precision mediump float;

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

void main() {
    // vPos is in NDC [-1, 1] (identity MVP in the blit pass). Remap to UV.
    vec2 uv = vPos * 0.5 + 0.5;
    fragColor = texture(uSource, uv);
}
