precision highp float;

// Debug: draws a filled disc in uMarkerColor with a thin white ring so the
// marker reads on any background — used to spot each colour-stop position
// on the perimeter (see NeonRenderer::Render, showColorStops).
//
// The vertex data is a unit quad in [-1, +1]; the C++ side scales the model
// matrix by the desired marker radius so vPos here is always in disc space.

in vec2 vPos;
out vec4 fragColor;

uniform vec4 uMarkerColor;

void main() {
    float d = length(vPos);
    if (d > 1.0) discard;

    // 0.0-0.85 → solid stop colour
    // 0.85-0.95 → white ring
    // 0.95-1.0 → alpha fade to zero (anti-alias)
    float ring  = smoothstep(0.85, 0.9, d);
    float alpha = 1.0 - smoothstep(0.95, 1.0, d);
    vec3  rgb   = mix(uMarkerColor.rgb, vec3(1.0), ring);
    fragColor = vec4(rgb, alpha);
}
