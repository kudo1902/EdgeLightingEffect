// ---------------------------------------------------------------------------
// The gather pass (pass 1a)
// ---------------------------------------------------------------------------
//
// Below NeonConfig::resolutionScale 1.0 the perimeter gather - the loop that
// is ~95% of the inline neon's cost - runs ONCE, alone, into NeonRenderer's
// gather buffer, at its own coarse scale (GetGatherScale in neon-renderer.cpp).
// The NEON_READS_GATHER variant of neon.frag then shades everything else from
// what this writes: pass 1 at resolutionScale, and the edge ring at full
// resolution. At 1.0 the same happens whenever the renderer splits the gather
// out (SplitsGatherAtFullRes), with the ring's program shading the whole quad;
// otherwise this program is never built and neon.frag runs the same
// gatherPerimeter inline. See docs/neon-resolution-scale-plan.md section 13
// and docs/neon-perf-review.md section 10.
//
// precision, vPos, the shape uniforms, the segment block and the loop itself
// are in neon-common.glsl, which is injected ahead of this file. What is here
// is only what makes this a PASS: where the results go, and in what encoding.
//
// NO CULLS, unlike neon.frag. Its one-sided cut and its cutoffs discard; this
// pass must not. Its texels are read by passes at OTHER resolutions - pass 1
// and the edge ring - whose own culls sit at their own guard distances, and a
// texel culled here would hand their bilinear read a black hue just inside a
// boundary they still draw. So the gather runs over its whole quad
// (setupRingGeometry), which pass 1's geometry already bounds, and the cut is
// drawn by the passes that shade.

// Both outputs get an explicit location because GLSL ES 3.0 requires that once
// there is more than one.
layout(location = 0) out vec4 oGather;
layout(location = 1) out vec4 oGatherSeg;

void main() {
    PerimeterGather gather = gatherPerimeter(vPos);

    // THE GATHER BUFFER: the loop's four results, for the passes that shade
    // from it to read back instead of running it. A coarse grid can carry them
    // where it cannot carry the filament because every one is a smooth
    // Lorentzian-weighted mean over the whole perimeter. See
    // docs/neon-resolution-scale-proposal.md.
    //
    // The buffer is RGBA16F where the driver renders to it and RGBA8 where
    // not, so each value has to fit [0, 1] for the fallback. The hues already
    // do - they are weighted means of stop colours - and are clamped only so a
    // stop authored above 1 cannot wrap. The coverages do not: arc intensity
    // and segment boost both fold into them unbounded. c / (1 + c) maps
    // [0, inf) onto [0, 1) monotonically, is exact at 0, and inverts as
    // e / (1 - e) - neon.frag's NEON_READS_GATHER read is the other half of
    // this pair, and the two have to change together. A fully lit ring's 1.0
    // stores as 0.5.
    //
    // Location 1 lands only when the gather buffer has a second attachment,
    // which the renderer gives it only when there are segments. Without one
    // the write is dropped by GL, and segHue and segCover are 0 anyway.
    oGather    = vec4(clamp(gather.hue,    0.0, 1.0), gather.arcCover / (1.0 + gather.arcCover));
    oGatherSeg = vec4(clamp(gather.segHue, 0.0, 1.0), gather.segCover / (1.0 + gather.segCover));
}
