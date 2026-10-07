precision highp float;

// ---------------------------------------------------------------------------
// The hue-invariant field's composite (pass 1c).
//
// Under a rotating hue - the library's default - only two things in neon.frag
// move from frame to frame: the gathered hue, and the colour-stop alpha read at
// the fragment's perimeter position. Everything else - the filament, the halo,
// the bloom, each piece's coverage, the fade - is a function of the config and
// the geometry, and enters the colour linearly:
//
//     neon.frag   = tonemap( col * Fa )        (no segments, nothing after the grade)
//
// where col is the gathered hue and Fa is time-invariant. So NeonRenderer
// bakes Fa once - neon.frag compiled with NEON_FIELD_BAKE, which sets col to 1
// and writes the pre-tone-map result - and a frame becomes the gather plus
// this: read the field, read the gathered hue, tone-map. The tone map is
// neon.frag's own function, neonToneMap from neon-grade.glsl.
//
// "Nothing after the grade": at 1.0 neon.frag multiplies the one-sided cut and
// the cutoffs in after its tone map, so a config with either at 1.0 is not
// eligible (IsFieldEligible) and shades directly; below 1.0 the blit applies
// both, so the shading never does. The field is one channel.
//
// At 1.0 this draws pass 1's glow quad onto the caller's framebuffer, on every
// frame. Below 1.0 it draws pass 1b's into the reduced buffer, unblended, under
// a rotating hue - the only frames below 1.0 that redraw pass 1b with nothing
// but the time moved.
//
// The texel a fragment wants is floor((vPos - uFieldOrigin) * uFieldTexelScale),
// rect-local, and so independent of where the host's viewport starts. At 1.0
// the field covers the glow quad's box with its texel centres on the
// viewport's pixel centres, and the scale is exactly 1. Below it the field IS
// the reduced buffer's region and grid, vPos is in scaled px, and the scale is
// that buffer's texels per scaled px - 1 but where the region is the whole
// viewport, whose texel count is truncated. Not gl_FragCoord, which would do
// below 1.0: a select between the two cost the 1.0 composite ~30% on an AMD
// Radeon Pro 5300M. A texel neon.frag discarded holds 0, which composites to
// nothing, as the discard did.
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uField;       ///< Fa, R16F; NEAREST.
uniform vec2      uFieldOrigin; ///< vPos of the field's texel (0, 0) corner.
uniform vec2      uFieldTexelScale; ///< Field texels per vPos unit: 1 at 1.0, the reduced buffer's below.
uniform sampler2D uGather;      ///< The gather buffer: the hue in .rgb.
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

void main() {
    float fa = texelFetch(uField, ivec2(floor((vPos - uFieldOrigin) * uFieldTexelScale)), 0).r;
    vec3 col = textureLod(uGather, vPos * uGatherUVScale + uGatherUVOffset, 0.0).rgb;

    // neon.frag's tone map - the same function, from neon-grade.glsl.
    vec3 result = neonToneMap(col * fa);

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
