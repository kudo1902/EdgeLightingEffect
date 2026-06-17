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

uniform int uColorStopCount;
uniform float uColorStopPositions[16];
uniform vec4 uColorStopColors[16];
uniform int uBlendSpace;

// --- SDF rounded box ---
// signed distance to a rounded rectangle centered at origin
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// --- HSV ---
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (d + e), q.x);
}

// --- Film grain hash ---
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// --- Blending ---
vec3 blendRGB(vec3 a, vec3 b, float t) {
    return mix(a, b, t);
}

vec3 blendHSV(vec3 a, vec3 b, float t) {
    vec3 ha = rgb2hsv(a);
    vec3 hb = rgb2hsv(b);
    float dh = hb.x - ha.x;
    if (dh > 0.5) dh -= 1.0;
    if (dh < -0.5) dh += 1.0;
    return hsv2rgb(vec3(ha.x + dh * t, mix(ha.y, hb.y, t), mix(ha.z, hb.z, t)));
}

vec3 blend(vec3 a, vec3 b, float t) {
    if (uBlendSpace == 1) return blendHSV(a, b, t);
    return blendRGB(a, b, t);
}

// --- Color stop sampling ---
vec3 sampleStops(float pos) {
    int count = uColorStopCount;
    if (count <= 0) return vec3(1.0);
    if (count == 1) return uColorStopColors[0].rgb;
    if (count == 2) {
        // triangular blend: stop0 at pos 0 & 1, stop1 at pos 0.5
        float tri = 1.0 - abs(2.0 * pos - 1.0);
        return blend(uColorStopColors[0].rgb, uColorStopColors[1].rgb, tri);
    }
    // 3+ stops: walk the segments, wrapping last→first
    for (int i = 0; i < count; i++) {
        int next = (i + 1 < count) ? i + 1 : 0;
        float a = uColorStopPositions[i];
        float b = uColorStopPositions[next];
        if (next != 0) {
            // normal segment (non-wrap)
            if (pos >= a && pos < b) {
                float t = (pos - a) / max(b - a, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
        } else {
            // wrap segment: last stop → first stop across the 0/1 boundary
            float wrapLen = (1.0 - a) + b;
            if (pos >= a) {
                float t = (pos - a) / max(wrapLen, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
            if (pos < b) {
                float t = ((1.0 - a) + pos) / max(wrapLen, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
        }
    }
    return uColorStopColors[0].rgb;
}

void main() {
    vec2 halfSize = uRectSize * 0.5;

    // d = signed distance to the rounded-rect edge
    //   d < 0  →  inside the rectangle
    //   d = 0  →  exactly on the edge
    //   d > 0  →  outside the rectangle
    float d = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // --- Determine the color for this fragment ---
    // Instead of using the fragment's own angular position (which creates a
    // rotating conic wheel at the centre), we trace back along the SDF
    // gradient to find the closest point ON the perimeter and use its angle.
    // This way every fragment in a line perpendicular to the edge gets the
    // same colour — no conic wheel.
    float eps = max(0.5, min(uRectSize.x, uRectSize.y) * 0.001);
    vec2 grad = vec2(
        sdRoundBox(vPos + vec2(eps, 0.0), halfSize, uCornerRadius) -
        sdRoundBox(vPos - vec2(eps, 0.0), halfSize, uCornerRadius),
        sdRoundBox(vPos + vec2(0.0, eps), halfSize, uCornerRadius) -
        sdRoundBox(vPos - vec2(0.0, eps), halfSize, uCornerRadius)
    );
    float gradLen = length(grad);
    float perimAng;
    if (gradLen < 1e-4) {
        perimAng = 0.0; // centre of a symmetric rect → arbitrary reference
    } else {
        vec2 closestPoint = vPos - d * (grad / gradLen);
        perimAng = atan(closestPoint.y, closestPoint.x);
    }
    // Map angle [-π, π] to [0, 1] perimeter position + time animation
    float h = fract((perimAng / 6.2831853) + 0.5 + uTime * uSpeed);
    vec3 col = sampleStops(h); // ← the colour for this fragment

    // --- Three-layer neon (ported from neon-square.html) ---
    // All three layers use `ad` = distance from the edge, so they are
    // SYMMETRIC — they glow on both the inside and the outside.

    // 1. Core: bright thin line at the stroke edge
    //   w / (ad + ε)  — inverse-distance falloff, ε prevents division by zero
    float w = max(uStrokeThickness * 0.5, 0.1);
    float core = w / (ad + 0.8);

    // 2. Glow: soft halo that spreads wider than the core
    //   pow(w / (ad + spread), 1.6)  — slower falloff = wider halo
    float glow = pow(w / (ad + uGlowSize * 0.5), 1.6);

    // 3. Fill (outer bloom): extra glow that only appears outside
    //   smoothstep(outside_start, inside_end, d)  → 1 far out, 0 far in
    //   At the edge (d=0) it is ~0.16, ramping to 1 at ~glowSize×0.6 px out
    float bloomFalloff = max(uGlowSize * 0.4, 2.0);
    float fill = smoothstep(bloomFalloff * 1.5, -bloomFalloff * 0.5, d);

    // --- Separate interior from exterior ---
    // outsideStrength gates the COLOURED three-layer glow to the exterior:
    //   d ≥ 1 px outside → outsideStrength = 1 (full colour)
    //   d ≤ –glowSize×0.3 px inside → outsideStrength ≈ 0 (no colour)
    // This prevents the 4 coloured quadrant shapes from appearing inside.
    float outsideStrength = smoothstep(-uGlowSize * 0.3, 1.0, d);

    // --- EXTERIOR: full-colour three-layer glow ---
    vec3 result = col * (core * 0.35 + glow * 0.90 + fill * 0.18) * outsideStrength * uIntensity;

    // --- INTERIOR: dim neutral spill only ---
    // insideStrength ramps from 0 at the edge to 1 at –glowSize×0.5 px inside
    float insideStrength = 1.0 - smoothstep(-uGlowSize * 0.5, 0.0, d);
    // A very dim (0.04×) neutral-grey version of the glow — no colour shapes
    result += vec3(glow * 0.04) * insideStrength * uIntensity;

    // --- Post-processing ---
    // Reinhard tone map — compresses HDR values while preserving colour
    result = result / (result + vec3(0.6));
    result = pow(result, vec3(0.85)); // gamma lift

    // Film grain
    float grain = 0.04;
    result += (hash(gl_FragCoord.xy + uTime) - 0.5) * grain;

    fragColor = vec4(result, 1.0);
}
