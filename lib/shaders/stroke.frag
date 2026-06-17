#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2 uRectSize;
uniform float uCornerRadius;
uniform float uStrokeThickness;
uniform float uIntensity;
uniform int uStrokeAlignment;
uniform float uFadeRange;
uniform int uFadeMode;

uniform int uStrokeAnimation;
uniform float uSegmentLength;
uniform float uHeadPosition;
uniform float uTime;
uniform float uSpeed;

uniform int uLineCount;
uniform float uGlowSize;
uniform int uWinding;

uniform int uColorStopCount;
uniform float uColorStopPositions[16];
uniform vec4 uColorStopColors[16];
uniform int uBlendSpace;

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

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

float getPerimeterPosCW(vec2 p, vec2 b, float r) {
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

float getPerimeterPosCCW(vec2 p, vec2 b, float r) {
    if (r <= 0.01) {
        float w_str = 2.0 * b.x;
        float h_str = 2.0 * b.y;
        float total = 2.0 * w_str + 2.0 * h_str;
        float distToTop = abs(p.y - b.y);
        float distToBottom = abs(p.y + b.y);
        float distToRight = abs(p.x - b.x);
        float distToLeft = abs(p.x + b.x);
        float minDist = min(min(distToTop, distToBottom), min(distToRight, distToLeft));
        if (minDist == distToLeft) {
            float t = (b.y - p.y) / h_str;
            return (t * h_str) / total;
        } else if (minDist == distToBottom) {
            float t = (p.x + b.x) / w_str;
            return (h_str + t * w_str) / total;
        } else if (minDist == distToRight) {
            float t = (p.y + b.y) / h_str;
            return (h_str + w_str + t * h_str) / total;
        } else {
            float t = (b.x - p.x) / w_str;
            return (h_str + w_str + h_str + t * w_str) / total;
        }
    }

    float w_str = 2.0 * (b.x - r);
    float h_str = 2.0 * (b.y - r);
    float arc = 0.5 * 3.1415926535 * r;
    float total = 4.0 * arc + 2.0 * w_str + 2.0 * h_str;
    vec2 c_tr = b - vec2(r);

    // corners (CCW order: BL, BR, TR, TL)
    if (p.x >= c_tr.x && p.y <= -c_tr.y) {
        // BR arc -π/2 → 0
        float angle = atan(p.y + c_tr.y, p.x - c_tr.x);
        float t = (angle + 3.14159265 * 0.5) / (3.14159265 * 0.5);
        return (h_str + arc + w_str + t * arc) / total;
    }
    if (p.x <= -c_tr.x && p.y <= -c_tr.y) {
        // BL arc -π → -π/2
        float angle = atan(p.y + c_tr.y, p.x + c_tr.x);
        float t = (angle + 3.14159265) / (3.14159265 * 0.5);
        return (h_str + t * arc) / total;
    }
    if (p.x >= c_tr.x && p.y >= c_tr.y) {
        // TR arc 0 → π/2
        float angle = atan(p.y - c_tr.y, p.x - c_tr.x);
        float t = angle / (3.14159265 * 0.5);
        return (h_str + arc + w_str + arc + h_str + t * arc) / total;
    }
    if (p.x <= -c_tr.x && p.y >= c_tr.y) {
        // TL arc π/2 → π
        float angle = atan(p.y - c_tr.y, p.x + c_tr.x);
        float t = (angle - 3.14159265 * 0.5) / (3.14159265 * 0.5);
        return (h_str + arc + w_str + arc + h_str + arc + w_str + t * arc) / total;
    }

    // straight edges (CCW: left → bottom → right → top)
    if (p.x <= -c_tr.x) {
        // left edge T→B
        float t = (c_tr.y - p.y) / h_str;
        return (t * h_str) / total;
    }
    if (p.y <= -c_tr.y) {
        // bottom edge L→R
        float t = (p.x + c_tr.x) / w_str;
        return (h_str + arc + t * w_str) / total;
    }
    if (p.x >= c_tr.x) {
        // right edge B→T
        float t = (p.y + c_tr.y) / h_str;
        return (h_str + arc + w_str + arc + t * h_str) / total;
    }
    if (p.y >= c_tr.y) {
        // top edge R→L
        float t = (c_tr.x - p.x) / w_str;
        return (h_str + arc + w_str + arc + h_str + arc + t * w_str) / total;
    }

    // interior (inside the rectangle, not near any corner or edge)
    float dTop = c_tr.y - p.y;
    float dBottom = p.y + c_tr.y;
    float dRight = c_tr.x - p.x;
    float dLeft = p.x + c_tr.x;
    float minD = min(min(dTop, dBottom), min(dRight, dLeft));
    if (minD == dTop) {
        float t = (c_tr.x - p.x) / w_str;
        return (h_str + arc + w_str + arc + h_str + arc + t * w_str) / total;
    } else if (minD == dRight) {
        float t = (p.y + c_tr.y) / h_str;
        return (h_str + arc + w_str + arc + t * h_str) / total;
    } else if (minD == dBottom) {
        float t = (p.x - c_tr.x) / w_str;
        return (h_str + arc + t * w_str) / total;
    } else {
        float t = (c_tr.y - p.y) / h_str;
        return (t * h_str) / total;
    }
}

float getPerimeterPos(vec2 p, vec2 b, float r) {
    if (uWinding == 0)
        return getPerimeterPosCW(p, b, r);
    return getPerimeterPosCCW(p, b, r);
}

vec3 blend(vec3 a, vec3 b, float t) {
    if (uBlendSpace == 1) return blendHSV(a, b, t);
    return blendRGB(a, b, t);
}

vec3 sampleStops(float pos) {
    int count = uColorStopCount;
    if (count <= 0) return vec3(1.0);
    if (count == 1) return uColorStopColors[0].rgb;
    if (count == 2) {
        float tri = 1.0 - abs(2.0 * pos - 1.0);
        return blend(uColorStopColors[0].rgb, uColorStopColors[1].rgb, tri);
    }
    for (int i = 0; i < count; i++) {
        int next = (i + 1 < count) ? i + 1 : 0;
        float a = uColorStopPositions[i];
        float b = uColorStopPositions[next];
        if (next != 0) {
            if (pos >= a && pos < b) {
                float t = (pos - a) / max(b - a, 0.0001);
                return blend(uColorStopColors[i].rgb, uColorStopColors[next].rgb, t);
            }
        } else {
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
    float d = sdRoundedBox(vPos, halfSize, uCornerRadius);
    float perimPos = getPerimeterPos(vPos, halfSize, uCornerRadius);
    float totalP = getPerimeterLength(uRectSize, uCornerRadius);

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
        float headBase = uHeadPosition;
        int count = max(uLineCount, 1);
        float bodyEndPix = uSegmentLength * totalP;
        float movingAlpha = 0.0;

        for (int i = 0; i < 16; i++) {
            if (i >= count) break;
            float offset = float(i) / float(count);
            float head = fract(headBase + offset);
            float diff = head - perimPos;
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

    if (uStrokeAnimation == 2) {
        float blink = step(0.5, fract(uTime * uSpeed));
        finalAlpha *= blink;
    }

    if (uStrokeAlignment == 1 && d > 0.0) discard;
    if (uStrokeAlignment == 2 && d < 0.0) discard;

    if (finalAlpha < 0.01) discard;

    vec3 color = sampleStops(perimPos);

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    dither = (dither - 0.5) / 255.0;

    fragColor = vec4(color, 1.0) * (finalAlpha * uIntensity) + vec4(vec3(dither), 0.0);
}
