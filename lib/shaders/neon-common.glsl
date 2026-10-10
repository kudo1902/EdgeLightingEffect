// ---------------------------------------------------------------------------
// What neon.frag and neon-gather.frag share
// ---------------------------------------------------------------------------
//
// Not a shader on its own. lib/CMakeLists.txt reads it and shaders.h.in
// injects it into both, after the version line and @NEON_TUNING@ and ahead of
// the file's own source - so everything here is declared before either file's
// first line, and neither declares any of it again.
//
// It is what the gather pass and the shading that reads it back both declare:
// the fragment position, the rect's shape, the segment block and the
// perimeter's length. The gather loop itself is neon-gather.frag's alone -
// it used to live here too, compiled out of neon.frag's reading variant, while
// neon.frag could also run it inline at 1.0; that path is gone
// (docs/neon-shader-cleanup-plan.md step 3).

precision highp float;

in vec2 vPos;

uniform vec2  uRectSize;
uniform float uCornerRadius;

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Here rather than in neon.frag because the gather branches on uSegmentCount;
// every neon program reads it, whichever file it is built from.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes one element per registered property ("uSegments[0]", ...) into
// the block's UBO at the reflected std140 array stride. On desktop GL the
// block is fed from a UBO in neon-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

// Perimeter of the rounded rect, in the px uRectSize is in - SCALED px below
// resolutionScale 1.0. Sizes the gather's colour kernel, and neon.frag also
// converts the arc feathers from px to perimeter fractions with it.
float rectPerimeter() {
    float r = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    return 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;
}
