#version 330 core
precision highp float;

in vec4 vColor;
out vec4 fragColor;

void main() {
    vec2 coord = gl_PointCoord - vec2(0.5);
    float dist = length(coord);
    if (dist > 0.5) {
        discard;
    }
    float alpha = smoothstep(0.5, 0.2, dist) * vColor.a;
    fragColor = vec4(vColor.rgb, alpha);
}
