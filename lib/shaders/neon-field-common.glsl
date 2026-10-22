// ---------------------------------------------------------------------------
// The hue-invariant field's composite: what pass 1c's (neon-field.frag, into
// the reduced buffer) and the edge ring's (neon-ring-field.frag, onto the
// caller's framebuffer) share.
//
// Under a rotating hue - the library's default - only two things in neon.frag
// move from frame to frame: the gathered hue, and the colour-stop alpha read at
// the fragment's perimeter position. Everything else - the filament, the halo,
// the bloom, each piece's coverage, the fade - is a function of the config and
// the geometry, and enters the colour linearly:
//
//     neon.frag   = tonemap( col * Fa + segColHue * Fs )   (nothing after the grade)
//
// where col and segColHue are the gathered arc and segment hues and Fa and Fs
// are time-invariant. So NeonRenderer bakes both once - neon-field-bake.frag,
// the same shading with the arc hue on red and the segment hue on green,
// writing the pre-tone-map result - and a frame becomes the gather
// plus a composite: read the field, read the gathered hues, tone-map. The tone
// map is neon.frag's own function, neonToneMap from neon-grade.glsl. Without
// segments Fs is 0 and the field is one channel (R16F); with them it is two
// (RG16F), and uFieldSegments says which (D3).
//
// Both fields are PACKED the same way (NeonRenderer::FieldLayout): each covers
// an axis-aligned annulus - a box of texels minus a hole - so its texels are
// four strips, the bottom and the top as they are and the left and the right
// transposed into rows, cut into columns side by side. fieldStripTexel finds a
// box texel's place in them.
//
// Not a shader: it has no main(). shaders.h.in injects it into the two
// composites, after neon-grade.glsl and neon-sdf.glsl, so both read the
// field's inputs, find its texels and tone-map them one way.
// ---------------------------------------------------------------------------

precision highp float;

in vec2 vPos;
out vec4 fragColor;

// HIGHP, every sampler here, not GLSL ES's lowp default (I97): the field is
// pre-tone-map brightness, which nothing bounds by the 2 a lowp lookup may
// return, and the gather's hues come back at its 2^-8.
uniform highp sampler2D uField; ///< Fa (.r) and, with segments, Fs (.g): R16F or RG16F; NEAREST.

// The one-sided cut and the cutoffs, as the shading they stand in for takes
// them (UploadEdgeMaskUniforms): the ring's at scale 1.0, pass 1b's at the
// reduced scale; a disabled cutoff at CUTOFF_NEUTRALISED.
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
uniform highp sampler2D uGather;    ///< The gather buffer: the hue in .rgb.
uniform highp sampler2D uGatherSeg; ///< Its second attachment: the segment hue in .rgb. Read only when uFieldSegments is set.
uniform int       uFieldSegments; ///< 1 when the field carries Fs in .g (the config has segments), else 0.
uniform float     uFieldGain;   ///< I / I0: the neon's intensity over the one the field was baked at; exactly 1 but while it moves (I56). Scales Fa only - the segments do not take the intensity.
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

// The field texel @p field under the hues gathered at vPos, tone-mapped:
// neon.frag's tone map - the same function, from neon-grade.glsl. The gain
// scales a field baked at another intensity to this one; at exactly 1 the
// product is fa's own bits, so a held config composites what it always did.
// The segments' term takes no gain: neon.frag's segment emission does not
// read the intensity (only the bloom's reach does, which the arc term's
// gain ignores too). Branching on a uniform, so a config with no segments
// composites exactly what it did before the field had a second channel.
vec3 fieldShade(vec2 field) {
    vec2 gatherUV = vPos * uGatherUVScale + uGatherUVOffset;
    vec3 col = textureLod(uGather, gatherUV, 0.0).rgb;

    vec3 linear = col * (field.r * uFieldGain);
    if (uFieldSegments != 0) {
        vec3 segColHue = textureLod(uGatherSeg, gatherUV, 0.0).rgb;
        linear += segColHue * field.g;
    }
    return neonToneMap(linear);
}

// The atlas texel holding box texel @p q of a field packed with @p hole -
// [x0, x1) x [y0, y1) as x0, y0, x1, y1, the box texels the composite never
// draws - and @p sides, the side rows' first atlas row and how many each atlas
// column holds: bottom strip, top strip stacked on it, then the side strips
// transposed - each box column beside the hole an atlas row, the left strip's
// then the right's - in columns side by side. The layout
// NeonRenderer::computeRingFieldLayout / computeGlowFieldLayout build and
// drawFieldStrips bakes - change them together. The hole is conservative, so
// every texel a composite draws is in a strip.
ivec2 fieldStripTexel(ivec2 q, ivec4 hole, ivec2 sides) {
    if (q.y < hole.y)
    {
        return q;
    }
    if (q.y >= hole.w)
    {
        return ivec2(q.x, q.y - hole.w + hole.y);
    }
    // The column by a FLOAT division, which is exact here - (row + 0.5) /
    // sides.y lies at least half a row from a column's edge, and the
    // division's error at any atlas size is orders of magnitude under that -
    // where an int division is a long emulated sequence: with it pass 1c ran
    // 1.5-2x as long at 4K on an Apple M2 Pro.
    int row    = (q.x < hole.x) ? q.x : hole.x + q.x - hole.z;
    int column = int((float(row) + 0.5) / float(sides.y));
    return ivec2(column * (hole.w - hole.y) + q.y - hole.y, sides.x + row - column * sides.y);
}
