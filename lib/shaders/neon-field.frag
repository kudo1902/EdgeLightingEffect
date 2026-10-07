precision highp float;

// ---------------------------------------------------------------------------
// The hue-invariant field's composite (pass 1c, scale 1.0).
//
// Under a rotating hue - the library's default - only two things in neon.frag
// move from frame to frame: the gathered hue, and the colour-stop alpha read at
// the fragment's perimeter position. Everything else - the filament, the halo,
// the bloom, each piece's coverage, the fade - is a function of the config and
// the geometry, and enters the colour linearly:
//
//     neon.frag   = mask * tonemap( col * Fa )        (no segments)
//
// where col is the gathered hue and Fa and mask are time-invariant. So
// NeonRenderer bakes Fa (.r) and, when the config has one, the mask (.g) once
// - neon.frag compiled with NEON_FIELD_BAKE, which sets col to 1 and writes the
// pre-tone-map result and the masks applied to 1.0 - and a frame becomes the
// gather plus this: read the field, read the gathered hue, tone-map. The tone
// map is neon.frag's own function, neonToneMap from neon-grade.glsl.
//
// The field covers the glow quad's box at full resolution with its texel
// centres on the viewport's pixel centres, so the texel a fragment wants is
// floor(vPos - uFieldOrigin): rect-local, and so independent of where the
// host's viewport starts. A texel neon.frag discarded holds 0, which composites
// to nothing, as the discard did.
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uField;       ///< .r Fa, .g the mask when uFieldHasMask; NEAREST.
uniform vec2      uFieldOrigin; ///< Rect-local px of the field's texel (0, 0) corner.
uniform int       uFieldHasMask;
uniform sampler2D uGather;      ///< The gather buffer: the hue in .rgb.
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

void main() {
    vec2 field = texelFetch(uField, ivec2(floor(vPos - uFieldOrigin)), 0).rg;
    vec3 col   = textureLod(uGather, vPos * uGatherUVScale + uGatherUVOffset, 0.0).rgb;

    // neon.frag's tone map - the same function, from neon-grade.glsl.
    vec3 result = neonToneMap(col * field.r);
    // The one-sided cut and the cutoffs, which neon.frag applies after the tone
    // map. An R16F field has no .g - it reads 0 - so it is read only when the
    // field carries one.
    if (uFieldHasMask != 0)
    {
        result *= field.g;
    }

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
