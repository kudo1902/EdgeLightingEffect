#ifndef _EDGE_LIGHTING_CAPI_DEMO_ANIMATION_PRESETS_H_
#define _EDGE_LIGHTING_CAPI_DEMO_ANIMATION_PRESETS_H_

#include "edge-lighting-capi.h"

namespace EdgeLightingCapiDemo
{
    // Names for the debug-UI preset combo. Index maps 1:1 to el_animation_preset_e
    // - keep this table in sync with the enum in edge-lighting-capi.h.
    inline const char *PresetName(el_animation_preset_e p)
    {
        switch (p)
        {
        case EL_ANIM_NONE:            { return "None"; }
        case EL_ANIM_BREATHING:       { return "Breathing"; }
        case EL_ANIM_STROBE:          { return "Strobe"; }
        case EL_ANIM_HEARTBEAT:       { return "Heartbeat"; }
        case EL_ANIM_SHIMMER:         { return "Shimmer"; }
        case EL_ANIM_AURORA:          { return "Aurora"; }
        case EL_ANIM_REVERSE_SWEEP:   { return "Reverse Sweep"; }
        case EL_ANIM_FADE_IN:         { return "Fade In"; }
        case EL_ANIM_SEGMENT_TRAVEL:  { return "Segment Travel"; }
        case EL_ANIM_SEGMENT_BOUNCE:  { return "Segment Bounce"; }
        case EL_ANIM_COMET:           { return "Comet"; }
        case EL_ANIM_OUTLINE_TRACER:  { return "Outline Tracer"; }
        case EL_ANIM_FADE_OUT:        { return "Fade Out"; }
        case EL_ANIM_HUE_REVERSE:     { return "Hue Reverse"; }
        case EL_ANIM_ARC_WIPE:        { return "Arc Wipe"; }
        default:                      { return "?"; }
        }
    }

    // Highest preset id; combo iterates 0..PRESET_LAST inclusive.
    constexpr int PRESET_LAST = EL_ANIM_ARC_WIPE;
} // namespace EdgeLightingCapiDemo

#endif
