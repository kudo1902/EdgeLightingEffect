precision highp float;

// Debug: one offscreen buffer a renderer published (BufferRegistry), shown
// one flat block of pixels per texel - in place over the area of the frame it
// maps onto, or as a panel for a table that maps onto none. See
// DebugConfig::bufferView and DebugRenderer::renderBufferView.
//
// Drawn on the unit quad ([-1, +1]) the stop markers use, scaled onto the
// destination box, so vPos runs across it. Opaque, drawn with blending off: in
// place, the view REPLACES the frame inside the box, since the buffer
// composited over what was drawn from it would show neither.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource; ///< The buffer. texelFetch, so whatever filter it was allocated with never applies.
uniform vec2 uDestSize;    ///< The destination box, px - for the outline.
uniform int uChannels;     ///< How many channels the buffer has: 1, 2, 3 or 4 (0 if unknown: treated as 4).
uniform int uChannel;      ///< DebugBufferChannel: 0 RGB, 1 red, 2 green, 3 blue, 4 alpha.
uniform float uGain;       ///< Multiplies the value shown, before the display clamps it.
uniform vec4 uBorderColor; ///< The box's 1 px outline; alpha mixes it over the buffer.

void main() {
    vec2 px = (vPos * 0.5 + 0.5) * uDestSize;
    ivec2 texels = textureSize(uSource, 0);
    // The texel under the pixel, at the uv the box maps it to - for a buffer
    // in place, the uv its producer reads it at - so each texel shows as the
    // pixels it spans.
    ivec2 texel = clamp(ivec2(floor(px / uDestSize * vec2(texels))), ivec2(0), texels - ivec2(1));
    vec4 c = texelFetch(uSource, texel, 0);
    int channels = uChannels > 0 ? uChannels : 4;

    vec3 rgb;
    if (uChannel == 0) {
        // A fetch fills missing channels with (0, 0, 1): a one-channel buffer
        // would show red alone, so it shows grey instead.
        rgb = channels == 1 ? vec3(c.r) : c.rgb;
    } else {
        // One channel as grey; one the buffer does not have is black, not the
        // fetch's fill.
        int k = uChannel - 1;
        float v = k == 0 ? c.r : (k == 1 ? c.g : (k == 2 ? c.b : c.a));
        bool present = k == 3 ? channels == 4 : k < channels;
        rgb = present ? vec3(v) : vec3(0.0);
    }
    rgb = clamp(rgb * uGain, 0.0, 1.0);

    vec2 edge = min(px, uDestSize - px);
    if (min(edge.x, edge.y) < 1.0) {
        rgb = mix(rgb, uBorderColor.rgb, uBorderColor.a);
    }
    fragColor = vec4(rgb, 1.0);
}
