// ---------------------------------------------------------------------------
// The rounded box's signed distance, its antialiasing width and the cutoff
// band's two boundaries, shared by neon.frag, neon-blit.frag, neon-field.frag
// and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the four
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution), the ring field's composite (which re-applies the ring's) and
// the opaque fill (which has to register with the glow's edge) - so the four
// stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// One pixel's width in sdRoundBox's units, for the box filter across the
// outline: what fwidth(d) returns along a straight stretch of it, taken from
// the distance's analytic gradient instead of from d's own differences.
//
// fwidth(d) differences d across a 2x2 quad, and at a sharp corner that is not
// a derivative. On the vertex pixel the quad can hold BOTH outside neighbours:
// d then steps a whole pixel along each axis, fwidth reads 2, and every ramp
// sized from it doubles there - the corner pixel of a one-sided cut came out
// at 0.84 of the other three corners', the fill's at 0.75. The quad's parity
// decides which corner, so one rect showed it at its top-left and the next at
// no corner at all (V26). Along the inside diagonal it read 0 or 2 the same
// way. The gradient is one-sided by construction - a pixel takes the edge it
// is nearest - so it is 1 on every corner pixel, as along the edges, and
// fwidth's value wherever d is smooth.
//
// |gradient| per axis times that axis's pixel, fwidth(p): exact because every
// caller's p is the rect's own frame through an unrotated orthographic
// transform - vPos, or gl_FragCoord less the centre - so each screen axis
// moves one axis of p. q and length(max(q, 0)) are sdRoundBox's own, so a
// caller taking both pays for the length once. A derivative: take it in
// uniform control flow.
float sdRoundBoxFwidth(vec2 p, vec2 b, float r) {
    vec2  q  = abs(p) - b + r;
    vec2  qp = max(q, 0.0);
    float l  = length(qp);
    vec2  g  = (q.x > 0.0 && q.y > 0.0) ? qp / l
             : ((q.x > q.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    return dot(g, fwidth(p));
}

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
}
