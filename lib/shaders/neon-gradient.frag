#version 330 core
precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2 uRectSize;
uniform float uCornerRadius;
uniform float uTime;
uniform float uSpeed;
uniform float uStrokeThickness;

uniform int uColorStopCount;
uniform float uColorStopPositions[16];
uniform vec4 uColorStopColors[16];
uniform int uBlendSpace;

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

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Map any point to a [0,1] perimeter position along the rounded rectangle edge.
float getPerimPosCCW(vec2 p, vec2 halfSize, float r) {
    vec2 c = halfSize - r;
    vec2 ap = abs(p);
    vec2 sp = sign(p);
    vec2 cp;

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

    float w = 2.0 * c.x;
    float h = 2.0 * c.y;
    float arc = 0.5 * 3.1415926535 * r;
    float total = 4.0 * arc + w + h;

    if (abs(cp.x + halfSize.x) < 0.001) {
        float t = (c.y - cp.y) / h;
        return (t * h) / total;
    }
    if (abs(cp.y + halfSize.y) < 0.001) {
        float t = (cp.x + c.x) / w;
        return (h + arc + t * w) / total;
    }
    if (abs(cp.x - halfSize.x) < 0.001) {
        float t = (cp.y + c.y) / h;
        return (h + arc + w + arc + t * h) / total;
    }
    if (abs(cp.y - halfSize.y) < 0.001) {
        float t = (c.x - cp.x) / w;
        return (h + arc + w + arc + h + arc + t * w) / total;
    }

    if (cp.x < 0.0 && cp.y < 0.0) {
        float a = atan(-cp.y - c.y, -cp.x - c.x);
        float t = (a + 3.14159265) / (3.14159265 * 0.5);
        return (t * arc) / total;
    }
    if (cp.x > 0.0 && cp.y < 0.0) {
        float a = atan(-cp.y - c.y, cp.x - c.x);
        float t = (a + 3.14159265 * 0.5) / (3.14159265 * 0.5);
        return (h + arc + w + t * arc) / total;
    }
    if (cp.x > 0.0 && cp.y > 0.0) {
        float a = atan(cp.y - c.y, cp.x - c.x);
        float t = a / (3.14159265 * 0.5);
        return (h + arc + w + arc + h + t * arc) / total;
    }
    float a = atan(cp.y - c.y, cp.x + c.x);
    float t = (a - 3.14159265 * 0.5) / (3.14159265 * 0.5);
    return (h + arc + w + arc + h + arc + w + t * arc) / total;
}

// Find the closest point on the perimeter and return its distance from p.
float closestPerimDist(vec2 p, vec2 halfSize, float r, out vec2 cp) {
    vec2 c = halfSize - r;     // core: inner rect before corner arcs start
    vec2 ap = abs(p);
    vec2 sp = sign(p);

    // Corner region: outside both edges of the core
    if (ap.x > c.x && ap.y > c.y) {
        vec2 delta = ap - c;
        float d = length(delta);
        vec2 dir = delta / d;
        // Project onto the corner arc (radius r from corner center c)
        cp = vec2(c.x + dir.x * r, c.y + dir.y * r) * sp;
    }
    // Edge region: outside core on one axis only — closest point is on the
    // corresponding straight edge of the rounded rectangle (at halfSize, not c)
    else if (ap.x > c.x) {
        cp = vec2(halfSize.x, clamp(ap.y, 0.0, c.y)) * sp;
    } else if (ap.y > c.y) {
        cp = vec2(clamp(ap.x, 0.0, c.x), halfSize.y) * sp;
    }
    // Inside core: closest is the nearest straight edge
    else {
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

    vec2 cp;
    float dist = closestPerimDist(vPos, halfSize, uCornerRadius, cp);
    float sd = sdRoundBox(vPos, halfSize, uCornerRadius);
    float lineHalf = max(uStrokeThickness * 0.5, 2.0);
    if (sd > lineHalf + 2.0) {
        fragColor = vec4(0.0);
        return;
    }
    float mask = 1.0 - smoothstep(0.0, lineHalf, dist);
    if (mask < 0.01) {
        fragColor = vec4(0.0);
        return;
    }
    float perimPos = getPerimPosCCW(vPos, halfSize, uCornerRadius);
    float h = fract(perimPos + uTime * uSpeed);
    vec3 col = sampleStops(h);
    fragColor = vec4(col, mask);
}
