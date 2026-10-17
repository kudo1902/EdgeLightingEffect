// ---------------------------------------------------------------------------
// The edge ring's packing, shared by every program that reads a packed ring:
// the ring field's composite and the ring atlas's copy (neon-field.frag), and
// a reader of the neon's light (neon-light.glsl). Not a shader: injected after
// the tuning header.
//
// The ring is an axis-aligned annulus - a box minus a hole - so its pixels are
// four strips, stored one above the other on the viewport's pixel grid: the
// bottom and the top as they are, the left and the right transposed, so a side
// strip lies along the atlas's width. The layout is NeonRenderer's
// RingFieldLayout, built by computeRingFieldLayout and drawn by drawRingStrips;
// the three change together.
// ---------------------------------------------------------------------------

precision highp float;

// The atlas texel holding the ring pixel at rect-local full-res p. lo is the
// box's lower-left pixel corner (rect-local px), hole the box pixels the ring
// never draws as [x0, x1) x [y0, y1), rows the first atlas row of the left and
// of the right strip. A p in the hole, or outside the box, has no texel: the
// caller only asks for ring pixels.
ivec2 neonRingTexel(vec2 p, vec2 lo, vec4 holeBox, vec2 rows)
{
    ivec2 q    = ivec2(floor(p - lo));
    ivec4 hole = ivec4(holeBox);
    if (q.y < hole.y)
    {
        return q;
    }
    if (q.y >= hole.w)
    {
        return ivec2(q.x, q.y - hole.w + hole.y);
    }
    if (q.x < hole.x)
    {
        return ivec2(q.y - hole.y, int(rows.x) + q.x);
    }
    return ivec2(q.y - hole.y, int(rows.y) + q.x - hole.z);
}
