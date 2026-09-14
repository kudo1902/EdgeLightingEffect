#ifndef _EDGE_LIGHTING_COMPARE_UTILS_H_
#define _EDGE_LIGHTING_COMPARE_UTILS_H_

#include <glm/glm.hpp>

namespace EdgeLighting
{
    /// @file
    /// @brief Numeric equality for CHANGE DETECTION, which is not the same
    ///        question as numeric equality in general.
    ///
    /// Everything in here differs from @c == in exactly one case: two NaNs
    /// compare EQUAL. That is wrong for arithmetic and right for "did this
    /// value move?", which is the only question these are asked.
    ///
    /// @note Deliberately does NOT include @c core/config.h, unlike every other
    ///       header in this directory. @c Config includes THIS one - its
    ///       @c operator== family is the caller - so a dependency the other way
    ///       would be a cycle. Keep it to @c glm and nothing else.
    namespace CompareUtils
    {
        /// @brief Whether @p a and @p b are the same value, counting two NaNs
        ///        as the same.
        ///
        /// @c Config's @c operator== implementations exist so
        /// @c EdgeLightingEffect::refreshActiveConfig can answer "did this
        /// config move?". That question needs @c x @c == @c x to hold for
        /// EVERY x, and IEEE @c == does not provide it: @c NaN @c != @c NaN, so
        /// a config holding one is not equal to a bitwise copy of itself.
        ///
        /// The consequence is not local and not visible. One NaN anywhere in
        /// the config makes the composite compare unequal on every frame for
        /// the rest of the process, so the generation counter bumps every
        /// frame, so every renderer's @c OnConfigChanged fires every frame -
        /// which among other things sets @c NeonRenderer::mEmissionDirty and
        /// re-bakes the emission table every frame, the exact per-frame cost
        /// docs/emission-prepass.md exists to remove. Measured: 120 of 120
        /// settled frames against 0 of 120 healthy, with nothing in the log.
        /// See review-findings I40.
        ///
        /// Two NaNs being "the same" is the right answer for this question and
        /// only this one: a field that was NaN last frame and is NaN now did
        /// not move. It says nothing about whether the value is USABLE - the C
        /// ABI refuses non-finite floats at the boundary (@c VALIDATE_FINITE)
        /// and @c Oscillator refuses a non-finite frequency, which is where
        /// that question belongs. This is only the backstop that keeps a NaN
        /// arriving some other way from degrading the frame loop instead of
        /// just looking wrong.
        ///
        /// Infinities need no special case: @c inf @c == @c inf is already
        /// true, and @c +inf against @c -inf is correctly a difference.
        inline bool IsSameValue(float a, float b)
        {
            // `a != a` is the NaN test without <cmath>, which this header does
            // not otherwise need.
            return a == b || (a != a && b != b);
        }

        /// @brief The same rule for any float vector, in GLM's own vocabulary.
        ///
        /// Note what is NOT being used here: @c glm::equal(a, @c b) on its own
        /// is component-wise @c ==, so it carries the identical NaN semantics
        /// and would leave the defect above exactly where it was. GLM has no
        /// NaN-reflexive comparison to reach for - what it has is the
        /// component-wise machinery to say this once for @c vec2, @c vec3 and
        /// @c vec4 together rather than as three near-identical overloads.
        ///
        /// Constrained to @c float rather than a free @c T on purpose:
        /// @c glm::isnan static_asserts on integer input, so an @c ivec fails
        /// to match HERE rather than failing deep inside GLM.
        ///
        /// It compares per COMPONENT, which is the part worth stating because a
        /// sloppier reading ("both hold a NaN somewhere") would pass a vector
        /// whose NaN moved from x to y. @c glm::isnan is evaluated on both
        /// operands unconditionally, so unlike the scalar overload this does
        /// not short-circuit on a plain match - a handful of extra unordered
        /// compares per vector field, against one function instead of three.
        template <glm::length_t L, glm::qualifier Q>
        inline bool IsSameValue(const glm::vec<L, float, Q> &a, const glm::vec<L, float, Q> &b)
        {
            return glm::all(glm::equal(a, b) || (glm::isnan(a) && glm::isnan(b)));
        }

    } // namespace CompareUtils
} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_COMPARE_UTILS_H_
