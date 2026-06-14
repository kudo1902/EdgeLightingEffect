#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2 uRectSize;
uniform float uBorderRadius;
uniform float uStrokeThickness;
uniform float uIntensity;
uniform vec4 uPrimaryColor;
uniform vec4 uSecondaryColor;
uniform int uStrokeAlignment;
uniform float uFadeRange;
uniform int uFadeMode;

uniform int uStrokeAnimation;
uniform float uSegmentLength;
uniform float uSpeed;
uniform float uTime;

uniform int uColorMode;
uniform int uLineCount;
uniform float uGlowSize;

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float getPerimeterLength(vec2 size, float r) {
    if (r <= 0.01) {
        return 2.0 * (size.x + size.y);
    }
    float ws = size.x - 2.0 * r;
    float hs = size.y - 2.0 * r;
    float arc = 3.14159265 * r * 0.5;
    return 4.0 * arc + 2.0 * (ws + hs);
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

void main() {
    vec2 halfSize = uRectSize * 0.5;
    float d = sdRoundedBox(vPos, halfSize, uBorderRadius);
    float absD = abs(d);

    float limit, edgeDist;
    if (uStrokeAlignment == 1) {
        limit = uStrokeThickness;
        edgeDist = absD;
    } else if (uStrokeAlignment == 2) {
        limit = uStrokeThickness;
        edgeDist = d;
    } else {
        limit = uStrokeThickness * 0.5;
        edgeDist = absD;
    }

    limit += uGlowSize;

    float s = min(uFadeRange, limit);
    float lineAlpha = 1.0 - smoothstep(limit - s, limit, edgeDist);
    if (uFadeMode == 1 && uStrokeAlignment != 0 && s > 0.0) {
        lineAlpha *= smoothstep(0.0, s, edgeDist);
    }

    float finalAlpha = lineAlpha;

    if (uStrokeAnimation == 1) {
        float p_pos = getPerimeterPos(vPos, halfSize, uBorderRadius);
        float headBase = fract(uTime * uSpeed);
        int count = max(uLineCount, 1);
        float totalP = getPerimeterLength(uRectSize, uBorderRadius);
        float bodyEndPix = uSegmentLength * totalP;
        float movingAlpha = 0.0;

        for (int i = 0; i < 16; i++) {
            if (i >= count) break;
            float offset = float(i) / float(count);
            float head = fract(headBase + offset);
            float diff = head - p_pos;
            if (diff < 0.0) diff += 1.0;
            float diffPix = diff * totalP;

            if (diffPix <= bodyEndPix) {
                float tailSoftPix = bodyEndPix * 0.25;
                float fade = 1.0 - smoothstep(bodyEndPix - tailSoftPix, bodyEndPix, diffPix);
                movingAlpha = max(movingAlpha, lineAlpha * fade);
            } else {
                float forwardPix = totalP - diffPix;
                float capDist = sqrt(forwardPix * forwardPix + edgeDist * edgeDist);
                if (capDist <= limit) {
                    float capIntensity = 1.0 - smoothstep(0.0, limit, capDist);
                    movingAlpha = max(movingAlpha, max(lineAlpha, capIntensity));
                }
            }
        }
        finalAlpha = movingAlpha;
    }

    if (uStrokeAlignment == 1 && d > 0.0) discard;
    if (uStrokeAlignment == 2 && d < 0.0) discard;

    if (finalAlpha < 0.01) discard;

    vec3 color;
    if (uColorMode == 0) {
        color = uPrimaryColor.rgb;
    } else if (uColorMode == 1) {
        float t = getPerimeterPos(vPos, halfSize, uBorderRadius);
        float smoothT = 1.0 - abs(2.0 * t - 1.0);
        color = mix(uPrimaryColor.rgb, uSecondaryColor.rgb, smoothT);
    } else if (uColorMode == 2) {
        float hue = getPerimeterPos(vPos, halfSize, uBorderRadius);
        color = hsv2rgb(vec3(hue, 0.8, 1.0));
    } else if (uColorMode == 3) {
        float hue = fract(uTime * 0.15);
        color = hsv2rgb(vec3(hue, 0.8, 1.0));
    } else {
        float t = sin(uTime * 2.0) * 0.5 + 0.5;
        color = mix(uPrimaryColor.rgb, uSecondaryColor.rgb, t);
    }

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    dither = (dither - 0.5) / 255.0;

    fragColor = vec4(color, 1.0) * (finalAlpha * uIntensity) + vec4(vec3(dither), 0.0);
}
