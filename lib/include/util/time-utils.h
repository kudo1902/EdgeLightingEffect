#ifndef _EDGE_LIGHTING_TIME_UTILS_H_
#define _EDGE_LIGHTING_TIME_UTILS_H_

#include <cmath>

namespace EdgeLighting
{
    namespace TimeUtils
    {
        /// @brief Reduce an ever-growing clock time to the smallest value that
        ///        samples the gradient ring identically.
        ///
        /// Every shader that reads @c uTime does exactly one thing with it:
        /// @c position @c - @c uTime @c * @c uHueRotationRate, used to sample a
        /// @c GL_REPEAT ring (neon.frag, neon-emission.frag, neon-lut-debug.frag
        /// - one use each, no others). Adding or removing a whole TURN from
        /// @c uTime @c * @c rate therefore lands on the same texel. One turn is
        /// @c 1/rate seconds, so reducing modulo that is EXACT: not an
        /// approximation, and not a wrap the eye can catch.
        ///
        /// It exists because @c uTime is a float uniform, and a float's ULP
        /// grows with its value. A raw clock time large enough stops resolving a
        /// frame's worth of rotation - at 2^18 seconds the ULP is 0.031s against
        /// an 0.0083s frame - and the hue simply stops turning. Reduced, the
        /// value never exceeds one turn (under two seconds at the default rate)
        /// and keeps full float precision for as long as the process lives.
        ///
        /// This is why the renderer interface carries @c double time: the
        /// reduction has to happen at full precision, so the renderer that knows
        /// what its own shader does with the value is the one that must do it.
        /// See review-findings I33b.
        ///
        /// @param time Clock seconds, full precision.
        /// @param rate Turns per second. At zero the term drops out of the
        ///             shader entirely, so there is nothing to preserve and
        ///             there is no turn to divide by - 0 is returned.
        inline double WrapHueTime(double time, float rate)
        {
            const double r = static_cast<double>(rate);
            if (r == 0.0)
            {
                return 0.0;
            }
            // |rate|, because the turn LENGTH is the same whichever way the ring
            // is rotating; the sign lives in the multiplication in the shader.
            const double turn = 1.0 / std::fabs(r);
            return std::fmod(time, turn);
        }

    } // namespace TimeUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_TIME_UTILS_H_
