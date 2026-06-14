#version 330 core
precision highp float;

layout(location = 0) in vec2 aPos;
out vec2 vPos;
uniform mat4 uMVP;

void main() {
    vPos = aPos;
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
