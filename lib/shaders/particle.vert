precision highp float;

// Per-particle vertex attributes.
layout(location = 0) in vec2  aPos;   // rect-local coords (rect-centered)
layout(location = 1) in vec3  aColor; // RGB, linear.
layout(location = 2) in float aLife;  // fractional life remaining in [0, 1]
layout(location = 3) in float aSize;  // spawn size in pixels

uniform mat4 uMVP;

out vec3  vColor;
out float vLife;

void main() {
    gl_Position  = uMVP * vec4(aPos, 0.0, 1.0);
    // Shrink toward zero as the particle ages so they don't pop out of view.
    gl_PointSize = max(aSize * aLife, 1.0);
    vColor       = aColor;
    vLife        = aLife;
}
