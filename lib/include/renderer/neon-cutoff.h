#ifndef _EDGE_LIGHTING_NEON_CUTOFF_H_
#define _EDGE_LIGHTING_NEON_CUTOFF_H_

#include "core/config.h"
#include "renderer/neon-tuning.h"
#include <algorithm>

namespace EdgeLighting
{
    /// Pixel distance the shaders should treat as a cutoff boundary when the
    /// host disabled that side. Huge, so the emission shader's smoothstep /
    /// discard math naturally no-ops on realistic geometry; only the CPU knows
    /// this number, shaders see it as a plain uniform.
    constexpr float CUTOFF_DISABLED_SIZE = 1.0e6f;

    /// Cutoff size as the *emission* shader wants it (neon.frag).
    inline float GetCutoffSize(const Cutoff &c)
    {
        return c.enable ? c.size : CUTOFF_DISABLED_SIZE;
    }

    /// Feather applied at a fill boundary that fell back to @c glowReach, as a
    /// fraction of that reach. The fill's outer boundary is a step in the
    /// *background*, not in the emission - at glowReach the emission is
    /// already zero, so there is no glow to hide a hard edge and the backdrop
    /// would snap back on over a single pixel. Feathering over a large
    /// fraction of the reach turns that into an unnoticeable gradient.
    constexpr float OPAQUE_AUTO_SOFTNESS_FRACTION = 0.5f;

    /// Per-side geometry for the opaque-mode background fill
    /// (black-rect.frag), resolved from the neon sub-config. Shared by
    /// NeonRenderer and NeonOptimizedRenderer so the two paths can't drift.
    ///
    /// Distances are positive pixel offsets from the rect edge along their own
    /// side, matching the fill shader's uniforms.
    typedef struct OpaqueFillParams
    {
        float insideBound;     ///< Fill reaches d = -insideBound.
        float insideSoftness;  ///< Feather width in px at that boundary.
        float outsideBound;    ///< Fill reaches d = +outsideBound.
        float outsideSoftness; ///< Feather width in px at that boundary.
    } OpaqueFillParams;

    /// Resolve the fill's per-side bounds and feathers.
    ///
    /// @param neon      Neon sub-config (cutoffs + opaqueSoftness).
    /// @param glowReach Distance in px from the rect edge at which the
    ///                  *exterior* emission is guaranteed zero - the
    ///                  renderer's draw-quad margin, past which there is no
    ///                  geometry and the shader has faded the emission out.
    ///
    /// Bounds, per side, when the side's Cutoff is disabled:
    ///
    ///  - Outside: fall back to @c glowReach. The sentinel would paint the
    ///    entire viewport exterior, and nothing is drawn out there to justify
    ///    it - the quad ends at glowReach.
    ///  - Inside: keep the sentinel, i.e. fill the whole interior. The
    ///    emission has no interior bound either: the bloom's 1/D tail plus the
    ///    far edges keep the whole rect interior lit (measured ~30% coverage
    ///    dead centre of an 800x400 rect at default settings), so any finite
    ///    bound here would cut the backing plate short of the glow it backs
    ///    and leave a seam. Hosts that want a band must state @c insideCutoff.
    ///
    /// Softness resolution per side, in order:
    ///   1. @c neon.opaqueSoftness when non-zero - an explicit host override,
    ///      applied to both sides.
    ///   2. the side's @c Cutoff::softness when that cutoff is enabled, so the
    ///      fill's edge and the emission's fade land together. Mismatched
    ///      feathers leave a dark trough (fill still opaque, emission already
    ///      faded) ending in a hard step where the background returns.
    ///   3. otherwise @c OPAQUE_AUTO_SOFTNESS_FRACTION of the fallback bound.
    ///      Only meaningful on the outside; an unbounded inside has no
    ///      boundary on screen to feather.
    inline OpaqueFillParams GetOpaqueFillParams(const NeonConfig &neon, float glowReach)
    {
        const float reach = std::max(glowReach, 1.0f);
        const float minSoft = static_cast<float>(SIDE_SOFT_EPSILON);

        OpaqueFillParams p = {};

        p.insideBound = neon.insideCutoff.enable ? neon.insideCutoff.size : CUTOFF_DISABLED_SIZE;
        p.outsideBound = neon.outsideCutoff.enable ? neon.outsideCutoff.size : reach;

        if (neon.opaqueSoftness > 0.0f)
        {
            p.insideSoftness = neon.opaqueSoftness;
            p.outsideSoftness = neon.opaqueSoftness;
        }
        else
        {
            p.insideSoftness = neon.insideCutoff.enable ? neon.insideCutoff.softness : 0.0f;
            p.outsideSoftness = neon.outsideCutoff.enable
                                    ? neon.outsideCutoff.softness
                                    : p.outsideBound * OPAQUE_AUTO_SOFTNESS_FRACTION;
        }

        p.insideSoftness = std::max(p.insideSoftness, minSoft);
        p.outsideSoftness = std::max(p.outsideSoftness, minSoft);
        return p;
    }

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_NEON_CUTOFF_H_
