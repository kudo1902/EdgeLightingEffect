precision highp float;

// ---------------------------------------------------------------------------
// The segment table's fill (pass 0f).
//
// The glow coverage table (neon-glow-cover.frag) holds, per piece of the
// emitter, the arcs' coverage on .r / .g and the segments' on .b / .a. An
// arc's coverage has edges as sharp as its 14 px feather, so the table is as
// wide as the arcs need (NeonRenderer::GetGlowCoverWidth). A segment is a
// Gaussian bell whose standard deviation is a third of its length - tens to
// hundreds of px - and baking its channels at the arcs' width integrated
// every bell at four to eight times the columns it needs, which made the bake
// the largest pass of every frame that moved a segment.
//
// So the segments are baked into a narrow table of their own (the bake with
// uBakeTarget 1, its own layout), and this copies them into the main table's
// .b / .a, under a colour mask that leaves .r / .g alone: for each texel of
// a piece, decode what the MAIN table's texel holds - a straight's projection
// and distance, or a corner's offset from its centre - through the main
// layout's inverse map, find where the segment table holds the same position
// through ITS layout's forward map, and take one linear fetch there. So
// neon.frag still reads one texel per piece, unchanged, and pays nothing for
// the split; the cost is this pass, on the frames a segment moved.
//
// Both maps are neon-pieces.glsl's (NEON_GLOW_COVER_FILL compiles both
// halves), with the same texel decode as the bake (glowCoverTexel), so this
// places a texel exactly where the bake does. Lengths are fractions of the
// perimeter, as the bake's.
//
// The value is copied ENCODED, c / (1 + c), and filtered so, as neon.frag
// filters the main table: the segment table's columns are a small fraction
// of a bell wide, so the curvature that filtering in the encoded space adds
// is far below a level.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform sampler2D uSegCover;       ///< The segment table, encoded segment (halo, bloom) on .r / .g.
uniform vec4  uGlowCoverLayout;    ///< The MAIN table's layout - the texel being written.
uniform vec2  uGlowCoverSplit;     ///< The main table's split.
uniform vec4  uSegCoverLayout;     ///< The segment table's layout - where the value is read.
uniform vec2  uSegCoverSplit;      ///< The segment table's split.
uniform float uHaloWidth;          ///< kh as a fraction of the perimeter, as the bake's.
uniform vec2  uStraightSize;       ///< The horizontal and vertical straights' lengths, as the bake's.
uniform float uRadius;             ///< The corner radius, as the bake's.

void main() {
    float kh = uHaloWidth;
    float r  = uRadius;
    int   band;
    float row;
    float inner;
    float split;
    glowCoverTexel(gl_FragCoord.xy, uGlowCoverSplit, uGlowCoverLayout, band, row, inner, split);
    float innerSeg = glowCoverInner(band, uSegCoverSplit);

    // Zero where the bake writes zero: a straight with no length (a circle's)
    // and a corner with no radius.
    vec2 segs = vec2(0.0);
    if (gl_FragCoord.x < split) {
        float len = (band == GLOW_COVER_BAND_NEG_X || band == GLOW_COVER_BAND_POS_X) ? uStraightSize.y
                                                                                     : uStraightSize.x;
        if (len > 0.0) {
            vec2 xa = glowCoverStraightAt(gl_FragCoord.x, row, len, kh, inner, uGlowCoverLayout);
            segs    = textureLod(uSegCover, glowCoverStraightUV(band, xa.x, len, xa.y, kh, innerSeg, uSegCoverLayout),
                                 0.0).rg;
        }
    } else if (r > 0.0) {
        // Corner block b is in band b, in both tables.
        vec2 w = glowCoverCornerAt(gl_FragCoord.x - split, row, r, kh, inner, uGlowCoverLayout);
        segs   = textureLod(uSegCover, glowCoverCornerUV(band, w, r, kh, innerSeg, uSegCoverLayout), 0.0).rg;
    }
    fragColor = vec4(0.0, 0.0, segs);
}
