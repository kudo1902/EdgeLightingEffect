// ---------------------------------------------------------------------------
// The hue-invariant field's bake (pass 1f, and pass 1r for the edge ring's
// field): the neon's shading, neonShade from neon-shade.glsl, with the arc
// hue on red alone and the segment hue on green alone, written as it leaves
// the shading - before the grade.
//
// Fa and Fs: the pre-tone-map result per unit of each hue, which fieldShade
// (neon-field-common.glsl) tone-maps times the gathered hues. The masks
// below the grade (neon.frag's main) are not in it: for the shading's field
// the blit owns both; the edge ring's field, baked with the ring's uniforms,
// has its composite apply them (neon-ring-field.frag). Nor are the culls
// above it (V35): neonShade(true) discards nothing, and each composite makes
// them on its own grid - see the culls in neon-shade.glsl.
//
// uGlowSide and uGlowSideSoftness reach only the culls and the NeonEdge this
// program never reads, so it may compile both out, and NeonRenderer leaves
// them out of the bake's upload (UploadEdgeMaskUniforms): a read of either
// that reaches neonShade's result has to go back into that upload.
// ---------------------------------------------------------------------------

void main() {
    NeonEdge edge;
    vec3 result = neonShade(true, edge);
    fragColor = vec4(result.r, result.g, 0.0, 1.0);
}
