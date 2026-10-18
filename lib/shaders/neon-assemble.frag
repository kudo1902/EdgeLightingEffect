// ---------------------------------------------------------------------------
// The shared buffer's light (pass 1s, NeonRenderer::COMPOSITE_KEY), assembled
// from the neon's two shared parts - nothing is shaded again.
//
// Each texel of the shared buffer covers a square of caller pixels 1 / s wide:
// texel q covers [uAssembleOrigin + q / s, + 1 / s] in caller px. Its value is
// the screen's light averaged over that square - every full-resolution pixel
// the square touches, weighted by how much of it the square covers - read
// through neonLight (neon-light.glsl), so the ring's pixels come from the ring
// atlas and the rest from the glow with the blit's edge mask. A box filter of
// the screen's own light, before dither: at s 1 the square is one pixel and
// the texel IS that pixel's light; below 1 a thin line keeps its brightness
// and width instead of being shaded again at s.
//
// Premultiplied, blended over the fill the pass drew first.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform float uAssembleScale;  ///< s, the shared buffer's texels per caller px.
uniform vec2  uAssembleOrigin; ///< Caller px of texel (0, 0)'s lower-left corner.

void main() {
    // At s 1 the square is one whole pixel (the origin is a whole pixel too):
    // its light is the one read at its centre, with weight 1 - what the loop
    // below computes there, without the loop or the test before it, which
    // cost a frame at s 1 up to 1.16x (docs/history/neon-shared-layer-perf-review.md).
    if (uAssembleScale >= 1.0)
    {
        fragColor = neonLight(uAssembleOrigin + floor(gl_FragCoord.xy) + 0.5, 1.0);
        return;
    }
    float footprint = 1.0 / uAssembleScale;
    vec2  lo = uAssembleOrigin + floor(gl_FragCoord.xy) * footprint;
    vec2  hi = lo + vec2(footprint);

    // The glow alone, unmasked, over a square that touches no ring pixel:
    // one bilinear read at the square's centre instead of one per pixel. The
    // glow is a bilinear upsample of a smooth buffer, so the average of its
    // reads at the square's pixel centres differs from the read at its centre
    // only by the buffer's second difference across a texel - a fraction of a
    // level a square's width away from the ring, which holds every steep part
    // of the light (right beside it, on a hairline, it reached 2). Not
    // where the edge mask is live: its ramps are a full-res pixel wide, and
    // only the per-pixel average is their box filter. Tested, and read, where
    // neonLight would read - the square moved onto the viewport - so a square
    // past the viewport's edge takes the edge's light either way.
    vec2 clo = neonLightClampPx(lo);
    vec2 chi = neonLightClampPx(hi);
    if (!neonEdgeMaskLive(uLightGlowSide, uLightInsideCutoff, uLightOutsideCutoff) &&
        !neonLightSquareTouchesRing(clo - uLightCenter - vec2(footprint), chi - uLightCenter + vec2(footprint)))
    {
        vec2 uv = (neonLightClampPx(0.5 * (lo + hi)) - uLightGlowOrigin) / uLightGlowSize;
        bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
        fragColor = inside ? textureLod(uLightGlow, uv, 0.0) : vec4(0.0);
        return;
    }
    ivec2 j0 = ivec2(floor(lo));
    ivec2 j1 = ivec2(ceil(hi)) - 1;
    vec4  sum = vec4(0.0);
    for (int y = j0.y; y <= j1.y; ++y)
    {
        float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));
        for (int x = j0.x; x <= j1.x; ++x)
        {
            float wx = min(hi.x, float(x + 1)) - max(lo.x, float(x));
            sum += neonLight(vec2(float(x), float(y)) + 0.5, 1.0) * (wx * wy);
        }
    }
    fragColor = sum * (uAssembleScale * uAssembleScale);
}
