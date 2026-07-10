precision highp float;

// Debug: sample the baked gradient LUT along the strip's x axis so the
// colour ring baked in rebuildGradientLUT() can be verified visually.
// Drawn on a small quad centred on the rect origin.

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uGradientLUT;
uniform vec2  uStripHalfSize; ///< half-width, half-height of the strip in local px.
uniform float uTime;
uniform float uHueRotationRate;

void main() {
    // Map local x in [-halfW, +halfW] to u in [0, 1], then apply the same
    // time-based hue scroll as the neon shader so this preview matches what
    // the perimeter is actually reading each frame (REPEAT-wrapped LUT).
    float u = vPos.x / (2.0 * uStripHalfSize.x) + 0.5;
    u -= uTime * uHueRotationRate;
    vec3 col = texture(uGradientLUT, vec2(u, 0.5)).rgb;
    fragColor = vec4(col, 1.0);
}
