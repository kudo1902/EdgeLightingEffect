#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

#include <glm/glm.hpp>
#include <cmath>

namespace EdgeLighting
{

    enum class ColorMode
    {
        STATIC,
        GRADIENT,
        AMBIENT_RAINBOW
    };

    typedef struct Config
    {
        // Dimensions of the rectangle boundary
        float width = 800.0f;
        float height = 600.0f;

        // Rectangle top-left corner in app space (0,0 = viewport top-left, +x right, +y down)
        glm::vec2 position = glm::vec2(0.0f, 0.0f);

        // Visual Parameters
        float borderRadius = 40.0f; // Corner rounding radius in pixels
        float glowWidth = 30.0f;    // Width of the glow falloff in pixels
        float lineWidth = 6.0f;     // Thickness of the moving light line in pixels
        float lineLength = 0.25f;   // Length of the line as a fraction of the total perimeter (0.0 to 1.0)
        float speed = 1.0f;         // Speed of the moving line along the perimeter (loops per second)
        float intensity = 1.5f;     // Master brightness intensity

        // Color Mode Configuration
        ColorMode colorMode = ColorMode::AMBIENT_RAINBOW;
        glm::vec4 primaryColor = glm::vec4(0.0f, 0.8f, 1.0f, 1.0f);   // Cyan/Blue (default static/gradient start)
        glm::vec4 secondaryColor = glm::vec4(1.0f, 0.0f, 0.8f, 1.0f); // Magenta/Pink (default gradient end)

        // Particle Visual Parameters
        bool enableParticles = true;
        int maxParticles = 150;
        float particleSize = 8.0f;
        float particleIntensity = 1.2f;
        glm::vec4 particleColor = glm::vec4(1.0f, 0.9f, 0.5f, 0.8f);

        // Multi-light config
        int lightCount = 1; // Number of moving light segments
    } Config;

    namespace GeometryUtils
    {

        inline glm::vec2 GetPointOnRectangle(float perimeterPos, const Config &config)
        {
            float r = config.borderRadius;
            float halfW = config.width * 0.5f;
            float halfH = config.height * 0.5f;

            // Handle sharp corners directly to avoid division-by-zero (NaN) issues
            if (r <= 0.01f)
            {
                float w_str = 2.0f * halfW;
                float h_str = 2.0f * halfH;
                float total = 2.0f * w_str + 2.0f * h_str;
                float dist = perimeterPos * total;

                // Segment 1: Top edge (Left to Right)
                if (dist < w_str)
                {
                    return glm::vec2(-halfW + dist, halfH);
                }
                // Segment 2: Right edge (Top to Bottom)
                if (dist < w_str + h_str)
                {
                    return glm::vec2(halfW, halfH - (dist - w_str));
                }
                // Segment 3: Bottom edge (Right to Left)
                if (dist < w_str + h_str + w_str)
                {
                    return glm::vec2(halfW - (dist - w_str - h_str), -halfH);
                }
                // Segment 4: Left edge (Bottom to Top)
                return glm::vec2(-halfW, -halfH + (dist - w_str - h_str - w_str));
            }

            float w_str = 2.0f * (halfW - r);
            float h_str = 2.0f * (halfH - r);
            float arc = 0.5f * 3.1415926535f * r;
            float total = 4.0f * arc + 2.0f * w_str + 2.0f * h_str;

            float dist = perimeterPos * total;

            glm::vec2 c_tr(halfW - r, halfH - r);
            glm::vec2 c_br(halfW - r, -halfH + r);
            glm::vec2 c_bl(-halfW + r, -halfH + r);
            glm::vec2 c_tl(-halfW + r, halfH - r);

            // Segment 1: Top straight edge
            float s1_end = w_str;
            if (dist < s1_end)
            {
                float t = dist / w_str;
                return glm::vec2(-halfW + r + t * w_str, halfH);
            }

            // Segment 2: Top-right corner arc
            float s2_end = s1_end + arc;
            if (dist < s2_end)
            {
                float t = (dist - s1_end) / arc;
                float angle = 3.1415926535f * 0.5f * (1.0f - t); // PI/2 down to 0
                return c_tr + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 3: Right straight edge
            float s3_end = s2_end + h_str;
            if (dist < s3_end)
            {
                float t = (dist - s2_end) / h_str;
                return glm::vec2(halfW, halfH - r - t * h_str);
            }

            // Segment 4: Bottom-right corner arc
            float s4_end = s3_end + arc;
            if (dist < s4_end)
            {
                float t = (dist - s3_end) / arc;
                float angle = -3.1415926535f * 0.5f * t; // 0 down to -PI/2
                return c_br + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 5: Bottom straight edge
            float s5_end = s4_end + w_str;
            if (dist < s5_end)
            {
                float t = (dist - s4_end) / w_str;
                return glm::vec2(halfW - r - t * w_str, -halfH);
            }

            // Segment 6: Bottom-left corner arc
            float s6_end = s5_end + arc;
            if (dist < s6_end)
            {
                float t = (dist - s5_end) / arc;
                float angle = -3.1415926535f * 0.5f * (1.0f + t); // -PI/2 down to -PI
                return c_bl + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 7: Left straight edge
            float s7_end = s6_end + h_str;
            if (dist < s7_end)
            {
                float t = (dist - s6_end) / h_str;
                return glm::vec2(-halfW, -halfH + r + t * h_str);
            }

            // Segment 8: Top-left corner arc
            float t = (dist - s7_end) / arc;
            float angle = 3.1415926535f * (1.0f - 0.5f * t); // PI down to PI/2
            return c_tl + glm::vec2(std::cos(angle), std::sin(angle)) * r;
        }

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
