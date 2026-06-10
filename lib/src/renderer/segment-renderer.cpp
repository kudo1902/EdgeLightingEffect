#include "renderer/segment-renderer.h"
#include <glad/glad.h>
#include <iostream>

namespace EdgeLighting
{
    // Vertex Shader: compatible with OpenGL 3.3 Core Profile
    static const char *const VERTEX_SHADER_SRC = R"(
    #version 330 core
    precision highp float;

    layout(location = 0) in vec2 aPos;
    layout(location = 1) in vec2 aTexCoords;

    out vec2 vTexCoords;
    out vec2 vPos;

    uniform vec2 uResolution;

    void main() {
        vTexCoords = aTexCoords;
        vPos = aPos * (uResolution * 0.5);
        gl_Position = vec4(aPos, 0.0, 1.0);
    }
    )";

    // Fragment Shader: compatible with OpenGL 3.3 Core Profile
    static const char *const FRAGMENT_SHADER_SRC = R"(
    #version 330 core
    precision highp float;

    in vec2 vTexCoords;
    in vec2 vPos;

    out vec4 fragColor;

    uniform vec2 uResolution;
    uniform vec2 uRectSize;
    uniform float uBorderRadius;
    uniform float uGlowWidth;
    uniform float uLineWidth;
    uniform float uLineLength;
    uniform float uProgress;
    uniform float uIntensity;
    uniform int uColorMode;
    uniform vec4 uPrimaryColor;
    uniform vec4 uSecondaryColor;
    uniform float uTime;
    uniform int uLightCount;

    float sdRoundedBox(vec2 p, vec2 b, float r) {
        vec2 q = abs(p) - b + r;
        return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
    }

    float getPerimeterPos(vec2 p, vec2 b, float r) {
        if (r <= 0.01) {
            float w_str = 2.0 * b.x;
            float h_str = 2.0 * b.y;
            float total = 2.0 * w_str + 2.0 * h_str;

            float distToTop = abs(p.y - b.y);
            float distToBottom = abs(p.y + b.y);
            float distToRight = abs(p.x - b.x);
            float distToLeft = abs(p.x + b.x);

            float minDist = min(min(distToTop, distToBottom), min(distToRight, distToLeft));

            if (minDist == distToTop) {
                float t = (p.x + b.x) / w_str;
                return (t * w_str) / total;
            } else if (minDist == distToRight) {
                float t = (b.y - p.y) / h_str;
                return (w_str + t * h_str) / total;
            } else if (minDist == distToBottom) {
                float t = (b.x - p.x) / w_str;
                return (w_str + h_str + t * w_str) / total;
            } else {
                float t = (p.y + b.y) / h_str;
                return (2.0 * w_str + h_str + t * h_str) / total;
            }
        }

        float w_str = 2.0 * (b.x - r);
        float h_str = 2.0 * (b.y - r);
        float arc = 0.5 * 3.1415926535 * r;
        float total = 4.0 * arc + 2.0 * w_str + 2.0 * h_str;

        vec2 c_tr = b - vec2(r);

        // Top-right corner arc
        if (p.x >= c_tr.x && p.y >= c_tr.y) {
            float angle = atan(p.y - c_tr.y, p.x - c_tr.x);
            float t = (3.1415926535 * 0.5 - angle) / (3.1415926535 * 0.5);
            return (w_str + t * arc) / total;
        }
        // Bottom-right corner arc
        else if (p.x >= c_tr.x && p.y <= -c_tr.y) {
            float angle = atan(-p.y - c_tr.y, p.x - c_tr.x);
            float t = angle / (3.1415926535 * 0.5);
            return (w_str + arc + h_str + t * arc) / total;
        }
        // Bottom-left corner arc
        else if (p.x <= -c_tr.x && p.y <= -c_tr.y) {
            float angle = atan(-p.y - c_tr.y, -p.x - c_tr.x);
            float t = (3.1415926535 * 0.5 - angle) / (3.1415926535 * 0.5);
            return (w_str + arc + h_str + arc + w_str + t * arc) / total;
        }
        // Top-left corner arc
        else if (p.x <= -c_tr.x && p.y >= c_tr.y) {
            float angle = atan(p.y - c_tr.y, -p.x - c_tr.x);
            float t = angle / (3.1415926535 * 0.5);
            return (w_str + arc + h_str + arc + w_str + arc + h_str + t * arc) / total;
        }
        // Top straight edge
        else if (p.y >= c_tr.y) {
            float t = (p.x - (-c_tr.x)) / (2.0 * c_tr.x);
            return (t * w_str) / total;
        }
        // Right straight edge
        else if (p.x >= c_tr.x) {
            float t = (c_tr.y - p.y) / (2.0 * c_tr.y);
            return (w_str + arc + t * h_str) / total;
        }
        // Bottom straight edge
        else if (p.y <= -c_tr.y) {
            float t = (c_tr.x - p.x) / (2.0 * c_tr.x);
            return (w_str + arc + h_str + arc + t * w_str) / total;
        }
        // Left straight edge
        else {
            float t = (p.y - (-c_tr.y)) / (2.0 * c_tr.y);
            return (w_str + arc + h_str + arc + w_str + arc + t * h_str) / total;
        }
    }

    vec3 getRainbowColor(float p) {
        vec3 c1 = vec3(1.0, 0.0, 0.0);
        vec3 c2 = vec3(1.0, 1.0, 0.0);
        vec3 c3 = vec3(0.0, 1.0, 0.0);
        vec3 c4 = vec3(0.0, 1.0, 1.0);
        vec3 c5 = vec3(0.0, 0.0, 1.0);
        vec3 c6 = vec3(1.0, 0.0, 1.0);

        float w = 1.0 / 6.0;
        if (p < w) return mix(c1, c2, p / w);
        if (p < 2.0 * w) return mix(c2, c3, (p - w) / w);
        if (p < 3.0 * w) return mix(c3, c4, (p - 2.0 * w) / w);
        if (p < 4.0 * w) return mix(c4, c5, (p - 3.0 * w) / w);
        if (p < 5.0 * w) return mix(c5, c6, (p - 4.0 * w) / w);
        return mix(c6, c1, (p - 5.0 * w) / w);
    }

    void main() {
        vec2 halfSize = uRectSize * 0.5;
        float d = sdRoundedBox(vPos, halfSize, uBorderRadius);
        float absD = abs(d);

        if (absD > uGlowWidth + 5.0) {
            discard;
        }

        float p_pos = getPerimeterPos(vPos, halfSize, uBorderRadius);
        float maxTailIntensity = 0.0;

        for (int i = 0; i < uLightCount; ++i) {
            float offset = float(i) / float(uLightCount);
            float progress = fract(uProgress + offset);

            float diff = progress - p_pos;
            if (diff < 0.0) diff += 1.0;

            if (diff <= uLineLength) {
                float tail = 1.0 - (diff / uLineLength);
                tail = pow(tail, 1.5);
                maxTailIntensity = max(maxTailIntensity, tail);
            }
        }

        if (maxTailIntensity <= 0.0) {
            discard;
        }

        float core = smoothstep(uLineWidth * 0.5, 0.0, absD);
        float glow = exp(-absD / (uGlowWidth * 0.35));
        float totalGlow = (core * 0.6 + glow * 0.4) * maxTailIntensity * uIntensity;

        vec4 baseColor;
        if (uColorMode == 0) {
            baseColor = uPrimaryColor;
        } else if (uColorMode == 1) {
            baseColor = mix(uPrimaryColor, uSecondaryColor, p_pos);
        } else {
            float shift = fract(p_pos - uTime * 0.25);
            baseColor = vec4(getRainbowColor(shift), 1.0);
        }

        fragColor = baseColor * totalGlow;
    }
    )";

    SegmentRenderer::SegmentRenderer() : shaderProgram_(0), quadVAO_(0), quadVBO_(0) {}

    SegmentRenderer::~SegmentRenderer()
    {
        if (quadVAO_ != 0)
            glDeleteVertexArrays(1, &quadVAO_);
        if (quadVBO_ != 0)
            glDeleteBuffers(1, &quadVBO_);
        if (shaderProgram_ != 0)
            glDeleteProgram(shaderProgram_);
    }

    bool SegmentRenderer::Initialize()
    {
        if (!setupShaders())
        {
            std::cerr << "Failed to compile/link SegmentRenderer shaders." << std::endl;
            return false;
        }
        setupQuadGeometry();
        return true;
    }

    bool SegmentRenderer::setupShaders()
    {
        unsigned int vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &VERTEX_SHADER_SRC, nullptr);
        glCompileShader(vertexShader);

        int success;
        char infoLog[512];
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
            std::cerr << "Vertex Shader Compile Error:\n"
                      << infoLog << std::endl;
            glDeleteShader(vertexShader);
            return false;
        }

        unsigned int fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &FRAGMENT_SHADER_SRC, nullptr);
        glCompileShader(fragmentShader);

        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
            std::cerr << "Fragment Shader Compile Error:\n"
                      << infoLog << std::endl;
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return false;
        }

        shaderProgram_ = glCreateProgram();
        glAttachShader(shaderProgram_, vertexShader);
        glAttachShader(shaderProgram_, fragmentShader);
        glLinkProgram(shaderProgram_);

        glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shaderProgram_, 512, nullptr, infoLog);
            std::cerr << "Shader Program Link Error:\n"
                      << infoLog << std::endl;
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(shaderProgram_);
            return false;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return true;
    }

    void SegmentRenderer::setupQuadGeometry()
    {
        float vertices[] = {
            // positions // texCoords
            -1.0f, 1.0f, 0.0f, 1.0f,
            -1.0f, -1.0f, 0.0f, 0.0f,
            1.0f, -1.0f, 1.0f, 0.0f,

            -1.0f, 1.0f, 0.0f, 1.0f,
            1.0f, -1.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 1.0f, 1.0f};

        glGenVertexArrays(1, &quadVAO_);
        glGenBuffers(1, &quadVBO_);

        glBindVertexArray(quadVAO_);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

        glBindVertexArray(0);
    }

    void SegmentRenderer::Update(float deltaTime, float progress, float time, const Config &config)
    {
        // No CPU updates needed for pure shader renderer
    }

    void SegmentRenderer::Render(int viewportWidth, int viewportHeight, float progress, float time, const Config &config)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive blending for glow

        glUseProgram(shaderProgram_);

        // Set uniforms
        glUniform2f(glGetUniformLocation(shaderProgram_, "uResolution"),
                    static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
        glUniform2f(glGetUniformLocation(shaderProgram_, "uRectSize"),
                    config.width, config.height);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uBorderRadius"), config.borderRadius);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uGlowWidth"), config.glowWidth);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uLineWidth"), config.lineWidth);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uLineLength"), config.lineLength);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uProgress"), progress);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uIntensity"), config.intensity);
        glUniform1i(glGetUniformLocation(shaderProgram_, "uColorMode"), static_cast<int>(config.colorMode));
        glUniform4f(glGetUniformLocation(shaderProgram_, "uPrimaryColor"),
                    config.primaryColor.r, config.primaryColor.g, config.primaryColor.b, config.primaryColor.a);
        glUniform4f(glGetUniformLocation(shaderProgram_, "uSecondaryColor"),
                    config.secondaryColor.r, config.secondaryColor.g, config.secondaryColor.b, config.secondaryColor.a);
        glUniform1f(glGetUniformLocation(shaderProgram_, "uTime"), time);
        glUniform1i(glGetUniformLocation(shaderProgram_, "uLightCount"), config.lightCount);

        glBindVertexArray(quadVAO_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glUseProgram(0);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void SegmentRenderer::OnConfigChanged(const Config &config)
    {
        // Config updates are handled on-the-fly inside Render() via config object
    }

} // namespace EdgeLighting
