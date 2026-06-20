#ifndef _EDGE_LIGHTING_EASING_FUNCTION_H_
#define _EDGE_LIGHTING_EASING_FUNCTION_H_

#include "util/constants.h"

namespace EdgeLighting
{
    /// Easing functions for smooth 0→1 transitions.
    ///
    /// Each function maps a normalised input @p t in [0,1] to a shaped output
    /// (typically also in [0,1]) that accelerates, decelerates, or overshoots
    /// according to the curve name.  Useful anywhere a linear ramp feels too
    /// mechanical — animation keyframes, UI transitions, bloom/glow sweeps, etc.
    ///
    /// All functions are pure (no state, no side effects) and thread-safe.
    namespace EasingFunction
    {
        using Curve = float (*)(float);

        /// Linear interpolation: output == input.
        float Linear(float t);

        /// Quadratic ease-in: starts slow, accelerates toward the end.
        float InQuad(float t);

        /// Quadratic ease-out: starts fast, decelerates toward the end.
        float OutQuad(float t);

        /// Quadratic ease-in-out: slow at both ends, fast in the middle.
        float InOutQuad(float t);

        /// Cubic ease-in: gentler start than InQuad, stronger acceleration.
        float InCubic(float t);

        /// Cubic ease-out: gentler stop than OutQuad, stronger deceleration.
        float OutCubic(float t);

        /// Cubic ease-in-out: smooth start and stop with a steeper middle.
        float InOutCubic(float t);

        /// Sine ease-in: smooth S-curve start.
        float InSine(float t);

        /// Sine ease-out: smooth S-curve stop.
        float OutSine(float t);

        /// Sine ease-in-out: smooth S-curve both ends.
        float InOutSine(float t);

        /// Exponential ease-in: very slow start, rapid finish.
        float InExpo(float t);

        /// Exponential ease-out: rapid start, very slow finish.
        float OutExpo(float t);

        /// Exponential ease-in-out: very slow both ends, very steep middle.
        float InOutExpo(float t);
    } // namespace EasingFunction
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_EASING_FUNCTION_H_
