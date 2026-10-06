precision highp float;

// INVARIANT, because two programs have to agree on it to the bit. Below
// resolutionScale 1.0, NeonRenderer splits the frame between the blit
// (neon-blit.frag) and the edge ring (neon.frag's NEON_READS_GATHER variant): two
// vertex arrays that share their boundary vertices, so the rasteriser gives
// every pixel to exactly one of the two draws. That holds only if both programs
// compute the same gl_Position from the same aPos and uMVP - and GLSL does not
// promise that across separately compiled programs without this qualifier. The
// spec's own example of what goes wrong without it is exactly this: geometry
// misaligned between the passes of a multi-pass algorithm. Here that would be a
// 1 px seam composited twice or not at all along the ring's edge. See
// NeonRenderer::setupRingGeometry.
//
// Every renderer compiles this file, so the qualifier reaches them all. It
// constrains how the compiler may evaluate the one expression below and is not
// meant to move a pixel: measured on desktop GL, 36 captures across all five
// layers and both resolution paths are byte-identical with and without it.
invariant gl_Position;

layout(location = 0) in vec2 aPos;

out vec2 vPos;

uniform mat4 uMVP;

void main() {
    vPos = aPos;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
