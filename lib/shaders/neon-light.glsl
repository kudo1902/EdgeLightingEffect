// ---------------------------------------------------------------------------
// The neon's light, read from its two PUBLIC products - the glow
// (NeonRenderer.Glow) and the ring atlas (NeonRenderer.Ring) - exactly as the
// neon composites them onto the screen: the edge ring's own pixels from the
// atlas, every other pixel from the glow with the edge mask applied, as the
// blit applies it. Premultiplied, over transparent black, UNDITHERED, without
// the opaque fill (docs/neon-shared-layer-plan.md, step 2).
//
// Not a shader: injected after the tuning header, neon-sdf.glsl, neon-mask.glsl
// and neon-ring.glsl - into the neon's own assemble pass (neon-assemble.frag),
// and available to a host as ShaderSource::NEON_LIGHT_GLSL, that whole chain in
// one string. The uniforms are set by NeonRenderer::UploadSharedLight from
// NeonRenderer::GetSharedLight, so a reader and the neon cannot disagree on them.
// ---------------------------------------------------------------------------

precision highp float;

uniform sampler2D uLightGlow;        ///< NeonRenderer.Glow: bilinear, clamp to edge.
uniform vec2      uLightGlowOrigin;  ///< Its record's box: lower-left corner, caller px, y up.
uniform vec2      uLightGlowSize;    ///< Its record's box: extent, caller px.
uniform sampler2D uLightRing;        ///< NeonRenderer.Ring: texel for pixel, NEAREST.
uniform vec2      uLightCenter;      ///< The rect's centre, caller px, y up.
uniform vec2      uLightRingOuter;   ///< The ring's outer box, half extents, rect-local px.
uniform vec2      uLightRingHole;    ///< The ring's hole, half extents; 0 on an axis: no hole.
uniform vec2      uLightRingLo;      ///< The atlas layout's box corner, rect-local px.
uniform vec4      uLightRingStrips;  ///< The atlas layout's hole, box px [x0, x1) x [y0, y1).
uniform vec2      uLightRingRows;    ///< The atlas layout's side-strip rows.
uniform vec2      uLightViewport;    ///< The viewport, caller px: the screen the light is read from.

// The edge mask, as GetEdgeMask gives it: full-res px.
uniform vec2  uLightRectSize;
uniform float uLightCornerRadius;
uniform int   uLightGlowSide;
uniform float uLightGlowSideSoftness;
uniform float uLightInsideCutoff;
uniform float uLightInsideCutoffSoftness;
uniform float uLightOutsideCutoff;
uniform float uLightOutsideCutoffSoftness;

// Whether rect-local p is one of the ring's pixels - inside its outer box and
// out of its hole, the annulus the neon draws the ring over (PushAnnulus: a
// hole empty on either axis leaves the whole box).
bool neonLightInRing(vec2 p)
{
    vec2 a = abs(p);
    if (a.x >= uLightRingOuter.x || a.y >= uLightRingOuter.y)
    {
        return false;
    }
    bool solid = uLightRingHole.x <= 0.0 || uLightRingHole.y <= 0.0;
    return solid || a.x >= uLightRingHole.x || a.y >= uLightRingHole.y;
}

// Whether the axis-aligned square [lo, hi] of rect-local px touches any of
// the ring's pixels - the annulus neonLightInRing tests a point against. A
// square that does not reads only glow pixels.
bool neonLightSquareTouchesRing(vec2 lo, vec2 hi)
{
    vec2 far  = max(abs(lo), abs(hi));
    vec2 near = vec2(lo.x > 0.0 || hi.x < 0.0 ? min(abs(lo.x), abs(hi.x)) : 0.0,
                     lo.y > 0.0 || hi.y < 0.0 ? min(abs(lo.y), abs(hi.y)) : 0.0);
    if (near.x >= uLightRingOuter.x || near.y >= uLightRingOuter.y)
    {
        return false; // wholly outside the ring's outer box
    }
    bool solid = uLightRingHole.x <= 0.0 || uLightRingHole.y <= 0.0;
    return solid || far.x >= uLightRingHole.x || far.y >= uLightRingHole.y;
}

// fragPx moved onto the viewport's nearest pixel centre: the screen ends at the
// viewport, and so does the ring's packing (computeRingFieldLayout clips its
// box there), while the ring's annulus runs on past it. A read past the edge
// gets the light at the edge - what clamp-to-edge gives a screen that ended
// there - never a ring texel the atlas does not hold.
vec2 neonLightClampPx(vec2 fragPx)
{
    return clamp(fragPx, vec2(0.5), uLightViewport - 0.5);
}

// The neon's light at caller px fragPx (a pixel centre, y up), for a
// destination whose pixel is px caller px wide - 1 at full resolution, which
// is what the screen's own masks take. Exact on the ring's pixels (the atlas
// holds them texel for pixel); on the glow's, the blit's bilinear read and
// mask. Past the viewport, the light at its edge (neonLightClampPx).
vec4 neonLight(vec2 fragPx, float px)
{
    fragPx = neonLightClampPx(fragPx);
    vec2 p = fragPx - uLightCenter;
    if (neonLightInRing(p))
    {
        return texelFetch(uLightRing, neonRingTexel(p, uLightRingLo, uLightRingStrips, uLightRingRows), 0);
    }
    vec2 uv = (fragPx - uLightGlowOrigin) / uLightGlowSize;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
    {
        return vec4(0.0);
    }
    vec2  halfSize = uLightRectSize * 0.5;
    float aa       = max(sdRoundBoxPixelWidth(p, halfSize, uLightCornerRadius, px), 1e-6);
    float mask     = neonEdgeMaskAt(p, halfSize, uLightCornerRadius, aa, uLightGlowSide, uLightGlowSideSoftness,
                                    uLightInsideCutoff, uLightInsideCutoffSoftness, uLightOutsideCutoff,
                                    uLightOutsideCutoffSoftness);
    return textureLod(uLightGlow, uv, 0.0) * mask;
}
