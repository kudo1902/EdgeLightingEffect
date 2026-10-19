precision highp float;

// Debug: the neon's shared buffer (SharedBufferId::NEON) as DebugRenderer's
// inset, drawn on a quad in a corner of the frame. Each texel of the buffer is
// a block of the inset, read with texelFetch, so the buffer's own grid shows
// and its filter is never touched - a reader of a shared buffer must not change
// its texture's parameters. The buffer is premultiplied, so its colour IS the
// layer over black; the inset is opaque, drawn with blending off, with a 1 px
// frame so it reads against whatever the frame holds behind it.

in vec2 vPos; // inset-local px, (0, 0) at its lower-left corner
out vec4 fragColor;

uniform sampler2D uBuffer;
uniform vec2 uInsetSize;  // the inset, px
uniform vec4 uFrameColor; // the frame round it

void main() {
    if (vPos.x < 1.0 || vPos.y < 1.0 || vPos.x > uInsetSize.x - 1.0 || vPos.y > uInsetSize.y - 1.0) {
        fragColor = uFrameColor;
        return;
    }
    ivec2 size = textureSize(uBuffer, 0);
    ivec2 texel = clamp(ivec2(vPos / uInsetSize * vec2(size)), ivec2(0), size - ivec2(1));
    fragColor = vec4(texelFetch(uBuffer, texel, 0).rgb, 1.0);
}
