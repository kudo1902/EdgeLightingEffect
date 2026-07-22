precision highp float;

// Trivial passthrough for a fullscreen NDC quad. The fragment shader derives
// pixel coordinates from gl_FragCoord (y-up) - not from a varying - so no
// interpolation precision is required here on mobile GLES.
in vec2 aPos;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
}
