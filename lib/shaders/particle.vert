#version 330 core
precision highp float;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;

out vec4 vColor;

uniform vec2 uResolution;
uniform vec2 uOffset;

void main() {
    vColor = aColor;
    vec2 worldPos = aPos + uOffset;
    vec2 ndcPos = worldPos / (uResolution * 0.5);
    gl_Position = vec4(ndcPos, 0.0, 1.0);
    gl_PointSize = aSize;
}
