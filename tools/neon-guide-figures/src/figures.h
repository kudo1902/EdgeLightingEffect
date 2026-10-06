#ifndef _NEON_GUIDE_FIGURES_FIGURES_H_
#define _NEON_GUIDE_FIGURES_FIGURES_H_

// Every figure in docs/neon-onboarding-guide.md, one function per group, each
// writing its files into a directory. The guide names the files; renaming one
// here means renaming its reference there.

#include "core/edge-lighting.h"

#include <string>

namespace NeonGuideFigures
{
    using namespace EdgeLighting;

    /// Part 1: drawn on the CPU from the shaders' own formulas - the signed
    /// distance field and the tone-map curve.
    void WriteConceptFigures(const std::string &dir);

    /// Part 3: the picture, its three layers and their cross-section, the
    /// perimeter position t, why colour is gathered, arcs, segments and the
    /// two coverages. @p effect has the neon and debug layers registered.
    void WriteModelFigures(EdgeLightingEffect &effect, const std::string &dir);

    /// Part 4: what each configuration field does on screen.
    void WriteConfigFigures(EdgeLightingEffect &effect, const std::string &dir);

    /// Parts 5, 6 and 8: what the passes write, captured from a real frame by
    /// PassRecorder, and the geometry each draws.
    void WritePassFigures(const std::string &dir);
}

#endif
