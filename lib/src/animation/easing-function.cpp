#include "animation/easing-function.h"
#include <cmath>

namespace EdgeLighting
{
    namespace EasingFunction
    {
        float Linear(float t) { return t; }

        float InQuad(float t) { return t * t; }

        float OutQuad(float t)
        {
            float u = 1.0f - t;
            return 1.0f - u * u;
        }

        float InOutQuad(float t)
        {
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        }

        float InCubic(float t) { return t * t * t; }

        float OutCubic(float t)
        {
            float u = 1.0f - t;
            return 1.0f - u * u * u;
        }

        float InOutCubic(float t)
        {
            return t < 0.5f
                       ? 4.0f * t * t * t
                       : 1.0f - 4.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
        }

        float InSine(float t) { return 1.0f - std::cos(t * (PI * 0.5f)); }

        float OutSine(float t) { return std::sin(t * (PI * 0.5f)); }

        float InOutSine(float t) { return 0.5f * (1.0f - std::cos(t * PI)); }

        float InExpo(float t) { return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f)); }

        float OutExpo(float t) { return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }

        float InOutExpo(float t)
        {
            if (t <= 0.0f)
            {
                return 0.0f;
            }
            if (t >= 1.0f)
            {
                return 1.0f;
            }
            return t < 0.5f
                       ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f)
                       : 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
        }
    } // namespace EasingFunction
} // namespace EdgeLighting
