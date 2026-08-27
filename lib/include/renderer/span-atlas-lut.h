#ifndef _EDGE_LIGHTING_SPAN_ATLAS_LUT_H_
#define _EDGE_LIGHTING_SPAN_ATLAS_LUT_H_

#include "core/config.h"
#include "renderer/base-lut.h"
#include "util/color-utils.h"
#include <algorithm>
#include <vector>

namespace EdgeLighting
{
    /// A per-item gradient atlas: one texture row per item, holding that
    /// item's colour stops baked head-to-tail across its span. Rows for items
    /// with no stops - and rows past the end of the list - stay zero; the neon
    /// shaders read that as "inherit the base gradient" via the hasStops flag
    /// their UBO carries (SegmentBlock / ArcBlock vec4.w), so unused rows need
    /// no other signal.
    ///
    /// @tparam T anything exposing @c colorStops and @c blendSpace - today
    ///         @ref Arc and @ref SegmentBoost, whose atlases differ only in
    ///         which list feeds them and how wide/tall they are.
    ///
    /// The texture is a DERIVED value: it is whatever the last @ref Bake was
    /// handed, and nothing else may write it - see @ref BaseLUT, which owns the
    /// handle and keeps @c Texture2D::SetData out of reach of this class as
    /// well as of callers.
    ///
    /// Usage is one call per frame from the owning renderer's
    /// @c OnConfigChanged - @ref Bake self-guards, so the caller does not
    /// carry a dirty flag:
    ///
    ///     mArcLUT.Bake(config.neon.arcs, ARC_LUT_WIDTH, MAX_ARCS);
    ///     ...
    ///     mArcLUT.Bind(2);
    template <typename T>
    class SpanAtlasLUT : public BaseLUT
    {
    public:
        SpanAtlasLUT() = default;

        /// Re-bake and upload the atlas, but only if it would actually differ
        /// from the one already on the GPU - see @ref isDirty. Safe (and
        /// intended) to call every frame.
        ///
        /// @param items   source list; only the first @p maxRows are baked,
        ///                the rest cannot reach the shader anyway.
        /// @param width   texels per row - the resolution of one item's span.
        ///                Clamped to >= 2: the row walk below divides by
        ///                @c width - 1, so 1 would be a division by zero and a
        ///                row of NaN, and 0 or less would hand
        ///                @c glTexImage2D an empty image. Today's callers pass
        ///                a constant, but @c OptimizedNeonConfig::gradientLutSize
        ///                shows these widths do become host-settable, and
        ///                @ref GradientRingLUT::Bake already guards its own.
        /// @param maxRows atlas height, matching the shader's array cap.
        ///                Clamped to >= 1 for the same reason.
        void Bake(const std::vector<T> &items, int width, int maxRows)
        {
            width = std::max(width, 2);
            maxRows = std::max(maxRows, 1);

            // Clamped BEFORE the dirty check, so the snapshot the check
            // compares against always describes the texture that was actually
            // uploaded rather than the arguments that were asked for.
            if (!isDirty(items, width, maxRows))
            {
                return;
            }

            const int rows = static_cast<int>(std::min(items.size(), static_cast<size_t>(maxRows)));

            // Held as a member so a re-bake does no heap allocation after the
            // first. resize() alone would leave a shrunken atlas's stale tail
            // in place, so zero the whole thing: "no stops" IS the zero row.
            mAtlas.assign(static_cast<size_t>(width) * maxRows * 4, 0);

            for (int i = 0; i < rows; ++i)
            {
                const T &item = items[i];
                if (item.colorStops.empty())
                {
                    continue; // row stays zero; shader falls back to base gradient
                }
                // Sorted once per row, not per texel - SampleSpan walks the
                // stops in order and an unsorted list bakes a silently wrong
                // gradient.
                const std::vector<ColorStop> stops = ColorUtils::SortStops(item.colorStops);
                unsigned char *row = mAtlas.data() + (static_cast<size_t>(i) * width * 4);
                for (int x = 0; x < width; ++x)
                {
                    float t = static_cast<float>(x) / static_cast<float>(width - 1);
                    // Clamped, not cyclic: this row is a head-to-tail span
                    // sampled CLAMP_TO_EDGE, so stops that do not reach 0 and 1
                    // must hold their end colours rather than wrapping. See
                    // ColorUtils::SampleSpan.
                    glm::vec4 c = ColorUtils::SampleSpan(t, stops, item.blendSpace);
                    row[x * 4 + 0] = ColorUtils::ToByte(c.r);
                    row[x * 4 + 1] = ColorUtils::ToByte(c.g);
                    row[x * 4 + 2] = ColorUtils::ToByte(c.b);
                    row[x * 4 + 3] = ColorUtils::ToByte(c.a);
                }
            }

            // CLAMP on U: a row runs head-to-tail, with no wrap at its own
            // ends. (BaseLUT always clamps V - rows must not bleed together.)
            Upload(mAtlas.data(), width, maxRows, GL_CLAMP_TO_EDGE);

            mBaked.clear();
            mBaked.reserve(static_cast<size_t>(rows));
            for (int i = 0; i < rows; ++i)
            {
                mBaked.push_back(BakedRow{items[i].colorStops, items[i].blendSpace});
            }
            mBakedWidth = width;
            mBakedRows = maxRows;
        }

    private:
        /// Exactly the inputs a row is baked from. Snapshotting these rather
        /// than whole @c T values is the point: comparing whole structs - which
        /// is what `arcs != mBakedArcs` used to do - re-baked and re-uploaded
        /// the atlas on every ArcWipe / OutlineTracer / SegmentTravel frame,
        /// because those animations write exactly the live fields (an arc's
        /// start / length / intensity, a segment's position / length / boost)
        /// that ride the UBOs and are re-uploaded every frame regardless. With
        /// the snapshot narrowed to what a bake actually reads, that whole
        /// class of false dirty cannot come back.
        typedef struct BakedRow
        {
            std::vector<ColorStop> colorStops;
            BlendSpace blendSpace = BlendSpace::RGB;
        } BakedRow;

        /// Whether an atlas baked from @p items would differ from the one
        /// currently uploaded.
        ///
        /// The comparison is POSITIONAL, not set-wise: the atlas is indexed by
        /// item index, so swapping two entries swaps their rows even though the
        /// collection of stops is unchanged.
        ///
        /// Items past @p maxRows are ignored on both sides - they cannot reach
        /// the atlas, so appending or editing a 9th arc when 8 fit is genuinely
        /// not a change to the texture.
        bool isDirty(const std::vector<T> &items, int width, int maxRows) const
        {
            if (!HasUploaded() || width != mBakedWidth || maxRows != mBakedRows)
            {
                return true;
            }
            const size_t rows = std::min(items.size(), static_cast<size_t>(maxRows));
            if (rows != mBaked.size())
            {
                return true;
            }
            for (size_t i = 0; i < rows; ++i)
            {
                if (mBaked[i].blendSpace != items[i].blendSpace ||
                    mBaked[i].colorStops != items[i].colorStops)
                {
                    return true;
                }
            }
            return false;
        }

    private:
        std::vector<BakedRow> mBaked;      ///< Snapshot of what produced the current texture.
        std::vector<unsigned char> mAtlas; ///< Reused bake scratch (RGBA8, width * maxRows * 4).
        int mBakedWidth = 0;               ///< Dimensions behind the current texture; 0 until
        int mBakedRows = 0;                ///< the first bake.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SPAN_ATLAS_LUT_H_
