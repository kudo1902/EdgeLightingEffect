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
    float footprint = 1.0 / uAssembleScale;
    vec2  lo = uAssembleOrigin + floor(gl_FragCoord.xy) * footprint;
    vec2  hi = lo + vec2(footprint);
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
