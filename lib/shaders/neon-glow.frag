#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2 uRectSize;
uniform float uCornerRadius;
uniform float uStrokeThickness;
uniform float uIntensity;
uniform float uTime;
uniform float uSpeed;
uniform float uGlowSize;
uniform vec2 uViewportSize;
uniform sampler2D uGradient;
uniform bool uShowGradient;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Find the closest point on the perimeter and return its distance from p.
float closestPerimDist(vec2 p, vec2 halfSize, float r, out vec2 cp) {
    vec2 c = halfSize - r;
    vec2 ap = abs(p);
    vec2 sp = sign(p);

    if (ap.x > c.x && ap.y > c.y) {
        vec2 delta = ap - c;
        float d = length(delta);
        vec2 dir = delta / d;
        cp = vec2(c.x + dir.x * r, c.y + dir.y * r) * sp;
    } else if (ap.x > c.x) {
        cp = vec2(halfSize.x, clamp(ap.y, 0.0, c.y)) * sp;
    } else if (ap.y > c.y) {
        cp = vec2(clamp(ap.x, 0.0, c.x), halfSize.y) * sp;
    } else {
        float ex = c.x - ap.x;
        float ey = c.y - ap.y;
        if (ex < ey)
            cp = vec2(halfSize.x, ap.y) * sp;
        else
            cp = vec2(ap.x, halfSize.y) * sp;
    }
    return length(p - cp);
}

void main() {
    vec2 halfSize = uRectSize * 0.5;

    if (uShowGradient)
    {
        vec2 uv = gl_FragCoord.xy / uViewportSize;
        fragColor = vec4(texture(uGradient, uv).rgb, 1.0);
        return;
    }

    // Analytical closest-point projection to find nearest perimeter point
    float d = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);
    vec2 cp;
    closestPerimDist(vPos, halfSize, uCornerRadius, cp);
    vec2 closestOffset = cp - vPos;

    vec2 uv = (gl_FragCoord.xy + closestOffset) / uViewportSize;
    vec3 col = texture(uGradient, uv).rgb;

    // Desaturate toward centre to hide closest-point discontinuities (4 blocks)
    // Only for interior points — exterior keeps full color
    float t = clamp(ad / min(halfSize.x, halfSize.y), 0.0, 1.0);
    if (d >= 0.0) t = 0.0;
    vec3 neutral = vec3(dot(col, vec3(0.3333)));
    col = mix(col, neutral, t * 0.6);

    float w = max(uStrokeThickness * 0.5, 0.1);
    float core = w / (ad + 0.8);
    float glow = pow(w / (ad + uGlowSize * 0.5), 1.6);
    float bloomFalloff = max(uGlowSize * 0.4, 2.0);
    float fill = smoothstep(bloomFalloff * 1.5, -bloomFalloff * 0.5, d);

    float outsideStrength = smoothstep(-uGlowSize * 0.3, 1.0, d);
    float insideStrength = 1.0 - smoothstep(-uGlowSize * 0.5, 0.0, d);

    vec3 result = col * (core * 0.35 + glow * 0.90 + fill * 0.18)
                * outsideStrength * uIntensity;
    result += col * (core * 0.35 + glow * 0.90) * insideStrength * uIntensity;

    result = result / (result + vec3(0.6));
    result = pow(result, vec3(0.85));
    float grain = 0.04;
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * grain;

    fragColor = vec4(result, 1.0);
}
