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
uniform float uHeadPosition;
uniform float uTime;
uniform float uSpeed;

uniform int uColorMode;
uniform int uLineCount;
uniform float uGlowSize;
uniform int uWinding;

uniform int   uPathSource;
uniform sampler1D uPathTexture;
uniform int   uPathPointCount;
uniform bool  uPathClosed;
uniform float uPathTotalLength;

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
        float t = (3.14159265 - angle) / (3.14159265 * 0.5);
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

/// Computes signed distance and perimeter position for a polyline path.
/// Returns vec2(signedDist, perimPos).
vec2 sdPolyline(vec2 p) {
    float minAbsDist = 1e10;
    float signedDist = 0.0;
    float perimPos = 0.0;
    float cumLen = 0.0;
    int n = uPathPointCount;
    int segCount = uPathClosed ? n : n - 1;

    for (int i = 0; i < segCount; ++i) {
        vec2 a = texelFetch(uPathTexture, i, 0).xy;
        vec2 b = texelFetch(uPathTexture, (i + 1) % n, 0).xy;

        vec2 ab = b - a;
        float segLen = length(ab);
        if (segLen < 0.0001) continue;

        float t = dot(p - a, ab) / (segLen * segLen);
        t = clamp(t, 0.0, 1.0);
        vec2 closest = a + ab * t;
        float dist = length(p - closest);

        if (dist < minAbsDist) {
            minAbsDist = dist;

            float side;
            if (t < 0.001) {
                // At vertex 'a' — use angle-weighted pseudonormal
                if (uPathClosed || i > 0) {
                    int prevI = uPathClosed ? ((i - 1 + n) % n) : (i - 1);
                    vec2 prev = texelFetch(uPathTexture, prevI, 0).xy;
                    vec2 dPrev = normalize(a - prev);
                    vec2 dNext = normalize(b - a);
                    vec2 pseudoN = normalize(vec2(-dPrev.y, dPrev.x) + vec2(-dNext.y, dNext.x));
                    side = dot(p - a, pseudoN);
                } else {
                    vec2 d = ab / segLen;
                    side = d.x * (p.y - a.y) - d.y * (p.x - a.x);
                }
            } else if (t > 0.999) {
                // At vertex 'b' — use angle-weighted pseudonormal
                if (uPathClosed || i < segCount - 1) {
                    int nextI = uPathClosed ? ((i + 1) % n) : (i + 1);
                    vec2 next = texelFetch(uPathTexture, (nextI + 1) % n, 0).xy;
                    vec2 dPrev = normalize(b - a);
                    vec2 dNext = normalize(next - b);
                    vec2 pseudoN = normalize(vec2(-dPrev.y, dPrev.x) + vec2(-dNext.y, dNext.x));
                    side = dot(p - b, pseudoN);
                } else {
                    vec2 d = ab / segLen;
                    side = d.x * (p.y - a.y) - d.y * (p.x - a.x);
                }
            } else {
                vec2 d = ab / segLen;
                side = d.x * (p.y - a.y) - d.y * (p.x - a.x);
            }

            signedDist = (side >= 0.0) ? dist : -dist;
            perimPos = (cumLen + t * segLen) / uPathTotalLength;
        }
        cumLen += segLen;
    }

    return vec2(signedDist, perimPos);
}

void main() {
    float d, perimPos, totalP;
    vec2 halfSize;

    if (uPathSource == 0) {
        halfSize = uRectSize * 0.5;
        d = sdRoundedBox(vPos, halfSize, uBorderRadius);
        perimPos = getPerimeterPos(vPos, halfSize, uBorderRadius);
        totalP = getPerimeterLength(uRectSize, uBorderRadius);
    } else {
        vec2 result = sdPolyline(vPos);
        d = result.x;
        perimPos = result.y;
        totalP = uPathTotalLength;
    }

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

    vec3 color;
    if (uColorMode == 0) {
        color = uPrimaryColor.rgb;
    } else if (uColorMode == 1) {
        float smoothT = 1.0 - abs(2.0 * perimPos - 1.0);
        color = mix(uPrimaryColor.rgb, uSecondaryColor.rgb, smoothT);
    } else if (uColorMode == 2) {
        color = hsv2rgb(vec3(perimPos, 0.8, 1.0));
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
