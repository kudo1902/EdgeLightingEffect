// ---------------------------------------------------------------------------
// The neon glow's EDGE MASK: the one-sided cut (NeonConfig::glowSide) and the
// inside / outside cutoffs, applied as COVERAGE after the tone map. One copy,
// shared by every program that applies it - the blit (neon-blit.frag), the
// edge ring (neon.frag with uBlitOwnsCut 0) and the ring field's composite
// (neon-field.frag with NEON_FIELD_RING) - and by any layer that reads the
// glow product, which holds the glow BEFORE this mask (docs/
// resource-sharing-plan.md 3.5).
//
// Not a shader: it has no main(). shaders.h.in injects it after neon-sdf.glsl
// (sdRoundBox, sdRoundBoxFwidth, bandOuterDistance, bandInnerDistance) and the
// neon tuning header (CUTOFF_NEUTRALISED). It reads no uniform: each program
// passes its own, in its own px - the ring and the blit at full resolution,
// pass 1b's discards at the reduced scale.
//
// Why COVERAGE, after the grade: above the tone map a half-covered pixel came
// out near full brightness and its alpha occluded at ~0.94 instead of 0.5;
// below it the ramp is the box filter and registers with the opaque fill.
// See docs/glow-side-comparison.md.
//
// The factors are separate functions, not only their product: the programs
// multiply them into their colour in different orders - the ring and the
// field composite one at a time, the blit as one product - and a float
// product is not associative, so folding them would move a last bit.
// ---------------------------------------------------------------------------
precision highp float;

// The ordinals of EdgeLighting::GlowSide, which the renderer casts straight to
// an int: one numbering, kept in step by hand. Renumber the enum and these
// follow.
#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// Where a fragment sits against the mask's edges, and how wide each ramp is -
// everything the three factors read, and what neon.frag's discards and bloom
// fade read too.
struct NeonEdge
{
    float d;        // Signed distance from the rect edge, px, negative inside.
    float sideSoft; // The cut's TOTAL feather, floored at the pixel width.
    float sideBack; // How far the cut's ramp reaches back across the line: half a pixel.
    float inHalf;   // Half the inside cutoff's ramp: its softness floored at the pixel width, halved.
    float outHalf;  // The same, outward.
    float inMid;    // Where the inside ramp is centred: the cutoff plus half its softness.
    float outMid;   // The same, outward.
    float dIn;      // Distance from the inside ramp's midpoint, positive in the band; CUTOFF_NEUTRALISED when the cut culls that side.
    float dOut;     // Distance from the outside ramp's midpoint, negative in the band; -CUTOFF_NEUTRALISED when the cut culls that side.
};

// The mask's edges for a fragment at rect-local p, distance d (sdRoundBox) from
// the rect of half-size halfSize and corner radius r, with aa the width of
// one DESTINATION pixel in p's units (sdRoundBoxFwidth, scaled to the pixel
// the opaque fill registers with).
//
// THE CUT is anchored at the line and feathers INTO the lit side, never back
// past the fill: its ramp ends half a pixel across the line, exactly as far as
// the fill's own box filter reaches (black-rect.frag), so the two share one
// ramp. Its feather is floored at the pixel - the floor is what antialiases it.
//
// THE CUTOFFS: the band is [-inCutoff, +outCutoff]; each side's feather STARTS
// at that boundary and runs its softness beyond it. Each ramp is centred on the
// midpoint of its fade, softness / 2 past the cutoff, over the softness floored
// at one pixel - halved, because smoothstep(-w, w, x) spans 2w. Below the floor
// the ramp stays centred on the requested fade, so at softness 0 the edge's 50%
// point is the cutoff itself at every resolution scale. The boundaries are
// measured against the offset rect (bandOuterDistance), so a cornerRadius-0
// band keeps square corners.
//
// A CUTOFF ON THE SIDE glowSide ALREADY CULLS IS NEUTRALISED - handed the huge
// distance a disabled one reaches through its sentinel - so it never binds
// there. It was not harmless: on the scaled path it ate the blit's guard band
// (a dark seam), and at full res it squared the shared boundary.
// NeonRenderer::setupGeometry mirrors this for the quad's hole. A disabled
// side arrives as a huge sentinel and saturates its smoothstep to exactly 1.
//
// neonEdgeSide fills the cut's half (d, sideSoft, sideBack) and leaves both
// cutoffs NEUTRALISED - each factor exactly 1 - so an edge built by it alone
// is whole, not half-filled; neonEdge, or neonEdgeCutoffs after it, sets the
// cutoffs for real.
NeonEdge neonEdgeSide(float d, float aa, float sideSoftness)
{
    NeonEdge e;
    e.d        = d;
    e.sideSoft = max(sideSoftness, aa);
    e.sideBack = 0.5 * aa;
    e.inHalf   = 0.5 * aa;
    e.outHalf  = 0.5 * aa;
    e.inMid    = CUTOFF_NEUTRALISED;
    e.outMid   = CUTOFF_NEUTRALISED;
    e.dIn      = CUTOFF_NEUTRALISED;
    e.dOut     = -CUTOFF_NEUTRALISED;
    return e;
}

// The cutoffs' half of neonEdge: their ramps, midpoints and distances, filled
// into @p e, which neonEdgeSide began. A side that is not LIVE keeps the
// neutral distance - no distance computed - and its factor is exactly 1:
// neonEdge passes the sides glowSide does not cull, as neon.frag always has (a
// disabled side's sentinel saturates its ramp through the arithmetic);
// neonEdgeMask passes only the sides that bind, so the blit computes no
// distance a dead side would throw away.
void neonEdgeCutoffs(inout NeonEdge e, vec2 p, vec2 halfSize, float r, float aa, bool inLive, bool outLive,
                     float inCutoff, float inSoftness, float outCutoff, float outSoftness)
{
    e.inHalf  = 0.5 * max(inSoftness,  aa);
    e.outHalf = 0.5 * max(outSoftness, aa);
    e.inMid   = inCutoff  + 0.5 * max(inSoftness,  0.0);
    e.outMid  = outCutoff + 0.5 * max(outSoftness, 0.0);
    if (outLive)
    {
        e.dOut = bandOuterDistance(p, e.d, halfSize, r, e.outMid);
    }
    else
    {
        e.dOut = -CUTOFF_NEUTRALISED;
    }
    if (inLive)
    {
        e.dIn = bandInnerDistance(e.d, e.inMid);
    }
    else
    {
        e.dIn = CUTOFF_NEUTRALISED;
    }
}

// The whole edge: the cut's half (neonEdgeSide) and the cutoffs'
// (neonEdgeCutoffs), neutralising the side glowSide culls.
NeonEdge neonEdge(vec2 p, float d, vec2 halfSize, float r, float aa, int glowSide, float sideSoftness,
                  float inCutoff, float inSoftness, float outCutoff, float outSoftness)
{
    NeonEdge e = neonEdgeSide(d, aa, sideSoftness);
    neonEdgeCutoffs(e, p, halfSize, r, aa, glowSide != GLOW_SIDE_OUTSIDE, glowSide != GLOW_SIDE_INSIDE,
                    inCutoff, inSoftness, outCutoff, outSoftness);
    return e;
}

// The one-sided cut's coverage: 1 on the lit side, ramping to 0 half a pixel
// past the line. Exactly 1 for GLOW_SIDE_BOTH. Written as
// 1.0 - smoothstep(lo, hi, d) for the INSIDE arm rather than smoothstep(hi,
// lo, d): edge0 > edge1 is undefined in GLSL.
float neonSideCut(NeonEdge e, int glowSide)
{
    if (glowSide == GLOW_SIDE_INSIDE)
    {
        return 1.0 - smoothstep(e.sideBack - e.sideSoft, e.sideBack, e.d);
    }
    if (glowSide == GLOW_SIDE_OUTSIDE)
    {
        return smoothstep(-e.sideBack, e.sideSoft - e.sideBack, e.d);
    }
    return 1.0;
}

// The inside cutoff's coverage: 0 inside the shrunk rect, 1 in the band.
float neonInsideCutoff(NeonEdge e)
{
    return smoothstep(-e.inHalf, e.inHalf, e.dIn);
}

// The outside cutoff's coverage: 1 in the band, 0 outside the grown rect.
float neonOutsideCutoff(NeonEdge e)
{
    return 1.0 - smoothstep(-e.outHalf, e.outHalf, e.dOut);
}

// Whether the mask can be anything but exactly 1 for these uniforms: a
// one-sided glow, or a cutoff enabled on either side. A function of uniforms
// alone, so a branch on it is uniform control flow - the only place
// sdRoundBoxFwidth's derivative may sit below a branch - and a config with
// neither (the default) pays only the test.
bool neonEdgeMaskLive(int glowSide, float inCutoff, float outCutoff)
{
    return glowSide != GLOW_SIDE_BOTH || inCutoff < 0.5 * CUTOFF_NEUTRALISED ||
           outCutoff < 0.5 * CUTOFF_NEUTRALISED;
}

// The whole mask at rect-local full-res p - the cut times both cutoffs, in that
// order - for a program on the caller's framebuffer: the blit, and a layer
// that reads the glow product and draws at full resolution. Exactly 1 when
// nothing is live. Call it in uniform control flow (it takes a derivative).
//
// The cutoffs only where one binds - enabled, on a side the cut does not
// cull - and of those only the side that binds: a dead side's factor is an
// exact 1, so it is not computed. A one-sided glow without cutoffs pays the
// cut alone, and one cutoff pays for its own side, as the blit did before
// this chunk (its branches were measured a straight saving). Every condition
// is of uniforms, so control flow stays uniform.
float neonEdgeMask(vec2 p, vec2 halfSize, float r, int glowSide, float sideSoftness, float inCutoff,
                   float inSoftness, float outCutoff, float outSoftness)
{
    if (!neonEdgeMaskLive(glowSide, inCutoff, outCutoff))
    {
        return 1.0;
    }
    float d  = sdRoundBox(p, halfSize, r);
    float aa = max(sdRoundBoxFwidth(p, halfSize, r), 1e-6);
    NeonEdge e = neonEdgeSide(d, aa, sideSoftness);
    float mask = neonSideCut(e, glowSide);
    bool cutIn  = glowSide != GLOW_SIDE_OUTSIDE && inCutoff  < 0.5 * CUTOFF_NEUTRALISED;
    bool cutOut = glowSide != GLOW_SIDE_INSIDE  && outCutoff < 0.5 * CUTOFF_NEUTRALISED;
    if (cutIn || cutOut)
    {
        neonEdgeCutoffs(e, p, halfSize, r, aa, cutIn, cutOut, inCutoff, inSoftness, outCutoff, outSoftness);
        mask *= neonInsideCutoff(e);
        mask *= neonOutsideCutoff(e);
    }
    return mask;
}
