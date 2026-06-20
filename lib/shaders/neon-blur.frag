precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;       // bar (Pass 2) or H-blurred (Pass 3)
uniform vec2      uTexelSize;    // 1.0 / fboSize
uniform vec2      uDirection;    // (1,0) for horizontal, (0,1) for vertical
uniform float     uBlurRadius;   // requested blur extent in pixels (= glowRadius)

// ---------------------------------------------------------------------------
// 13-tap separable Gaussian. Kernel naturally has σ = 3 tap-units; the
// per-tap offset is scaled by uBlurRadius so the screen-space sigma in pixels
// equals uBlurRadius. Very large radii get a non-ideal kernel shape but the
// neon halo is forgiving — visually it stays smooth and crease-free.
// ---------------------------------------------------------------------------

#define BLUR_NATURAL_SIGMA 3.0
#define BLUR_TAPS_PER_SIDE 6

void main() {
    vec2 uv = gl_FragCoord.xy * uTexelSize;

    // Centre tap + 6 paired side taps; Gaussian weights for σ=3.
    const float w0 = 0.137;
    const float w[BLUR_TAPS_PER_SIDE] = float[BLUR_TAPS_PER_SIDE](
        0.1296, 0.1097, 0.0831, 0.0563, 0.0342, 0.0185
    );

    float stride = max(uBlurRadius, 1.0) / BLUR_NATURAL_SIGMA;
    vec2  step   = uDirection * uTexelSize * stride;

    vec3 result = texture(uSource, uv).rgb * w0;
    for (int i = 0; i < BLUR_TAPS_PER_SIDE; i++) {
        vec2 offset = step * float(i + 1);
        result += texture(uSource, uv + offset).rgb * w[i];
        result += texture(uSource, uv - offset).rgb * w[i];
    }

    fragColor = vec4(result, 1.0);
}
