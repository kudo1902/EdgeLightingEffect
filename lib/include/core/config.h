#ifndef EDGE_LIGHTING_CONFIG_H
#define EDGE_LIGHTING_CONFIG_H

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

    struct Config
    {
        // Dimensions of the rectangle boundary
        float width = 800.0f;
        float height = 600.0f;

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
    };

    namespace GeometryUtils
    {

        inline glm::vec2 GetPointOnRectangle(float perimeterPos, const Config &config)
        {
            float r = config.borderRadius;
            float halfW = config.width * 0.5f;
            float halfH = config.height * 0.5f;

            float w_str = 2.0f * (halfW - r);
            float h_str = 2.0f * (halfH - r);
            float arc = 0.5f * 3.1415926535f * r;
            float total = 4.0f * arc + 2.0f * w_str + 2.0f * h_str;

            float dist = perimeterPos * total;

            glm::vec2 c_tr(halfW - r, halfH - r);
            glm::vec2 c_br(halfW - r, -halfH + r);
            glm::vec2 c_bl(-halfW + r, -halfH + r);
            glm::vec2 c_tl(-halfW + r, halfH - r);

            // Segment 1: Top straight edge (right half)
            float s1_end = w_str * 0.5f;
            if (dist < s1_end)
            {
                return glm::vec2(dist, halfH);
            }

            // Segment 2: Top-right corner
            float s2_end = s1_end + arc;
            if (dist < s2_end)
            {
                float t = (dist - s1_end) / arc;
                float angle = 3.1415926535f * 0.5f * (1.0f - t);
                return c_tr + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 3: Right straight edge
            float s3_end = s2_end + h_str;
            if (dist < s3_end)
            {
                float t = (dist - s2_end) / h_str;
                return glm::vec2(halfW, c_tr.y - t * h_str);
            }

            // Segment 4: Bottom-right corner
            float s4_end = s3_end + arc;
            if (dist < s4_end)
            {
                float t = (dist - s3_end) / arc;
                float angle = -3.1415926535f * 0.5f * t;
                return c_br + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 5: Bottom straight edge
            float s5_end = s4_end + w_str;
            if (dist < s5_end)
            {
                float t = (dist - s4_end) / w_str;
                return glm::vec2(c_br.x - t * w_str, -halfH);
            }

            // Segment 6: Bottom-left corner
            float s6_end = s5_end + arc;
            if (dist < s6_end)
            {
                float t = (dist - s5_end) / arc;
                float angle = -3.1415926535f * 0.5f * (1.0f + t);
                return c_bl + glm::vec2(std::cos(angle), std::sin(angle)) * r;
            }

            // Segment 7: Left straight edge
            float s7_end = s6_end + h_str;
            if (dist < s7_end)
            {
                float t = (dist - s6_end) / h_str;
                return glm::vec2(-halfW, c_bl.y + t * h_str);
            }

            // Segment 8: Top-left corner
            float t = (dist - s7_end) / arc;
            float angle = 3.1415926535f * (1.0f - 0.5f * t);
            return c_tl + glm::vec2(std::cos(angle), std::sin(angle)) * r;
        }

    } // namespace GeometryUtils

} // namespace EdgeLighting

#endif // EDGE_LIGHTING_CONFIG_H
