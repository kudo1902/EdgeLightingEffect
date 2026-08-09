precision highp float;

// Debug: one marker glyph, drawn once per thing being marked by
// DebugRenderer - colour stops, arc start/end bounds, segment positions.
// Each gets its own shape so the three families stay apart at a glance:
//
//   uMarkerShape 0 = disc     - a colour stop, sitting on the perimeter.
//   uMarkerShape 1 = chevron  - an arc bound. The C++ side rotates it to
//                               point INTO the lit span, so a pair reads as
//                               "the arc runs between these two".
//   uMarkerShape 2 = diamond  - a segment's centre position.
//
// The vertex data is a unit quad in [-1, +1]; the C++ side scales the model
// matrix by the desired marker radius (and rotates it for the chevron), so
// vPos here is always in glyph space. Every shape resolves to a signed
// distance `d` (negative inside, 0 on the border) and shares one fill / rim /
// anti-alias tail from there.

in vec2 vPos;
out vec4 fragColor;

uniform vec4 uMarkerColor;
uniform int uMarkerShape;

const int SHAPE_DISC = 0;
const int SHAPE_CHEVRON = 1;
const int SHAPE_DIAMOND = 2;

/// Approximate signed distance to an isoceles triangle pointing at +x, as the
/// max of the three edge half-planes. Not exact off-shape, but the error is
/// well under a pixel near the border, which is all the AA tail reads.
float chevronDistance(vec2 p) {
    const vec2 apex = vec2(1.0, 0.0);
    // Outward normals of the two slanted edges (apex -> base corners).
    const vec2 n0 = normalize(vec2(0.8, 1.7));
    const vec2 n1 = normalize(vec2(0.8, -1.7));
    float slanted = max(dot(p - apex, n0), dot(p - apex, n1));
    float base = -(p.x + 0.7);
    return max(slanted, base);
}

void main() {
    float d;
    if (uMarkerShape == SHAPE_CHEVRON) {
        d = chevronDistance(vPos);
    } else if (uMarkerShape == SHAPE_DIAMOND) {
        // |x| + |y| = 1 scaled by cos(45 deg) to approximate true distance.
        d = (abs(vPos.x) + abs(vPos.y) - 1.0) * 0.7071;
    } else {
        d = length(vPos) - 1.0;
    }

    // One pixel of screen-space falloff, so every glyph anti-aliases at
    // whatever size the C++ side scaled it to.
    float aa = max(fwidth(d), 1e-4);
    float alpha = 1.0 - smoothstep(-aa, aa, d);
    if (alpha <= 0.0) discard;

    // Thin white rim just inside the border so the glyph reads on any
    // background - including a stop marker sitting on its own colour.
    float rim = smoothstep(-0.22, -0.08, d);
    vec3 rgb = mix(uMarkerColor.rgb, vec3(1.0), rim);
    fragColor = vec4(rgb, alpha * uMarkerColor.a);
}
