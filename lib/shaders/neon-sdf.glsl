// ---------------------------------------------------------------------------
// The rounded box's signed distance and the cutoff band's two boundaries,
// shared by neon.frag, neon-blit.frag and black-rect.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it into the three
// programs that have to agree on the shape to the pixel - the glow, the
// scaled path's blit (which re-applies the glow's cut and cutoffs at full
// resolution) and the opaque fill (which has to register with the glow's
// edge) - so the three stay one copy. It reads no uniform and no constant.
// ---------------------------------------------------------------------------

precision highp float;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
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
