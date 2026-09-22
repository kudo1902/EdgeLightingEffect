precision highp float;

// Spotlight strip vertex stage.
//
// Every lamp is drawn as a strip of quads hugging its own support, and its
// scalars ride ALONG WITH that geometry as vertex attributes rather than in a
// uniform block. Three consequences worth knowing:
//
//   - There is no per-index array anywhere in this shader, so the project's
//     no-bare-uniform-arrays rule is satisfied by construction. Nothing to
//     bind, no std140 layout to keep in step with a C++ struct.
//   - aP0 / aP1 / aColor / aClipWeight carry the SAME value on all six of
//     every quad in a lamp's strip, so they would interpolate to themselves
//     even without `flat`. The qualifier is here because skipping the
//     interpolation is free, not because correctness needs it.
//   - aLocal is the one attribute that genuinely varies: the renderer
//     pre-rotates each corner into the lamp's own frame, so the fragment stage
//     never touches a sin or a cos, and never reads gl_FragCoord.
//
// aPos is in APP coordinates - origin top-left, +y DOWN, the same space as
// Config::geometry.position. uMVP carries a y-flipped ortho that maps it
// straight to clip space (see SpotlightRenderer::Render).
//
// vApp forwards that app-space position to the fragment stage, where the clip
// area is evaluated. It is a SECOND USE of aPos, not a second attribute, and
// it is what keeps the clip out of gl_FragCoord: the projection is an ortho
// (w is 1 everywhere), so this varying interpolates exactly and reads the same
// app pixel whatever buffer the strips are being rasterised into.
//
// That makes the clip's GEOMETRY scale-free; it does not make its EDGE so. The
// mask is still evaluated once per fragment, so a reduced-resolution buffer
// resolves the boundary at its own texel pitch and the blit smears it back.
// SpotlightRenderer pins resolutionScale to 1.0 whenever a clipped lamp is
// enabled for that reason - see GetClampedSpotScale for the measurements, and
// the note about uniforms in SpotlightRenderer::Render.

layout(location = 0) in vec2 aPos;   ///< App px, top-left origin, +y down.
layout(location = 1) in vec2 aLocal; ///< (along, across) px in the lamp's frame.
layout(location = 2) in vec4 aP0;    ///< tanHalfBeam, throwLength, softK, intensity.
layout(location = 3) in vec4 aP1;    ///< apertureWidth, bloom, bloomRadius, bloomWindow.
layout(location = 4) in vec3 aColor; ///< Linear RGB, baked from colorTemp on the CPU.
// A WEIGHT, not a flag, and named for it: the fragment stage mixes with it
// rather than branching on it. "Weight" also keeps it clear of the OTHER clip
// in a vertex shader - gl_Position's clip space, which this has nothing to do
// with.
layout(location = 5) in float aClipWeight; ///< 1 where this lamp honours the clip area, else 0.

out vec2 vLocal;
out vec2 vApp;
flat out vec4 vP0;
flat out vec4 vP1;
flat out vec3 vColor;
flat out float vClipWeight;

uniform mat4 uMVP;

void main() {
    vLocal = aLocal;
    vApp = aPos;
    vP0 = aP0;
    vP1 = aP1;
    vColor = aColor;
    vClipWeight = aClipWeight;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
