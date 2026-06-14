#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2 uRectSize;
uniform float uBorderRadius;
uniform float uLineWidth;
uniform float uIntensity;
uniform int uColorMode;
uniform vec4 uPrimaryColor;
uniform vec4 uSecondaryColor;
uniform float uTime;

uniform float uInnerGlowWidth;
uniform float uInnerGlowIntensity;
uniform float uOuterGlowWidth;
uniform float uOuterGlowIntensity;
uniform int uEdgeSelector;
uniform float uEdgeStripWidth;
uniform int uEdgeStripSide;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float getPerimeterPos(vec2 p, vec2 b, float r) {
    if (r <= 0.01) {
        float w_str = 2.0 * b.x;
        float h_str = 2.0 * b.y;
        float total = 2.0 * w_str + 2.0 * h_str;
        float distToTop = abs(p.y - b.y);
        float distToBottom = abs(p.y + b.y);
        float distToRight = abs(p.x - b.x);
        float distToLeft = abs(p.x + b.x);
        float minDist = min(min(distToTop, distToBottom), min(distToRight, distToLeft));
        if (minDist == distToTop) {
            float t = (p.x + b.x) / w_str;
            return (t * w_str) / total;
        } else if (minDist == distToRight) {
            float t = (b.y - p.y) / h_str;
            return (w_str + t * h_str) / total;
        } else if (minDist == distToBottom) {
            float t = (b.x - p.x) / w_str;
            return (w_str + h_str + t * w_str) / total;
        } else {
            float t = (p.y + b.y) / h_str;
            return (2.0 * w_str + h_str + t * h_str) / total;
        }
    }

    float w_str = 2.0 * (b.x - r);
    float h_str = 2.0 * (b.y - r);
    float arc = 0.5 * 3.1415926535 * r;
    float total = 4.0 * arc + 2.0 * w_str + 2.0 * h_str;
    vec2 c_tr = b - vec2(r);

    if (p.x >= c_tr.x && p.y >= c_tr.y) {
        float angle = atan(p.y - c_tr.y, p.x - c_tr.x);
        float t = (3.1415926535 * 0.5 - angle) / (3.1415926535 * 0.5);
        return (w_str + t * arc) / total;
    } else if (p.x >= c_tr.x && p.y <= -c_tr.y) {
        float angle = atan(-p.y - c_tr.y, p.x - c_tr.x);
        float t = angle / (3.1415926535 * 0.5);
        return (w_str + arc + h_str + t * arc) / total;
    } else if (p.x <= -c_tr.x && p.y <= -c_tr.y) {
        float angle = atan(-p.y - c_tr.y, -p.x - c_tr.x);
        float t = (3.1415926535 * 0.5 - angle) / (3.1415926535 * 0.5);
        return (w_str + arc + h_str + arc + w_str + t * arc) / total;
    } else if (p.x <= -c_tr.x && p.y >= c_tr.y) {
        float angle = atan(p.y - c_tr.y, -p.x - c_tr.x);
        float t = angle / (3.1415926535 * 0.5);
        return (w_str + arc + h_str + arc + w_str + arc + h_str + t * arc) / total;
    } else if (p.y >= c_tr.y) {
        float t = (p.x - (-c_tr.x)) / (2.0 * c_tr.x);
        return (t * w_str) / total;
    } else if (p.x >= c_tr.x) {
        float t = (c_tr.y - p.y) / (2.0 * c_tr.y);
        return (w_str + arc + t * h_str) / total;
    } else if (p.y <= -c_tr.y) {
        float t = (c_tr.x - p.x) / (2.0 * c_tr.x);
        return (w_str + arc + h_str + arc + t * w_str) / total;
    } else if (p.x <= -c_tr.x) {
        float t = (p.y - (-c_tr.y)) / (2.0 * c_tr.y);
        return (w_str + arc + h_str + arc + w_str + arc + t * h_str) / total;
    } else {
        float dTop = c_tr.y - p.y;
        float dBottom = p.y + c_tr.y;
        float dRight = c_tr.x - p.x;
        float dLeft = p.x + c_tr.x;
        float minD = min(min(dTop, dBottom), min(dRight, dLeft));
        if (minD == dTop) {
            float t = (p.x + c_tr.x) / (2.0 * c_tr.x);
            return (t * w_str) / total;
        } else if (minD == dRight) {
            float t = (c_tr.y - p.y) / (2.0 * c_tr.y);
            return (w_str + arc + t * h_str) / total;
        } else if (minD == dBottom) {
            float t = (c_tr.x - p.x) / (2.0 * c_tr.x);
            return (w_str + arc + h_str + arc + t * w_str) / total;
        } else {
            float t = (p.y + c_tr.y) / (2.0 * c_tr.y);
            return (w_str + arc + h_str + arc + w_str + arc + t * h_str) / total;
        }
    }
}

vec3 getRainbowColor(float p) {
    vec3 c1 = vec3(1.0, 0.0, 0.0);
    vec3 c2 = vec3(1.0, 1.0, 0.0);
    vec3 c3 = vec3(0.0, 1.0, 0.0);
    vec3 c4 = vec3(0.0, 1.0, 1.0);
    vec3 c5 = vec3(0.0, 0.0, 1.0);
    vec3 c6 = vec3(1.0, 0.0, 1.0);
    float w = 1.0 / 6.0;
    if (p < w) return mix(c1, c2, p / w);
    if (p < 2.0 * w) return mix(c2, c3, (p - w) / w);
    if (p < 3.0 * w) return mix(c3, c4, (p - 2.0 * w) / w);
    if (p < 4.0 * w) return mix(c4, c5, (p - 3.0 * w) / w);
    if (p < 5.0 * w) return mix(c5, c6, (p - 4.0 * w) / w);
    return mix(c6, c1, (p - 5.0 * w) / w);
}

float distToEdgeSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    vec2 ap = p - a;
    float t = dot(ap, ab) / dot(ab, ab);
    t = clamp(t, 0.0, 1.0);
    return length(ap - t * ab);
}

float edgeSelectorMask(vec2 p, vec2 hs) {
    if (uEdgeSelector == 15) return 1.0;
    float glowW = max(uInnerGlowWidth, uOuterGlowWidth);
    float k = 1.0 / max(glowW * glowW * 0.3, 1.0);
    float mask = 0.0;
    if ((uEdgeSelector & 1) != 0) {
        float d = distToEdgeSegment(p, vec2(-hs.x, hs.y), vec2(hs.x, hs.y));
        mask = max(mask, exp(-d * d * k));
    }
    if ((uEdgeSelector & 2) != 0) {
        float d = distToEdgeSegment(p, vec2(hs.x, hs.y), vec2(hs.x, -hs.y));
        mask = max(mask, exp(-d * d * k));
    }
    if ((uEdgeSelector & 4) != 0) {
        float d = distToEdgeSegment(p, vec2(hs.x, -hs.y), vec2(-hs.x, -hs.y));
        mask = max(mask, exp(-d * d * k));
    }
    if ((uEdgeSelector & 8) != 0) {
        float d = distToEdgeSegment(p, vec2(-hs.x, -hs.y), vec2(-hs.x, hs.y));
        mask = max(mask, exp(-d * d * k));
    }
    return mask;
}

float edgeStripClip(float d) {
    if (uEdgeStripWidth <= 0.0) return 1.0;
    if (uEdgeStripSide == 0) {
        float halfW = uEdgeStripWidth * 0.5;
        float t = abs(d) / halfW;
        return 1.0 - smoothstep(0.0, 1.0, t);
    } else if (uEdgeStripSide == 1) {
        if (d > 0.0) return 0.0;
        float t = abs(d) / uEdgeStripWidth;
        return 1.0 - smoothstep(0.0, 1.0, t);
    } else {
        if (d < 0.0) return 0.0;
        float t = d / uEdgeStripWidth;
        return 1.0 - smoothstep(0.0, 1.0, t);
    }
}

void main() {
    vec2 halfSize = uRectSize * 0.5;
    float d = sdRoundedBox(vPos, halfSize, uBorderRadius);
    float absD = abs(d);

    float edgeMask = edgeSelectorMask(vPos, halfSize);
    if (edgeMask < 0.01) discard;

    float stripClip = edgeStripClip(d);
    if (stripClip < 0.01) discard;

    float effectiveInnerW = uInnerGlowWidth;
    float effectiveOuterW = uOuterGlowWidth;
    if (uEdgeStripWidth > 0.0) {
        if (uEdgeStripSide == 1) {
            effectiveInnerW = uEdgeStripWidth;
        } else if (uEdgeStripSide == 2) {
            effectiveOuterW = uEdgeStripWidth;
        } else {
            effectiveInnerW = uEdgeStripWidth * 0.5;
            effectiveOuterW = uEdgeStripWidth * 0.5;
        }
    }
    float maxGlowW = max(effectiveInnerW, effectiveOuterW);
    if (absD > maxGlowW + 5.0) discard;

    float p_pos = getPerimeterPos(vPos, halfSize, uBorderRadius);

    float pulse = 1.0 + 0.12 * sin(uTime * 3.5);
    float pulseIntensity = uIntensity * (1.0 + 0.08 * sin(uTime * 3.5));

    float core = smoothstep(uLineWidth * 0.5, 0.0, absD);
    float innerVal = (d < 0.0)
        ? exp(-abs(d) / (effectiveInnerW * 0.35 * pulse)) * uInnerGlowIntensity
        : 0.0;
    float outerVal = (d > 0.0)
        ? exp(-d / (effectiveOuterW * 0.35 * pulse)) * uOuterGlowIntensity
        : 0.0;

    float totalGlow = (core * 0.6 + (innerVal + outerVal) * 0.4) * pulseIntensity * edgeMask * stripClip;

    vec4 baseColor;
    if (uColorMode == 0) {
        baseColor = uPrimaryColor;
    } else if (uColorMode == 1) {
        float flowPos = fract(p_pos - uTime * 0.15);
        baseColor = mix(uPrimaryColor, uSecondaryColor, flowPos);
    } else {
        float shift = fract(p_pos - uTime * 0.25);
        baseColor = vec4(getRainbowColor(shift), 1.0);
    }

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    dither = (dither - 0.5) / 255.0;

    fragColor = (baseColor * totalGlow) + vec4(vec3(dither), 0.0);
}
