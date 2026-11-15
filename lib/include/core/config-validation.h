#ifndef _EDGE_LIGHTING_CONFIG_VALIDATION_H_
#define _EDGE_LIGHTING_CONFIG_VALIDATION_H_

#include "core/config.h"

namespace EdgeLighting
{
    /// The ranges every Config value is held to before a renderer sees it -
    /// the one place they live. A renderer clamps what it reads to the range
    /// it can draw (a resolution scale to (0, 1], a sample count to the
    /// UBO's size); this holds the values to the range in which those clamps,
    /// and the arithmetic before them, are defined at all.
    ///
    /// WHY. The C ABI refuses only NaN and infinity, and an animation's
    /// overlay (a Remap, a FieldBoundAnimation) reaches the config without
    /// even that. Finite but absurd values used to break the library: a
    /// spotlight throw of 1e10 px left a float bisection unable to finish
    /// (SetConfig never returned), a rect whose negative width outweighed its
    /// height - a perimeter below 0 - hung the GPU in the glow coverage bake
    /// on every frame, and a 1e10 px rect cast an out-of-range float to int
    /// (UB) when its buffer windows were sized. Every bound here is far
    /// outside any value a host means: no shipped config, preset or gate
    /// scene comes near one, so sanitising changes no pixel it draws.
    ///
    /// @ref EdgeLightingEffect applies it to the composited config - base
    /// plus animation overlays - so both ways a value gets in are covered;
    /// the base it hands back (GetConfig) is left as set. A host driving a
    /// renderer without the effect calls @ref Sanitize itself.
    namespace ConfigValidation
    {
        /// The largest magnitude of any length, size or position in px: far
        /// past any display (8K is 7680 px across) with room for a rect
        /// animated in from off screen, and small enough that px arithmetic
        /// keeps sub-pixel precision and every px count fits an int.
        constexpr float MAX_EXTENT_PX = 1.0e5f;
        /// The largest magnitude of any other float - gains, rates, angles in
        /// degrees, perimeter positions, colour channels, durations.
        constexpr float MAX_MAGNITUDE = 1.0e6f;
        /// The widest gradient ring: GLES 3.0's minimum GL_MAX_TEXTURE_SIZE,
        /// so the LUT's upload cannot fail on any target. Documented 32-256.
        constexpr int MAX_GRADIENT_LUT_SIZE = 2048;

        /// Hold every field of @p config to its range, in place, NaN mapped to
        /// 0 (or the bound nearest 0): a NaN never equals itself, so one left
        /// in the config would defeat every change test and skip key, every
        /// frame. Cheap - a pass over the scalars and the light and stop
        /// vectors, no allocation - and idempotent.
        void Sanitize(Config &config);
    }
}

#endif // _EDGE_LIGHTING_CONFIG_VALIDATION_H_
