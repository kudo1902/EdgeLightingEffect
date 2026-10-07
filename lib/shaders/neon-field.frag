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
// "Nothing after the grade": pass 1b leaves the one-sided cut and the cutoffs
// to the blit (uBlitOwnsCut), so the shading never applies them after its tone
// map and the field is one channel.
//
// This draws pass 1b's glow quad into the reduced buffer - full size at 1.0 -
// unblended, in pass 1b's place, under a rotating hue or a moving intensity
// (IsFieldEligible): the only frames that redraw pass 1b with nothing but the
// time, or the gain, moved.
//
// The texel a fragment wants is floor((vPos - uFieldOrigin) * uFieldTexelScale),
// rect-local, and so independent of where the host's viewport starts. The
// field IS the reduced buffer's region and grid, vPos is in scaled px, and the
// scale is that buffer's texels per scaled px - 1 but where the region is the
// whole viewport, whose texel count is truncated. Not gl_FragCoord: a select
// between the two cost the old full-resolution composite ~30% on an AMD Radeon
// Pro 5300M. A texel neon.frag discarded holds 0, which composites to nothing,
// as the discard did.
//
// NEON_FIELD_RING builds the edge ring's composite instead (at every scale, onto
// the caller's framebuffer, in pass 2c's place). Its field is full resolution
// but holds only the ring, PACKED: the ring is an axis-aligned annulus - a box
// minus a hole (setupRingGeometry) - so its pixels are four strips, stored
// one above the other on the viewport's pixel grid: the bottom and the top as
// they are, the left and the right transposed, so a side strip lies along the
// atlas's width rather than wasting a column of it. A fragment finds its box
// pixel q from vPos, its strip from q against the hole, and its texel from
// the strip's row. The hole is CONSERVATIVE - only box pixels the ring can
// never draw - so every ring fragment lands in a strip. The ring shades with
// scale-1.0 uniforms, so unlike pass 1b it multiplies the one-sided cut and the
// cutoffs in after its tone map; this composite does the same, from the
// pixel's own distance to the edge, so a ring with either still takes its
// field.
// ---------------------------------------------------------------------------

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uField;       ///< Fa, R16F; NEAREST.
#ifdef NEON_FIELD_RING
uniform vec2      uRingLo;      ///< vPos (full-res px) of the ring box's lower-left pixel corner.
uniform vec4      uRingHole;    ///< Box pixels the ring never draws: [x0, x1) x [y0, y1), as x0, y0, x1, y1.
uniform vec2      uRingRows;    ///< First atlas row of the left strip and of the right strip.

// The one-sided cut and the cutoffs, as the ring's own shading takes them
// (uploadNeonUniforms at scale 1.0): full-res px, a disabled cutoff at
// CUTOFF_NEUTRALISED.
#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;
uniform float uInsideCutoffSoftness;
uniform float uOutsideCutoff;
uniform float uOutsideCutoffSoftness;
#else
uniform vec2      uFieldOrigin; ///< vPos of the field's texel (0, 0) corner.
uniform vec2      uFieldTexelScale; ///< Field texels per vPos unit: the reduced buffer's texels per scaled px.
#endif
uniform sampler2D uGather;      ///< The gather buffer: the hue in .rgb.
uniform float     uFieldGain;   ///< I / I0: the neon's intensity over the one the field was baked at; exactly 1 but while it moves (I56).
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

void main() {
#ifdef NEON_FIELD_RING
    // Bottom strip, then the top stacked on it, then the left and the right
    // transposed - the layout computeRingFieldLayout builds and
    // renderRingFieldPass bakes. Keep the three together.
    ivec2 q    = ivec2(floor(vPos - uRingLo));
    ivec4 hole = ivec4(uRingHole);
    ivec2 texel;
    if (q.y < hole.y)
    {
        texel = q;
    }
    else if (q.y >= hole.w)
    {
        texel = ivec2(q.x, q.y - hole.w + hole.y);
    }
    else if (q.x < hole.x)
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.x) + q.x);
    }
    else
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.y) + q.x - hole.z);
    }
    float fa = texelFetch(uField, texel, 0).r;
#else
    float fa = texelFetch(uField, ivec2(floor((vPos - uFieldOrigin) * uFieldTexelScale)), 0).r;
#endif
    vec3 col = textureLod(uGather, vPos * uGatherUVScale + uGatherUVOffset, 0.0).rgb;

    // neon.frag's tone map - the same function, from neon-grade.glsl. The gain
    // scales a field baked at another intensity to this one; at exactly 1 the
    // product is fa's own bits, so a held config composites what it always did.
    vec3 result = neonToneMap(col * (fa * uFieldGain));

#ifdef NEON_FIELD_RING
    // The one-sided cut and the cutoffs, which the ring's own shading applies
    // after its tone map as coverage (neon.frag at scale 1.0, where
    // blitOwnsCut is false): the same expressions, term for term - keep them
    // in step with neon.frag's, and with neon-blit.frag's below 1.0. The field
    // holds Fa from before them (NEON_FIELD_BAKE returns at the grade), and
    // what neon.frag discarded ahead of them is a 0 texel, which composites to
    // nothing. Skipped on a uniform when neither is set: every factor is then
    // exactly 1 (a disabled side's sentinel saturates its smoothstep). The
    // derivative is in uniform control flow, so defined.
    if (uGlowSide != GLOW_SIDE_BOTH || uInsideCutoff < 0.5 * CUTOFF_NEUTRALISED ||
        uOutsideCutoff < 0.5 * CUTOFF_NEUTRALISED)
    {
        vec2  halfSize = uRectSize * 0.5;
        float d        = sdRoundBox(vPos, halfSize, uCornerRadius);
        float sideAA   = max(fwidth(d), 1e-6);
        float sideSoft = max(uGlowSideSoftness, sideAA);
        float sideBack = 0.5 * sideAA;
        float inHalf   = 0.5 * max(uInsideCutoffSoftness,  sideAA);
        float outHalf  = 0.5 * max(uOutsideCutoffSoftness, sideAA);
        float inMid    = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
        float outMid   = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);
        float dOut;
        float dIn;
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            dOut = -CUTOFF_NEUTRALISED;
        }
        else
        {
            dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
        }
        if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            dIn = CUTOFF_NEUTRALISED;
        }
        else
        {
            dIn = bandInnerDistance(d, inMid);
        }
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        }
        else if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            result *= smoothstep(-sideBack, sideSoft - sideBack, d);
        }
        result *= smoothstep(-inHalf, inHalf, dIn);
        result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
    }
#endif

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
