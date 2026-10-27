#ifndef _NEON_SCALE_CHECK_FORMAT_CHECK_RUN_H_
#define _NEON_SCALE_CHECK_FORMAT_CHECK_RUN_H_

// What `formats`' cases share (format-check.cpp, format-check-causes.cpp):
// the cells a run draws, the hooks it sets on the effect's
// RenderTargetFormats, one run on a fresh effect, and the helpers that judge
// what it left. See format-check.h for the cases themselves.

#include "harness.h"
#include "scenes.h"

#include "core/config.h"
#include "core/render-target-formats.h"
#include "gl/gl-header.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace NeonScaleCheck
{
    namespace FormatRun
    {
        /// GLES 3.0 with neither EXT_color_buffer_half_float nor
        /// EXT_color_buffer_float: no 16-bit float target renders.
        inline const std::vector<GLint> HALF_FLOAT = {GL_RGBA16F, GL_RG16F, GL_R16F};
        /// Every format the library allocates: nothing renders.
        inline const std::vector<GLint> EVERYTHING = {GL_RGBA16F, GL_RG16F, GL_R16F, GL_RGBA8, GL_RG8};
        /// The startup report's rows (RenderTargetFormats::LogReport): what a
        /// run probes, less what a hook answers instead.
        inline constexpr GLint REPORT_ROWS[] = {GL_RGBA16F, GL_RG16F, GL_R16F, GL_RGBA8, GL_RG8};

        inline constexpr float CELL_SCALES[] = {1.0f, 0.5f, 0.25f};
        inline constexpr const char *CELL_SCALE_TAGS[] = {"s1000", "s500", "s250"};
        /// CELL_SCALES' indices of 1.0, 0.5 and 0.25.
        inline constexpr size_t SCALE_1000 = 0;
        inline constexpr size_t SCALE_500 = 1;
        inline constexpr size_t SCALE_250 = 2;

        /// What moves between a cell's frames.
        typedef enum class Variant
        {
            STILL,     ///< Nothing: the steady frame, as `check` draws it.
            HUE,       ///< The hue rotating, the clock advancing - the glow field's frames.
            INTENSITY, ///< The intensity moving every frame - the field's gain path (I56).
        } Variant;

        /// One scene, drawn one way, at one scale.
        typedef struct Cell
        {
            std::string name;  ///< For a failure line.
            std::string group; ///< What its quality bound is kept per: layer, variant, scale.
            Config config;
            Variant variant;
            /// For a flare or spotlight cell below 1.0: the name of the same
            /// layer's cell at 1.0, which case C must draw - with no buffer to
            /// be had, the layer falls back to 1.0. Empty otherwise.
            std::string fullScaleName;
        } Cell;

        /// What a run's RenderTargetFormats is told, through its verification
        /// hooks.
        typedef struct Hooks
        {
            std::vector<GLint> refused;                  ///< SetRefusedFormats.
            RenderTargetFormats::ForcedFailure failed;   ///< SetFailedAllocations: fails nothing by default.
            std::vector<GLint> unknown;                  ///< SetUnknownFormats.
            int maxTextureSize = 0;                      ///< SetMaxTextureSize: 0 lowers nothing.
        } Hooks;

        /// Everything one run left to judge.
        typedef struct RunResult
        {
            std::vector<RGBA> frames;             ///< Each cell's last frame, over transparent black.
            bool neonPresent = false;             ///< The neon survived Initialize.
            size_t probes = 0;                    ///< RenderTargetFormats::GetAnswers().size() at the end.
            int glErrorFrames = 0;                ///< Frames after which glGetError was not GL_NO_ERROR.
            std::map<GLint, long> drawFormats;    ///< Offscreen draws by the format they wrote.
            std::map<GLint, int> aliveFormats;    ///< Textures alive at the end, by format.
            int sharedReadyFrames = 0;
            std::map<GLint, int> sharedFormats;   ///< The shared buffer's formats while ready.
            std::vector<std::string> fieldLinks;  ///< The field programs' "linked" log lines.
            std::vector<std::string> unknownRows; ///< The startup report's UNKNOWN rows.
            std::vector<std::string> infos;
            std::vector<std::string> warnings;
            std::vector<std::string> errors;
        } RunResult;

        /// Frames a cell draws: enough for its variant's fields to settle.
        int FramesFor(Variant variant);
        const char *VariantName(Variant variant);

        /// The neon on @p scene, drawn @p variant, at CELL_SCALES[@p s].
        Cell NeonCell(const Scene &scene, Variant variant, size_t s);
        /// The flare alone, at CELL_SCALES[@p s]; below 1.0 its fullScaleName
        /// is the 1.0 cell's, what it draws when no scaled buffer allocates.
        Cell FlareCell(size_t s);
        /// Two lamps alone, at CELL_SCALES[@p s]; fullScaleName as FlareCell's.
        Cell SpotlightCell(size_t s);
        /// The scene called @p id, or nullptr.
        const Scene *FindScene(const char *id);

        /// The main cells: every check scene at 1.0, 0.5 and 0.25, still, hue
        /// and intensity; a flare and a spotlight at 0.5 and 1.0.
        std::vector<Cell> MakeCells();
        /// Cases D2, D3 and D5's cells - see MakeRetryCells in the .cpp.
        std::vector<Cell> MakeRetryCells();
        /// The size cases D2 and D3 fail allocations from, in texels.
        int64_t RetryThreshold();

        /// The message part of a library log line ("[...][LEVEL] file: func(line) > message").
        std::string MessageOf(const std::string &line);

        /// The report rows a run does not probe: a hook answers them instead.
        std::vector<GLint> Unprobed(const Hooks &hooks);
        /// The report's probes less @p unprobed.
        size_t ExpectedProbes(const std::vector<GLint> &unprobed);

        /// Draw every cell on a fresh effect - the neon, the flare, the
        /// spotlight and a reader of the neon's shared buffer - under
        /// @p hooks, and keep what it left.
        RunResult Run(const Hooks &hooks, bool bypassFields, const std::vector<Cell> &cells);

        bool Contains(const std::vector<GLint> &set, GLint format);
        const char *FormatName(GLint format);

        /// A run's summary lines, and the checks every run shares: the probe
        /// count and the GL errors. Returns the failures.
        int Summarise(const char *label, const RunResult &run, const std::vector<GLint> &unprobed, bool verbose);

        /// Largest channel difference, and share of lit channels more than 1
        /// level off, percent.
        void Compare(const RGBA &a, const RGBA &b, int &maxLevels, double &over1Pct);
        bool AllZero(const RGBA &frame);

        /// Cells whose frames differ by a byte between two runs; prints the
        /// first (every one with @p verbose), naming @p otherName.
        int CountDiffering(const RunResult &run, const RunResult &other, const char *otherName,
                           const std::vector<Cell> &cells, bool verbose);

        /// Draws into, and textures alive in, any format of @p set.
        void CountInFormats(const RunResult &run, const std::vector<GLint> &set, int &drawn, int &alive);

        /// The shared buffer was ready, and only ever in @p format.
        bool SharedOnlyIn(const RunResult &run, GLint format);

        /// Whether @p run's frame of the cell named @p name is, byte for byte,
        /// @p reference's frame of the cell named @p referenceName - and draws
        /// something. Prints the cell when it is not, or when either name is
        /// missing from @p cells, naming @p referenceLabel.
        bool SameFrame(const RunResult &run, const std::string &name, const RunResult &reference,
                       const std::string &referenceName, const char *referenceLabel, const std::vector<Cell> &cells);

        /// Lines of @p lines holding @p needle and, if given, @p also.
        int CountLines(const std::vector<std::string> &lines, const char *needle, const char *also = nullptr);

        /// B against A, the worst cell per group against
        /// FORMAT_QUALITY_BOUNDS, printed. Returns the failures.
        int CheckQualityBounds(const RunResult &a, const RunResult &b, const std::vector<Cell> &cells, bool verbose);

        /// Cases D4, D7, D5, D6 and S (format-check-causes.cpp): what each cause
        /// of a failed allocation may block, and a renderer outside an effect.
        /// @p a is case A's run on @p cells, the reference. Returns the
        /// failures.
        int CheckFailureCauses(const std::vector<Cell> &cells, const RunResult &a, bool verbose);
    }
}

#endif
