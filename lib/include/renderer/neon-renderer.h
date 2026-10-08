#ifndef _EDGE_LIGHTING_NEON_RENDERER_H_
#define _EDGE_LIGHTING_NEON_RENDERER_H_

#include "renderer/base-renderer.h"
#include "gl/shader-program.h"
#include "gl/uniform-buffer.h"
#include "gl/vertex-array.h"
#include "gl/framebuffer.h"
#include "renderer/span-atlas-lut.h"
#include "renderer/gradient-ring-lut.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace EdgeLighting
{
    /// Value types @ref NeonRenderer's members share with the helpers in its
    /// .cpp. Not part of the library's API.
    namespace NeonDetail
    {
        /// A rectangular annulus: an axis-aligned box with a smaller box cut out
        /// of its middle, both centred on the rect.
        ///
        /// @verbatim
        ///                outer.x
        ///            |<---------->|
        ///   +-------------------------+  ---
        ///   |     +-------------+     |   ^
        ///   |     |    hole     |     |   | outer.y
        ///   |     |      +      |     |   v       + = the rect's centre
        ///   |     |             |     |  ---
        ///   |     +-------------+     |
        ///   +-------------------------+
        ///         |<--->| hole.x
        /// @endverbatim
        ///
        /// @par Coordinates
        /// Both fields are HALF-EXTENTS in FULL-RES px, measured from the rect's
        /// centre: the box spans @c -outer to @c +outer and the hole @c -hole to
        /// @c +hole on each axis. An annulus carries no position of its own, and
        /// being symmetric it reads the same with y up or y down. To place one
        /// in the caller's framebuffer, add the rect's centre there -
        /// @c centerFull in @ref NeonRenderer::Render, viewport px from the
        /// lower-left corner, y up - so the box covers
        /// <tt>centerFull - outer</tt> to <tt>centerFull + outer</tt>, as
        /// GetBufferRegion and @ref NeonRenderer::computeRingFieldLayout do.
        ///
        /// @par Empty parts
        /// A hole of 0 on EITHER axis means there is none - a hole with no
        /// width or no height cuts nothing - so the annulus is the whole box.
        /// That is how PushAnnulus draws it and how AnnulusCovers compares two.
        /// An outer box of 0 on either axis is empty: PushAnnulus draws
        /// nothing.
        ///
        /// @par Units of what is built from it
        /// An annulus is in full-res px whichever pass draws it, so any two
        /// compare directly. A mesh built from one is in its own pass's space: the
        /// edge ring's and the blit's in full-res rect-local px, the same
        /// units, but the gather's and the glow quad's in SCALED px - which is
        /// why setupRingGeometry pushes the gather quad as
        /// <tt>outer * scale</tt> while @c mGatherBounds keeps the full-res
        /// values.
        ///
        /// Used for @c mGlowBounds (pass 1b's quad, the hole where the glow
        /// cannot reach - @ref NeonRenderer::setupGeometry), @c mRingBounds (the
        /// edge ring's band) and @c mGatherBounds (the gather quad, also cached
        /// in @c GatherCache::bounds) - the last two built by
        /// @ref NeonRenderer::setupRingGeometry.
        typedef struct Annulus
        {
            glm::vec2 outer{0.0f}; ///< The box's half width and half height, full-res px from the rect's centre.
            glm::vec2 hole{0.0f};  ///< The hole's half width and half height, the same; 0 on an axis where it has none.
        } Annulus;

        /// The part of the frame an offscreen buffer covers, and how many texels
        /// it takes to cover it. The offscreen buffers (the gather buffer, the
        /// reduced buffer, the glow's field) cover a region round the rect,
        /// not the whole viewport, so a rect smaller than the screen holds a
        /// fraction of a viewport-sized buffer.
        ///
        /// @verbatim
        ///                 origin + size
        ///   +----------------+
        ///   |   region       |    texels.x * texels.y texels,
        ///   |      +         |    each 1 / scale full-res px
        ///   |    centre      |    on a side
        ///   +----------------+
        ///   origin
        /// @endverbatim
        ///
        /// @par Coordinates
        /// @c origin and @c size are rect-local FULL-RES px: measured from the
        /// rect's centre, y up, so the region spans @c origin to
        /// <tt>origin + size</tt>. In the caller's framebuffer (viewport px
        /// from its lower-left corner, y up) that is <tt>centerFull +
        /// origin</tt> to <tt>centerFull + origin + size</tt>. @c texels is the
        /// buffer's own size - what its Framebuffer is allocated at.
        ///
        /// @par The texel grid
        /// Built by GetBufferRegion from a box's half-extents (an
        /// @ref Annulus's @c outer, or @c mScaledOuter) at a scale: the box
        /// clipped to the viewport, grown past its edge by the bilinear
        /// footprint. The region's corner is snapped to the scale's texel pitch
        /// in viewport space, so texel centres sit at <tt>(i + 0.5) /
        /// scale</tt> from the viewport's corner wherever the rect is - a
        /// moving rect reads the same texel centres - and the texel count is
        /// rounded up to REGION_ALLOC_STEP, so a moving or resizing rect does
        /// not reallocate the buffer every frame. So @c size is
        /// <tt>texels / scale</tt>, except on an axis capped to the whole
        /// viewport (the reduced buffer and the glow's field, which shares its
        /// region, when the region would reach it): there @c size is the
        /// viewport and @c texels <tt>floor(viewport * scale)</tt>. The gather
        /// buffer is never capped - see GetBufferRegion.
        ///
        /// @par Drawing and reading one
        /// A pass draws a region through GetRegionProjection(region, scale), the
        /// ortho from SCALED rect-local px onto it, into a buffer of @c texels.
        /// Its readers find it through a @ref UVMap from GetRegionUVMap. Held by
        /// @c GatherCache::region, so a gather that is still current is read
        /// through the region it was drawn over.
        typedef struct BufferRegion
        {
            glm::vec2 origin{0.0f}; ///< The lower-left corner, rect-local full-res px (from the rect's centre, y up).
            glm::vec2 size{0.0f};   ///< The extent, full-res px.
            glm::ivec2 texels{0};   ///< The buffer's size in texels.
        } BufferRegion;

        /// How a reader finds its texel in a buffer that covers a
        /// @ref BufferRegion: per axis, <tt>uv = px * scale + offset</tt>,
        /// where @c px is the reader's own @c vPos - rect-local px, from the
        /// rect's centre, y up.
        ///
        /// @par Which px
        /// A reader's @c vPos is in the space its pass draws in, so the map
        /// depends on the reader as well as the buffer. GetRegionUVMap(region,
        /// pxScale) builds it, @p pxScale being what one of the reader's px is
        /// in full-res px:
        ///   - the resolution scale for a pass drawn in SCALED space - the
        ///     shading (P1b), the field's bake (P1f) and composite (P1c);
        ///   - 1 for a pass drawn in full-res px - the blit (P2b), the edge ring
        ///     (P2c) and its field's bake (P1r) and composite (P2r).
        ///
        /// It sends the region's lower-left corner to uv 0 and its upper-right
        /// corner to uv 1: <tt>scale = 1 / (size * pxScale)</tt>,
        /// <tt>offset = -origin / size</tt>. @ref NeonRenderer::Render builds
        /// three a frame - the gather buffer from scaled px, the gather buffer
        /// from full-res px, the reduced buffer from full-res px - and the
        /// shaders take them as @c uGatherUVScale / @c uGatherUVOffset
        /// (neon.frag, neon-field.frag) and @c uUVScale / @c uUVOffset
        /// (neon-blit.frag). The default, all zeros, sends every px to uv 0.
        typedef struct UVMap
        {
            glm::vec2 scale{0.0f};  ///< uv per px of the reader: 1 / (region size * the reader's px scale).
            glm::vec2 offset{0.0f}; ///< uv at px 0, the rect's centre: -origin / size.
        } UVMap;
    }

    /// The neon renderer.
    ///
    /// Draws a tight quad over the rect + glow-reach margin and runs one
    /// fragment shader (neon.frag) that composes filament + halo + bloom from
    /// an analytic rounded-box SDF and what the gather pass left in its
    /// buffer.
    ///
    /// The gather (neon-gather.frag) is a loop over
    /// @c NeonConfig::numSamples perimeter samples (positions live in a UBO),
    /// run at its own coarse scale, that costs two @c texelFetch calls per
    /// sample into @c uEmission - the table baked by the emission pre-pass -
    /// or ONE where the config carries no segments, since the pre-pass then
    /// writes row 1 as all zeros and the shader takes a second, shorter loop
    /// body. That branch is on a uniform and sits OUTSIDE the loop; see the
    /// note at the loop in neon-gather.frag for why inside was measured
    /// slower than not branching at all. The per-sample arc scan, segment
    /// loop and filtered LUT reads that used to run in it are all
    /// fragment-invariant and moved to @c neon-emission.frag, which is why its
    /// cost no longer scales with the arc or segment count. See
    /// docs/emission-prepass.md.
    ///
    /// The three baked LUTs stay bound, but for the POINTWISE reads only - the
    /// colour-stop alpha, taken at the fragment's own perimeter position:
    ///   - @c uGradientLUT      - base colour RING (REPEAT, cyclic).
    ///   - @c uSegmentLUT       - per-segment atlas, one row per segment
    ///                            (CLAMP, head-to-tail SPAN).
    ///   - @c uArcLUT           - per-arc atlas, one row per arc (CLAMP, SPAN).
    ///
    /// Arc gating splits in two, and the halves use different rules on purpose.
    /// The gather's HUE resolves overlap winner-take-all per sample (largest
    /// @c mask*intensity), decided in the pre-pass. The EMISSION comes from
    /// @c arcCoverContinuous evaluated at the fragment's own perimeter
    /// position and combined across arcs with @c max. See neon.frag for the
    /// full compose. Visual parameters come from @c Config::neon;
    /// @c NeonConfig::arcs and @c NeonConfig::segmentBoosts drive their
    /// respective UBOs and atlases.
    ///
    /// --- Resolution scale -----------------------------------------------
    ///
    /// ONE path at every @c NeonConfig::resolutionScale, 1.0 included (I57;
    /// there was a direct path at 1.0 that shaded straight onto the target,
    /// removed once no shipped config used 1.0). The gather runs ALONE
    /// (neon-gather.frag) into @c mGather.buffer, at a scale set by its own
    /// smoothness rather than by @c resolutionScale - about 2 texels per
    /// colour kernel, so typically far coarser (@ref GetGatherScale). Then
    /// neon.frag, which reads that result back, shades @c mScaledBuffer at the
    /// requested fraction - full size at 1.0 - which is bilinear-blitted back
    /// over the area that can still be lit outside a thin ring around the rect
    /// edge, and the ring is re-shaded at full resolution. The loop - ~95% of
    /// the neon's cost when every pixel ran it - runs on a few thousand texels,
    /// while the filament, the cut and the cutoffs near the line come out at
    /// full resolution. Both buffers cover only the part of the frame their
    /// readers reach (@ref GetBufferRegion). See
    /// docs/neon-resolution-scale-plan.md sections 12 and 13.
    ///
    /// With no segments and a colour-stop alpha time cannot move, the shading
    /// is factored: neon.frag's output is tonemap(col * Fa) with only the
    /// gathered hue col changing, so pass 1f bakes Fa into @c mGlowField.buffer
    /// once the config has held for a frame, and pass 1c (neon-field.frag)
    /// composites it with the hue in pass 1b's place, into @c mScaledBuffer -
    /// under a rotating hue, the frames that would otherwise redraw pass 1b
    /// with only the time moved, 1.1-2.0x on them, and under an intensity
    /// animation, scaled (I56). The ring has a field of its own (pass 1r / 2r),
    /// still frames included. A frame whose config just changed shades
    /// directly. See docs/neon-perf-plan.md section 11, and I46, I50, I52 and
    /// I56 in docs/review-findings.md.
    ///
    /// Every pixel-valued uniform is multiplied by the scale (a no-op at 1.0)
    /// and the shader converts its own full-res px constants with
    /// @c uResolutionScale; the ring takes them at 1.0, and @c uBlitOwnsCut
    /// says which passes leave the cut and the cutoffs to the blit.
    ///
    /// The neon programs and the blit are built on the first frame
    /// (@ref ensureGlowPrograms): the gather, the shading twice - one object
    /// per target - and the blit. The fields' three programs are built on the
    /// first frame a config they can serve is drawn (@ref ensureFieldPrograms,
    /// @ref ensureRingFieldPrograms), and the glow coverage bake on the first
    /// frame that bakes the table (@ref ensureGlowCoverProgram).
    ///
    /// Debug overlays (LUT strip, colour-stop markers) are NOT here - they are
    /// a separate layer, @ref DebugRenderer, driven by @ref DebugConfig. The
    /// one debug field this renderer does read is
    /// @c DebugConfig::opaqueOnly, which selects which of its passes run and
    /// so cannot live anywhere but in the schedule below.
    class NeonRenderer : public BaseRenderer
    {
    public:
        /// Creates the renderer's GL objects - its meshes' vertex arrays, its
        /// uniform buffers, its LUT textures - so a GL context must be current.
        /// Builds no program and allocates no buffer: see @ref Initialize.
        NeonRenderer() = default;
        /// Releases every GL object it holds, through their RAII wrappers; the
        /// context they were created in must be current.
        virtual ~NeonRenderer() = default;

        /// Builds the one program every frame uses (the emission pre-pass),
        /// allocates the emission table, declares the meshes' vertex format,
        /// and builds the CPU-side inputs and the geometry from the config the
        /// last @ref OnConfigChanged stored - the loop samples, the glow quad,
        /// the fill's band, the ring / blit / gather meshes, the LUTs.
        ///
        /// Every other program is built on the first frame that needs it, so
        /// a @c neon.frag that fails to compile does not fail this call: the
        /// frame then draws the fill alone (see @ref ensureProgram).
        /// @return false if the emission program fails to build, or no format
        ///         for the emission table allocates.
        virtual bool Initialize() override;

        /// Per-frame bookkeeping, before @ref Render; draws nothing. Advances
        /// the colour ring's cross-fade (a fade frame re-uploads the ring,
        /// which moves its upload count and so re-bakes the emission table),
        /// and gives the glow coverage table back once nothing has read it for
        /// GLOW_COVER_RELEASE_SECONDS of frame time - here and not in
        /// @ref Render, which must not delete a framebuffer.
        /// @param deltaTime  Seconds since the last frame; the release timer's step.
        /// @param time       Unused: the neon reads time in @ref Render.
        /// @param config     The active configuration.
        virtual void Update(float deltaTime, float time, const Config &config) override;

        /// Draws the neon, as a schedule of passes in two phases. Nothing when
        /// @c NeonConfig::enable is off.
        ///
        /// Phase 1, offscreen, each pass only when stale: the emission table
        /// (P0), the glow coverage table (P0b), the gather (P1a), the glow's
        /// field bake (P1f), the reduced buffer - shaded (P1b) or composited
        /// from the field (P1c) - and the ring's field bake (P1r). On a frame
        /// where nothing they read moved, none of it runs and the frame never
        /// leaves the caller's framebuffer. Phase 2, onto the caller's
        /// framebuffer, premultiplied over: the opaque fill (P2a), the blit
        /// (P2b), the edge ring - shaded (P2c) or from its field (P2r). The
        /// passes in their declaration order below, and
        /// docs/neon-shader-outputs.html, show each one.
        ///
        /// With @c DebugConfig::opaqueOnly, the fill alone. A program that
        /// fails to build leaves the frame the fill alone too.
        ///
        /// @par GL state
        /// Restores the framebuffer and the viewport it was handed - whatever
        /// framebuffer that is, never framebuffer 0 - and lifts the host's
        /// scissor only for its offscreen passes. Owns blending: a frame that
        /// draws returns with @c GL_BLEND on and @c GL_SRC_ALPHA,
        /// @c GL_ONE_MINUS_SRC_ALPHA (a disabled layer touches nothing).
        /// It sets nothing else the host owns, so the host supplies: no face
        /// culling (or a front face the meshes' winding survives), a full
        /// colour and alpha write mask, @c GL_FUNC_ADD, and depth test and
        /// write off.
        /// @pre The GL viewport is (0, 0, @p viewportWidth, @p viewportHeight)
        ///      - see @ref BaseRenderer::Render; and @ref OnConfigChanged has
        ///      run for any change to @p config, which the effect does before
        ///      it forwards @ref Update.
        /// @param viewportWidth  The caller's framebuffer width, px.
        /// @param viewportHeight Its height, px.
        /// @param time           Seconds of animation time: what the hue rotates by.
        /// @param config         The active configuration.
        virtual void Render(int viewportWidth, int viewportHeight, float time, const Config &config) override;

        /// The composited configuration changed. Records what moved and
        /// rebuilds only what that feeds: the loop samples (the geometry, the
        /// resolution scale, the sample count), the glow quad and the ring /
        /// blit / gather meshes (those three, the glow radius, bloom,
        /// intensity, line width, filament falloff, glow side and cutoffs, or
        /// the brightest emission the arcs and segments carry), the fill's
        /// band (the geometry and the fill's own mode and cutoffs), the LUTs
        /// (which guard themselves). The light blocks
        /// and the glow coverage table's pieces are only marked dirty here;
        /// @ref Render repacks and re-bakes them.
        ///
        /// Also clears what the change makes stale: the offscreen phase's
        /// reuse on any change to the neon or the geometry, and both fields
        /// on any of those but the intensity alone. Releases the reduced
        /// buffer, the gather buffer and the glow coverage table when the layer
        /// is disabled, and a field's buffer when the config can no longer use
        /// it.
        ///
        /// Can run more than once between two frames, and before
        /// @ref Initialize - so its flags accumulate, and its rebuilds wait
        /// for @c mInitialized.
        /// @param config  The new composited configuration.
        virtual void OnConfigChanged(const Config &config) override;

        /// @return @ref RendererLayer::NEON.
        virtual RendererLayer GetLayer() const override { return RendererLayer::NEON; }

    private:
        // --- Types -----------------------------------------------------------
        typedef NeonDetail::Annulus Annulus;
        typedef NeonDetail::BufferRegion BufferRegion;
        typedef NeonDetail::UVMap UVMap;

        /// A vertex array and how many vertices it holds: the geometry of every
        /// quad and @ref Annulus this renderer draws - @c mGlowMesh,
        /// @c mFillMesh, @c mRingMesh, @c mBlitMesh and @c mGatherMesh.
        ///
        /// @par Format
        /// Triangles (@c GL_TRIANGLES), one @c vec2 position per vertex at
        /// attribute 0, tightly packed - so @c count is the number of floats
        /// over two, and a quad is 6 vertices. The positions are rect-local px
        /// in the space of the pass that draws the mesh: SCALED px for the glow
        /// quad and the gather quad, FULL-RES px for the fill, the edge ring
        /// and the blit (see each member). The attribute format is declared
        /// once, on @c vertexArray, by @ref NeonRenderer::Initialize: a VAO
        /// remembers it and the buffer it reads, and neither changes, so an
        /// @ref Upload does not repeat it.
        ///
        /// @par Lifetime
        /// The geometry builders (@ref NeonRenderer::setupGeometry,
        /// @ref NeonRenderer::setupFillGeometry,
        /// @ref NeonRenderer::setupRingGeometry) @ref Upload a mesh whenever
        /// the geometry it bounds changes, @c GL_DYNAMIC_DRAW. A Mesh holds a
        /// VertexArray, which creates its GL names in its constructor - so a
        /// context must be current when the renderer is constructed - and
        /// which is move-only, so a Mesh cannot be copied.
        ///
        /// @par An empty mesh
        /// A @c count of 0 draws nothing, and is how a builder says there is
        /// nothing to draw: the blit's area when no pixel outside the ring can
        /// be lit, the fill's band when there is no fill or it covers every
        /// pixel and a clear draws it instead (set straight to 0, no upload).
        /// The passes test @c count to skip their work, so keep it in step
        /// with what was uploaded - @ref Upload does that.
        typedef struct Mesh
        {
            /// @p name labels the vertex array in log messages. @p initialCount is
            /// @c count before the first @ref Upload; @ref NeonRenderer::Initialize
            /// uploads every mesh before anything draws, so it is only a
            /// starting value (@c mGlowMesh's 6, its plain quad).
            explicit Mesh(const char *name, int initialCount = 0) : vertexArray(name), count(initialCount) {}
            /// Replace the vertices with @p floatCount floats from @p data -
            /// two per vertex, in triangles - and set @c count to match.
            void Upload(const float *data, size_t floatCount);
            /// Draw @c count vertices as triangles. Binds the vertex array and
            /// nothing else: the caller has its program in use and its uniforms,
            /// the transform among them, uploaded.
            void Draw();

            VertexArray vertexArray; ///< The VAO and its one VBO.
            int count = 0;           ///< Vertices in it; 0 when there is nothing to draw.
        } Mesh;

        /// Everything pass 0 reads besides the time, AS THE PASS SEES IT: its
        /// two value uniforms, and a version number for each texture and
        /// uniform block it binds. See @ref isEmissionTableStale.
        ///
        /// @par Two kinds of field
        ///   - The VALUE uniforms, @c hueRotationRate and @c numSamples,
        ///     compared by value.
        ///   - The GL objects the pass binds - the three LUTs and the two light
        ///     blocks - compared by their upload counts, a version number on
        ///     each object, not by their contents or by the config fields that
        ///     fill them. A uniform block counts only uploads that changed its
        ///     bytes; a LUT counts every upload, a cross-fade frame's included.
        ///
        /// Keyed on what the pass binds rather than on a list of config fields,
        /// so no list can fall out of step with the shader: an intensity,
        /// bloom, glow, geometry or other-layer animation moves none of these
        /// and re-bakes nothing, and anything that does move the table moves
        /// one of them.
        ///
        /// @par Left out
        /// @c uTime, which @ref EmissionBake::time covers - and only while the
        /// hue rotates, since at a rate of 0 it drops out of the table - and
        /// @c uMVP, the identity on every frame.
        ///
        /// @par Lifetime
        /// Built by @ref getEmissionInputs, from exactly what
        /// @ref renderEmissionPass uploads and binds, after
        /// @ref packLightBlocks has run this frame (the blocks' counts move
        /// only there); kept in @ref EmissionBake::inputs by each bake.
        ///
        /// @warning Add an input to @ref renderEmissionPass and it belongs here
        /// - a field, a term in @c operator== and a line in
        /// @ref getEmissionInputs - or the table goes stale whenever that
        /// input alone moves.
        typedef struct EmissionInputs
        {
            float hueRotationRate = 0.0f;     ///< uHueRotationRate.
            int numSamples = 0;               ///< uNumSamples, clamped as the pass uploads it.
            uint32_t gradientUploads = 0;     ///< mGradientLUT.GetUploadCount() - uGradientLUT.
            uint32_t segmentAtlasUploads = 0; ///< mSegmentLUT.GetUploadCount() - uSegmentLUT.
            uint32_t arcAtlasUploads = 0;     ///< mArcLUT.GetUploadCount() - uArcLUT.
            uint32_t segmentBlockUploads = 0; ///< mLightBlocks.segment.GetUploadCount() - SegmentBlock.
            uint32_t arcBlockUploads = 0;     ///< mLightBlocks.arc.GetUploadCount() - ArcBlock.

            bool operator==(const EmissionInputs &o) const
            {
                return hueRotationRate == o.hueRotationRate && numSamples == o.numSamples &&
                       gradientUploads == o.gradientUploads && segmentAtlasUploads == o.segmentAtlasUploads &&
                       arcAtlasUploads == o.arcAtlasUploads && segmentBlockUploads == o.segmentBlockUploads &&
                       arcBlockUploads == o.arcBlockUploads;
            }
            bool operator!=(const EmissionInputs &o) const { return !(*this == o); }
        } EmissionInputs;

        /// What pass 0 last wrote into @c mEmission.buffer: the record that lets
        /// a frame skip the pass when nothing it reads has moved.
        ///
        /// @par Written
        /// Only by @ref NeonRenderer::renderEmissionPass, on its way out, after
        /// the draw - the only thing that ever writes the buffer, which is
        /// allocated once in @ref NeonRenderer::Initialize and never released.
        /// So the record never has to be reset: it describes the buffer for the
        /// renderer's whole life.
        ///
        /// @par Read
        /// By @ref NeonRenderer::isEmissionTableStale, which says the table
        /// has to be baked again when any of these holds:
        ///   - @c baked is false - nothing has been written yet;
        ///   - @ref NeonRenderer::getEmissionInputs differs from @c inputs
        ///     - a value uniform, or the upload count of a LUT or a light
        ///     block the pass binds, moved;
        ///   - the hue rotates (@c hueRotationRate non-zero) and the frame's
        ///     time differs from @c time. At a rate of 0 the table does not
        ///     depend on time, and @c time is not consulted.
        ///
        /// And by @ref NeonRenderer::getGatherInputs, for @c version: the
        /// gather reads the table, so a fresh bake has to change the gather's
        /// key too, and the version is how it does without comparing texels.
        typedef struct EmissionBake
        {
            bool baked = false;    ///< Written at all: until then the buffer holds undefined texels, whatever @c inputs holds.
            EmissionInputs inputs; ///< The inputs other than time it was baked from.
            float time = 0.0f;     ///< The time it was baked at; consulted only while the hue rotates.
            uint32_t version = 0;  ///< How many times it has been written - the table's version, for @ref GatherInputs. Only ever compared for equality.
        } EmissionBake;

        /// Everything pass 1a reads besides the region it draws and the quad
        /// it draws over: what neon-gather.frag is handed
        /// (@ref uploadShapeUniforms, @ref bindGatherInputs) and the buffer it
        /// draws into. The table and the sample block by version, as
        /// @ref EmissionInputs keys them. See @ref getGatherInputs.
        ///
        /// @par What each field stands for
        ///   - The table and the samples by version: @c emissionVersion moves
        ///     with every bake of pass 0 (@ref EmissionBake::version) and
        ///     @c sampleUploads with every change to the sample positions.
        ///     Under a rotating hue the table is baked on every frame, so the
        ///     gather runs on every hue frame too.
        ///   - The shape as the pass uploads it, in SCALED px: @c scale,
        ///     @c rectSize, @c cornerRadius, and @c numSamples clamped.
        ///   - @c attachments, 1 or 2: segments add the second attachment and
        ///     pick the shader's second loop body.
        ///   - @c viewport and @c center are no uniform at all: they place the
        ///     buffer's texel grid, which is anchored to the viewport's corner
        ///     (@ref BufferRegion). A texel holds the same value only while the
        ///     rect sits at the same place in the same viewport.
        ///
        /// @par Left out
        /// The region and the quad, which @ref GatherCache covers instead - so
        /// that a quad that only grows or shrinks (an animation of the glow's
        /// reach) does not make the gather run - and the time, which reaches
        /// the gather only through the table.
        ///
        /// @par Lifetime
        /// Built by @ref getGatherInputs, after pass 0 has run this frame if
        /// it is going to (the table's version is part of the key); kept in
        /// @ref GatherCache::inputs by each gather.
        ///
        /// @warning Add an input to @ref renderGatherPass and it belongs here
        /// - a field, a term in @c operator== and a line in
        /// @ref getGatherInputs - or the gather is skipped on a frame that
        /// input alone moved: a stale image, not wasted work.
        typedef struct GatherInputs
        {
            uint32_t emissionVersion = 0; ///< mEmission.bake.version - what uEmission holds.
            uint32_t sampleUploads = 0;   ///< mLoopSamplesBlock.GetUploadCount() - LoopSamplesBlock.
            int numSamples = 0;           ///< uNumSamples, clamped as the pass uploads it.
            float scale = 0.0f;           ///< The resolution scale - the space of the shape and the projection.
            glm::vec2 rectSize{0.0f};     ///< uRectSize.
            float cornerRadius = 0.0f;    ///< uCornerRadius.
            int attachments = 0;          ///< 2 with segments (uSegmentCount > 0 picks the loop body), else 1.
            glm::ivec2 viewport{0};       ///< The viewport the region's texel grid is anchored to, and clipped to.
            glm::vec2 center{0.0f};       ///< The rect's centre in it, full-res px, y up - where that grid and clip fall on the rect.

            bool operator==(const GatherInputs &o) const
            {
                return emissionVersion == o.emissionVersion && sampleUploads == o.sampleUploads &&
                       numSamples == o.numSamples && scale == o.scale && rectSize == o.rectSize &&
                       cornerRadius == o.cornerRadius && attachments == o.attachments && viewport == o.viewport &&
                       center == o.center;
            }
            bool operator!=(const GatherInputs &o) const { return !(*this == o); }
        } GatherInputs;

        /// What pass 1a last drew into @c mGather.buffer, so a frame whose
        /// gather would draw the same values can skip it (Render). Every texel
        /// is a function of its position and @c inputs alone, on a texel grid
        /// anchored to the viewport rather than to the region, so while the
        /// inputs hold the buffer answers any quad inside @c bounds: readers
        /// then read it through @c region. That is what an animation of the
        /// glow's reach - an intensity or bloom pulse, a glow-radius or cutoff
        /// sweep - needs, since it moves the quad every frame and nothing the
        /// gather computes.
        ///
        /// @par When the buffer still answers
        /// @ref NeonRenderer::Render skips pass 1a, and reads the buffer
        /// through @c region, when all of these hold:
        ///   - @c drawn - the last gather succeeded;
        ///   - @c mGather.buffer is still allocated - it is released when the
        ///     layer is disabled, which @c drawn alone would not see;
        ///   - @ref NeonRenderer::getGatherInputs equals @c inputs - among
        ///     them the emission table's version (@ref EmissionBake), so a
        ///     fresh table makes the gather run again;
        ///   - @c bounds covers this frame's @c mGatherBounds (AnnulusCovers):
        ///     the quad the readers need lies inside the one that was drawn.
        ///
        /// Otherwise the gather runs over this frame's quad and region, and the
        /// cache is replaced whole with what it drew - including a failure, as
        /// @c drawn false. Under an animation of the glow's reach the gather
        /// therefore runs only while the quad grows past the last one drawn,
        /// and stops once the quad has been at its widest.
        ///
        /// @par Not byte-identical
        /// A held region reaches the same texels through another projection,
        /// so a frame that reuses the buffer reads within 1 level of one that
        /// gathers afresh, not bit for bit (I55).
        typedef struct GatherCache
        {
            bool drawn = false;  ///< The buffer holds a gather at all.
            GatherInputs inputs; ///< What it was drawn from.
            Annulus bounds;      ///< The quad it covered, as @c mGatherBounds was then.
            BufferRegion region; ///< The region it was laid out over (GetBufferRegion).
        } GatherCache;

        /// When a field is baked and when it is read - the schedule the
        /// shading's field (@c mGlowField.schedule) and the edge ring's (@c mRingField.schedule)
        /// share. A field outlives the frame it was baked on; a config change
        /// (@ref OnConfigChanged calls @ref Invalidate), the viewport and a
        /// gradient ring upload - a cross-fade frame - invalidate it.
        ///
        /// The bake is LAZY: only a config that has held for a frame is baked
        /// (@c settled), so an animation that changes the config every frame
        /// keeps shading directly and never pays a bake it cannot reuse - a
        /// bake AND a composite on every frame measured 5-7% slower than
        /// shading directly.
        ///
        /// Except on the FIRST frame a renderer draws a config the field can
        /// serve, which builds the field's programs and bakes it at once,
        /// while still shading directly. Building alone is not enough: this
        /// driver finishes a program's compile on its first DRAW, in the
        /// target and blend state it draws with, so a bake program only linked
        /// there left a ~125 ms stall on the bake frame after it (~155 ms all
        /// told, AMD Radeon Pro 5300M). And that frame still shades directly
        /// so that the shading program's first draw lands there too -
        /// compositing instead deferred THAT stall to the first config change.
        /// The price is one bake an animated config may not reuse, once per
        /// renderer.
        ///
        /// The neon's INTENSITY is the one config change that does not
        /// invalidate a field (I56): neon.frag's output is near enough
        /// I * G with G fixed while only the intensity moves, so a field baked
        /// at intensity I0 serves intensity I as Fa * I / I0 (@c Decision::gain).
        /// Near enough, not exactly: the bloom's reach and the quad's fade and
        /// hole also follow the intensity, and the field holds them as they
        /// were at I0 - up to 2 levels on the default look across
        /// IntensityPulse's 0.4-1.0, 6 on a 1.5 bloom (docs/neon-reduced-scale-
        /// plan.md, D1). So only while the intensity is MOVING, and only below
        /// I0, whose quad covers every dimmer one's; a brighter frame bakes
        /// again at its own intensity, so a pulse settles on its peak after
        /// one cycle; and the frame the intensity stops on bakes again too,
        /// exactly - a held config never shows a scaled field.
        ///
        /// @par One frame
        /// @ref Decide, in this order - the first row that holds decides:
        /// | State                                      | Decision            | Why |
        /// | ------------------------------------------ | ------------------- | --- |
        /// | viewport or gradient upload count moved    | (invalidates, then reads on) | what the buffer holds was drawn for another frame |
        /// | @c current, intensity unchanged            | use                 | the field still describes this frame |
        /// | @c current, intensity moving, 0 <= I < I0  | use, gain I / I0    | serve the dimmer frame scaled (I56) |
        /// | @c current, any other intensity            | bake + use          | brighter, or stopped: bake again, exactly |
        /// | @c settled                                 | bake + use          | the config held for a frame: a bake now is reused |
        /// | @p firstFrame                              | bake                | warm the programs' first draws; still shade directly |
        /// | otherwise                                  | nothing; sets @c settled | the config just changed: shade directly, bake next frame if it holds |
        ///
        /// @par Who calls what
        ///   - @ref NeonRenderer::Render calls @ref Decide once a frame, and
        ///     only for a config the field can serve and a field not
        ///     @c unavailable; then, after a bake, @ref OnBaked.
        ///   - @ref NeonRenderer::OnConfigChanged calls @ref Invalidate on any
        ///     change to the neon or the geometry but the intensity alone, and
        ///     clears @c current itself when it releases the buffer for a
        ///     config that cannot use it.
        ///   - A failed program build sets @c unavailable
        ///     (@ref NeonRenderer::ensureFieldPrograms,
        ///     @ref NeonRenderer::ensureRingFieldPrograms), as a failed bake
        ///     does through @ref OnBaked: no half-float target on this driver.
        typedef struct LazyBake
        {
            /// What @ref Decide says a frame does with the field:
            ///   - neither: shade directly (pass 1b, or the ring's pass 2c);
            ///   - @c bake alone: shade directly, and bake the field too - the
            ///     first frame only;
            ///   - @c bake and @c use: bake, then composite what was baked;
            ///   - @c use alone: composite the field already held, times
            ///     @c gain.
            /// A bake that fails leaves @c use off for the frame (Render).
            typedef struct Decision
            {
                bool use = false;  ///< Composite the field in the shading's place.
                bool bake = false; ///< Bake it first, after the gather it reads.
                float gain = 1.0f; ///< What the composite multiplies the field by: I / I0, exactly 1 unless scaled.
            } Decision;

            bool current = false;         ///< The buffer describes this frame up to its intensity: nothing else invalidated it since its bake.
            bool settled = false;         ///< Nothing invalidated it on the last frame, so a bake now would be reused.
            bool unavailable = false;     ///< Not on this driver (a program failed, or no half-float target): shade directly.
            glm::ivec2 viewport{0};       ///< The viewport it was decided for.
            uint32_t gradientUploads = 0; ///< The gradient ring's upload count it was decided for.
            float intensity = 0.0f;       ///< NeonConfig::intensity it was baked at (I0).

            /// The schedule's step for a frame whose config the field can
            /// serve, drawn at @p frameViewport with the gradient ring at
            /// @p frameGradientUploads and the neon at @p frameIntensity, which
            /// a config change moved since the last frame if
            /// @p intensityMoving; @p firstFrame when the field's programs were
            /// not built before this frame. See the table above. Records the
            /// viewport and the upload count, and may set @c settled; the bake
            /// itself is recorded by @ref OnBaked.
            Decision Decide(const glm::ivec2 &frameViewport, uint32_t frameGradientUploads, bool firstFrame,
                            float frameIntensity, bool intensityMoving);

            /// The config changed: what the buffer holds is stale, and the
            /// next frame is not settled - so the frame after it shades
            /// directly, and the bake waits for a config that holds.
            void Invalidate()
            {
                current = false;
                settled = false;
            }

            /// A bake ran, at @p bakedIntensity: what the buffer holds is
            /// current if it @p baked, and the field is never used again on
            /// this driver if not - a bake fails only when the buffer cannot be
            /// allocated, i.e. no half-float render target.
            void OnBaked(bool baked, float bakedIntensity)
            {
                current = baked;
                unavailable = !baked;
                intensity = bakedIntensity;
            }
        } LazyBake;

        /// How the ring's pixels are packed into @c mRingField.buffer. The ring
        /// is an axis-aligned annulus (setupRingGeometry's PushAnnulus), so a
        /// box of pixels minus a hole: its bottom and top strips are stored as
        /// they are, one above the other, and its left and right strips
        /// TRANSPOSED above those, so each runs along the atlas's width. Every
        /// texel sits on one of the viewport's pixels, so the composite reads
        /// the texel of its own pixel.
        /// neon-field.frag (NEON_FIELD_RING) reads this layout and
        /// @ref renderRingFieldPass bakes it: change the three together.
        ///
        /// @par The box and its strips
        /// In box px, y up, with @c hole = (x0, y0, x1, y1):
        /// @verbatim
        ///   size.y +-------------------------+
        ///          |          top            |
        ///       y1 +------+-----------+------+
        ///          |      |           |      |
        ///          | left |   hole    | right|
        ///          |      |           |      |
        ///       y0 +------+-----------+------+
        ///          |         bottom          |
        ///        0 +-------------------------+
        ///          0      x0          x1     size.x
        /// @endverbatim
        ///
        /// @par The atlas
        /// The same strips in texels, y up; the side strips transposed, so a
        /// column of the box becomes a row of the atlas:
        /// @verbatim
        ///   atlas.y +----------------+
        ///           | right, x <-> y |   x1 .. size.x as rows, y0 .. y1 along
        ///    rows.y +----------------+
        ///           | left,  x <-> y |   0 .. x0 as rows, y0 .. y1 along
        ///    rows.x +----------------+--------+
        ///           |          top            |
        ///        y0 +-------------------------+
        ///           |         bottom          |
        ///         0 +-------------------------+
        /// @endverbatim
        /// So <tt>rows.x = y0 + (size.y - y1)</tt>, <tt>rows.y = rows.x +
        /// x0</tt>, and the atlas is <tt>max(size.x, y1 - y0)</tt> wide by
        /// <tt>rows.y + (size.x - x1)</tt> tall - with no hole the side strips
        /// are empty and it is just the box. About 0.27 MB of R16F for a
        /// 960 x 540 rect at 1080p, against 1.06 MB as the plain box.
        ///
        /// @par Finding a pixel's texel
        /// The composite takes its box pixel <tt>q = floor(vPos - origin)</tt>
        /// and, by the first strip that holds it:
        ///   - bottom, <tt>q.y < y0</tt>: texel @c q;
        ///   - top, <tt>q.y >= y1</tt>: <tt>(q.x, q.y - y1 + y0)</tt>;
        ///   - left, <tt>q.x < x0</tt>: <tt>(q.y - y0, rows.x + q.x)</tt>;
        ///   - right: <tt>(q.y - y0, rows.y + q.x - x1)</tt>.
        ///
        /// @par Built
        /// By @ref computeRingFieldLayout from @c mRingBounds, every frame the
        /// ring's field can serve; kept in @c mRingField.layout only on a frame
        /// that bakes, since the composite has to read the layout the field was
        /// baked with. The box is every pixel whose centre the ring's outer box
        /// can reach, plus a pixel of slack each side, clipped to the viewport.
        /// The hole is CONSERVATIVE - only pixels whose centre lies more than a
        /// pixel inside the ring's hole - so every pixel the ring can draw falls
        /// in a strip; with no such pixel (a hole of a pixel or less) there is
        /// no hole, and the bottom strip is the whole box.
        typedef struct RingFieldLayout
        {
            glm::vec2 origin{0.0f}; ///< The box's lower-left pixel corner, rect-local full-res px - a whole viewport pixel minus centerFull.
            glm::ivec2 size{0};     ///< The box, in px: the ring's outer box, clipped to the viewport.
            glm::ivec4 hole{0};     ///< Box px the ring never draws, [x0, x1) x [y0, y1) as x0, y0, x1, y1.
            glm::ivec2 rows{0};     ///< First atlas row of the left strip and of the right strip.
            glm::ivec2 atlas{0};    ///< The buffer's size, in texels.
        } RingFieldLayout;

        /// The two limits a neon.frag program shades within, as a pair: the
        /// quad's fade margin and the corner-arc skip, in the px of the pass
        /// that draws. One for pass 1b and its field's bake, scaled
        /// (@c mGlowLimits), one for the edge ring and its field's bake, at
        /// scale 1.0 (@c mRingLimits) - both set by @ref setupGeometry and
        /// handed to @ref uploadNeonUniforms together.
        typedef struct ShadeLimits
        {
            float quadMargin = 0.0f; ///< uQuadMargin: the draw quad's margin past the rect edge; the shader fades the bloom out by here.
            float cornerSkip = 0.0f; ///< uCornerSkip: a corner arc is skipped whole this far from its circle - see GetCornerSkip.
        } ShadeLimits;

        /// The two light blocks - the segments and the arcs - and whether
        /// they still hold the current config. Read by the emission pre-pass,
        /// the coverage bake and every neon.frag program; packed by
        /// @ref packLightBlocks.
        typedef struct LightBlocks
        {
            /// Backs neon.frag's std140 `SegmentBlock` (DALi-compatible uniform
            /// block holding uSegmentCount + uSegments[]).
            UniformBuffer segment{"NeonRenderer.SegmentBlock"};

            /// Backs neon.frag's std140 `ArcBlock` (uArcCount + uArcs[MAX_ARCS]).
            UniformBuffer arc{"NeonRenderer.ArcBlock"};

            /// Whether @c mLightBlocks.segment / @c mLightBlocks.arc still hold the current
            /// config. Cleared by @ref packLightBlocks once it has repacked.
            ///
            /// NOT set on every config change, as the emission table's flag used
            /// to be: the blocks are packed from @c mEffectiveSegments and
            /// @c NeonConfig::arcs and nothing else, so @ref OnConfigChanged gates
            /// it on exactly those two. It accumulates rather than being assigned,
            /// because that call can run more than once before the next
            /// @ref Render - see the note there.
            ///
            /// Starts true because @ref Initialize does not pack - the first
            /// @ref Render is what fills them.
            bool dirty = true;
        } LightBlocks;

        /// Pass 0's target and what it holds: the per-sample emission table and
        /// the record that lets a frame skip re-baking it.
        typedef struct EmissionTable
        {
            /// Perimeter emission table, NEON_MAX_LOOP_SAMPLES x 2 (see
            /// neon-emission.frag for the row packing). Written by
            /// @ref renderEmissionPass and read by the gather with texelFetch.
            /// Rebuilt only when @ref isEmissionTableStale says its inputs moved -
            /// the buffer is allocated once and nothing else writes it, so its
            /// contents survive between frames.
            ///
            /// Also remembers, on the renderer's behalf, whether the driver would
            /// give it RGBA16F: @ref renderEmissionPass asks a live buffer for the
            /// format it already holds, so the RGBA8 fallback sticks without a flag
            /// here to say so.
            Framebuffer buffer{"NeonRenderer.Emission"};
            EmissionBake bake; ///< What @c buffer holds - see @ref EmissionBake.
        } EmissionTable;

        /// Pass 0b's target and its state: the glow coverage table, the format
        /// the driver gave it, which of its pieces need a bake, and how long
        /// it has gone unread.
        typedef struct GlowCoverTable
        {
            /// Glow coverage table, GLOW_COVER_WIDTH wide by four bands of
            /// GLOW_COVER_ROWS, each holding one straight and one corner, which
            /// share its columns in proportion to their lengths (GetGlowCoverSplit):
            /// for each piece of the emitter and each fragment position round it,
            /// how lit the arcs and the segments make that piece as each layer sees
            /// it - see neon-glow-cover.frag, laid out in neon-pieces.glsl. Written by @ref renderGlowCoverPass, read by every
            /// neon.frag program with one filtered fetch per piece. Allocated on
            /// the first frame that bakes it (@ref ensureGlowCoverBuffer), never
            /// for a ring lit uniformly, and released when the layer is disabled
            /// or once nothing has read it for GLOW_COVER_RELEASE_SECONDS
            /// (@ref Update); two channels while the config has no segments. Its
            /// lengths are ratios that scale together, so every resolution scale
            /// shares it.
            Framebuffer buffer{"NeonRenderer.GlowCover"};

            /// Index into the glow coverage table's format lists of the best
            /// format the driver has not refused: the tier ResizeInBestFormat
            /// resumes from, one for both lists. Only ever advances - see
            /// @ref ensureGlowCoverBuffer.
            size_t format = 0;

            /// Which pieces of @c mGlowCover.buffer have to be re-baked: bit 2b is
            /// band b's straight, bit 2b + 1 its corner, all eight when the shape
            /// moves. NOT set on every config change, as the emission table's
            /// flag used to be: the bake
            /// reads the two light blocks (the arcs and the effective segments),
            /// the rect's width, height, corner radius and winding, and the glow
            /// radius - every one of them in @ref renderGlowCoverPass or the
            /// blocks it binds - so @ref OnConfigChanged gates it on exactly
            /// those, as it gates @c mLightBlocks.dirty. The bake is the costly
            /// pass (0.14-0.27 ms on an AMD Radeon Pro 5300M), and "any change"
            /// re-ran it every frame under an intensity or colour animation, or an
            /// animation of another LAYER's fields, none of which it reads.
            ///
            /// Per PIECE because each piece's texels integrate the lights over that
            /// piece alone: a changed arc or segment dirties only the pieces its
            /// old and new supports reach (GetGlowCoverDirtyArcPieces,
            /// GetGlowCoverDirtySegmentPieces), so a segment
            /// travelling along one straight re-bakes that band rather than the
            /// table - 1.18-1.91x on such frames on an Apple M2 Pro
            /// (docs/neon-perf-plan.md, item 5). The shape, the winding and the
            /// glow radius dirty all eight, as do the brightest arc's intensity
            /// and any light covering the whole ring.
            ///
            /// Accumulated, never assigned, for @c mLightBlocks.dirty's reasons, and
            /// cleared only by the bake - so a change made while the bake is
            /// skipped (a ring lit uniformly) is still pending when it next runs.
            /// Also set whole by @ref ensureGlowCoverBuffer on a fresh allocation.
            /// Never depends on time. Starts with every piece - the buffer holds
            /// undefined texels until the first bake.
            ///
            /// And per LIGHT TYPE: a piece's arc channels (.r / .g) depend on the
            /// arcs and the geometry alone, its segment channels (.b / .a) on the
            /// segments alone. So the two kinds keep separate masks, and the bake
            /// integrates and writes only the kind a piece needs - arcs animating
            /// over still segments no longer re-integrate every segment's bell
            /// in each dirty piece (1.35 -> 0.36 ms of bake with 4 + 4 on an AMD
            /// Radeon Pro 5300M). The geometry dirties both.
            uint32_t dirtyArcPieces = 0xFFu;     ///< Pieces whose arc channels need a bake; GLOW_COVER_ALL_PIECES in the .cpp.
            uint32_t dirtySegmentPieces = 0xFFu; ///< Pieces whose segment channels need a bake, likewise.

            /// Seconds of frame time @c mGlowCover.buffer has gone unread - the ring
            /// lit uniformly, or the layer off. @ref Update releases the table at
            /// GLOW_COVER_RELEASE_SECONDS (docs/neon-perf-plan.md item 10).
            float unreadSeconds = 0.0f;
        } GlowCoverTable;

        /// Pass 1a's target and its state: the gather buffer, the format the
        /// driver gave it, and what it last drew.
        typedef struct GatherTarget
        {
            /// The gather pass's target, at GetGatherScale -
            /// coarser than @c mScaledBuffer for any rect much bigger than a
            /// thumbnail - over the gather quad's box (@c mGatherBounds) clipped
            /// to the viewport: the gathered colour and arc coverage, plus a
            /// second attachment for the segments' only when there are segments,
            /// RGBA16F where the driver renders to it and RGBA8 where not. Read by
            /// pass 1 and the edge ring; released when the layer is disabled.
            Framebuffer buffer{"NeonRenderer.Gather"};

            /// Index into the gather buffer's format list (GATHER_FORMATS) of the
            /// best format the driver has not refused: the tier
            /// ResizeInBestFormat resumes from, kept here because the buffer is
            /// released and cannot record it. Only ever advances.
            size_t format = 0;
            GatherCache cache; ///< What @c buffer holds - see @ref GatherCache.
        } GatherTarget;

        /// The glow's hue-invariant field (pass 1f, read by 1c): its buffer,
        /// the schedule it is baked and read on, and how the composite finds
        /// its texels.
        typedef struct GlowField
        {
            /// The hue-invariant field (pass 1f): Fa, R16F, on the reduced buffer's
            /// own region and grid, half that buffer's size (0.84 MB at 0.5 for a
            /// 960 x 540 rect at 1080p). Released with the conditions that want it
            /// - see OnConfigChanged - and never allocated for a config that cannot
            /// use it (segments, or a hue that does not rotate with the intensity
            /// still).
            Framebuffer buffer{"NeonRenderer.Field"};

            /// When @c mGlowField.buffer is baked and read - see @ref LazyBake.
            LazyBake schedule;

            /// How pass 1c finds a fragment's texel of the field it was baked as -
            /// floor((vPos - origin) * texelScale), both in vPos's scaled
            /// rect-local px: the reduced buffer's origin and texels per scaled px.
            /// Recorded by the bake, from the region it drew (@ref BufferRegion),
            /// and uploaded as @c uFieldOrigin / @c uFieldTexelScale.
            glm::vec2 origin{0.0f};     ///< The region's lower-left corner, scaled rect-local px.
            glm::vec2 texelScale{1.0f}; ///< Texels per scaled px: 1 but on an axis capped to the viewport, whose texel count is truncated.
        } GlowField;

        /// The edge ring's field (pass 1r, read by 2r): its buffer, how the
        /// ring's pixels are packed into it, and the schedule it is baked and
        /// read on.
        typedef struct RingField
        {
            /// The edge ring's field (pass 1r): Fa, R16F, at full resolution over
            /// the ring alone, packed as @c mRingField.layout. Only for a config
            /// @ref Render finds eligible (no segments, and a
            /// colour-stop alpha time cannot move). Released with the conditions
            /// that want it - see OnConfigChanged.
            Framebuffer buffer{"NeonRenderer.RingField"};
            RingFieldLayout layout{}; ///< The packing it was baked with.

            /// When @c mRingField.buffer is baked and read - see @ref LazyBake.
            LazyBake schedule;
        } RingField;

        /// Whether the offscreen phase can be skipped on this frame, and the
        /// viewport that decision holds for.
        typedef struct OffscreenState
        {
            /// Whether the offscreen phase's buffers - @c mGather.buffer and
            /// @c mScaledBuffer, at every scale - still hold exactly what this
            /// frame's passes 1a and 1b would draw into them, so @ref Render can
            /// skip both and never leave the caller's framebuffer.
            ///
            /// The gather has a skip of its own since I55 (@ref GatherCache), which
            /// a still frame meets too; pass 1b has no other. So what this flag
            /// alone saves is pass 1b's shading - the glow's field serves only hue
            /// and intensity frames, and is not consulted on a frame that reuses
            /// the buffers - and the trip off the caller's framebuffer and back,
            /// with the target capture it takes.
            ///
            /// Both passes are pure functions of the config, the viewport, the two
            /// tables they read (emission, glow coverage) and - only through the
            /// hue rotation - the time; both buffers persist between frames and
            /// nothing else writes them. So: cleared by @ref OnConfigChanged on ANY
            /// change to the neon's config or the geometry (pass 1b reads too much
            /// of the former for a narrower gate to be worth its risk), but not on
            /// another layer's, which nothing here reads and an animation of which
            /// would otherwise clear it every frame; set by @ref Render on every
            /// frame whose offscreen phase drew or reused both, unless the reduced
            /// buffer holds the field's composite rather than the shading (I56: the
            /// frame the intensity stops on redraws pass 1b exactly); and honoured
            /// only on a frame whose emission table is current
            /// (@ref isEmissionTableStale, which carries the time and the ring's
            /// cross-fade), whose glow coverage table needs no bake, and whose
            /// viewport is @c viewport. Byte-identical. With the gather's own
            /// skip in place, a still frame without this flag is 4.2x slower
            /// at scale 0.5 (3.1-5.5x across neon-scale-check's twelve scenes),
            /// 10.3x at 1.0 and 2.1x at 0.125; frames whose config changes are
            /// unaffected, since every change clears it (AMD Radeon Pro 5300M,
            /// 1920 x 1080, 2026-10-08). First built as docs/neon-perf-plan.md
            /// item 4.
            bool reusable = false;

            /// The viewport, px, the offscreen buffers were drawn for. The regions
            /// they cover are placed in it (GetBufferRegion), so @c reusable holds
            /// only on a frame drawn at this same viewport.
            glm::ivec2 viewport{0};
        } OffscreenState;

        // --- Programs: built once, on first use ------------------------------
        /// Build @p program from @p fragSrc over neon.vert, with @p define
        /// spliced in (none for @c nullptr), once - every program goes through
        /// here, the emission pre-pass's from @ref Initialize and the rest on
        /// first use. Returns true if it is ready. A failed build is
        /// recorded in @c mFailedPrograms under @p programBit, logged once with
        /// @p consequence, and never retried, so a broken driver costs one
        /// compile and one log line, not one per frame. @p blocks (@c BLOCK_*)
        /// names the uniform blocks the source declares, each bound to its
        /// binding point.
        bool ensureProgram(ShaderProgram &program, const char *fragSrc, const char *define, const char *name,
                           unsigned int programBit, unsigned int blocks, const char *consequence);

        /// Make sure every program the glow draws with is built, building any
        /// that are not: neon-gather.frag, neon.frag twice (pass 1b and the
        /// ring - one program object per target) and the blit. Lazy rather
        /// than at @ref Initialize, so a host that never enables the glow
        /// never compiles them. False if any of them failed, in which case the
        /// glow is skipped and the frame degrades to the fill.
        bool ensureGlowPrograms();

        /// Make sure the glow coverage bake (neon-glow-cover.frag) is built,
        /// building it if not. Called only on a frame that will bake the
        /// table, so a host whose ring is lit uniformly never compiles it. A
        /// failed build is recorded in @c mFailedPrograms and never retried,
        /// and the frame draws the fill alone, as for a path program.
        bool ensureGlowCoverProgram();

        /// Make sure the field's two programs - neon.frag with NEON_FIELD_BAKE,
        /// and neon-field.frag - are built, building them if not. Called on
        /// every frame whose config the field can serve, so they are built on
        /// the first such frame - which shades directly, and is compiling its
        /// path's programs too - rather than stalling the bake frame after it.
        /// A failure is recorded in @c mFailedPrograms; the field is then never
        /// used and the shading draws as before.
        bool ensureFieldPrograms();

        /// Make sure the ring field's composite (neon-field.frag with
        /// NEON_FIELD_RING) and the bake (@ref ensureFieldPrograms) are built.
        /// A failure is recorded in @c mFailedPrograms; the ring is then shaded
        /// directly.
        bool ensureRingFieldPrograms();

        /// Make sure the opaque fill (black-rect.frag) is built, building it if
        /// not. Called by @ref renderFillPass only when it draws through the
        /// shader, so a host with no fill, or one a clear stands in for, never
        /// compiles it. A failed build is recorded in @c mFailedPrograms and
        /// never retried, and the fill is skipped.
        bool ensureFillProgram();

        // --- Buffers: allocated in the best format the driver gives ----------
        /// Make sure @c mGlowCover.buffer is allocated, allocating it if not, at
        /// the coverage table's fixed dimensions in the best format the driver
        /// will give. Called only on a frame that will bake the table, beside
        /// @ref ensureGlowCoverProgram - so a host whose ring is lit uniformly
        /// never holds the table's 1 MB - and the buffer is released again
        /// when the layer is disabled (@ref OnConfigChanged).
        ///
        /// The format walk is ResizeInBestFormat's, resumed from
        /// @c mGlowCover.format, since a released buffer has no format of its
        /// own to resume from. A fresh allocation holds undefined texels, so it
        /// sets both of @c mGlowCover's dirty masks whole.
        ///
        /// Two channels (RG16F, 0.5 MB) when the config has no segments
        /// (@p segments false), whose coverage would fill .b / .a with zeros;
        /// four (RGBA16F, 1 MB) when it has - reallocating a two-channel table
        /// the first frame segments appear, and keeping four after they go,
        /// so a transient segment does not reallocate it every time.
        /// @return false only if NO candidate could be allocated, in which case
        ///         the frame draws the fill alone, as for a failed program.
        bool ensureGlowCoverBuffer(bool segments);

        /// Size @c mScaledBuffer to @p texels, bind it and clear it - pass 1b's
        /// target, whether the shading draws it (@ref renderShadePass) or the
        /// field's composite does (@ref renderFieldCompositePass). The caller
        /// lifts the host's scissor first and keeps it lifted through its draw
        /// (see renderShadePass).
        /// @return false if the buffer could not be allocated, in which case
        ///         nothing was bound and the caller must draw nothing.
        bool bindScaledTarget(const glm::ivec2 &texels);

        // --- CPU-side inputs: rebuilt on a config change ---------------------
        /// Place @c NeonConfig::numSamples points (clamped) evenly by arc
        /// length round the perimeter, from @c t = 0, in scaled rect-local px,
        /// and upload them to @c mLoopSamplesBlock as @c vec4(x, y, 0, 0).
        /// Entries past the count stay 0 and are never read - the gather's
        /// loop stops at the same count - so lowering the count spreads the
        /// samples rather than cutting the walk short. An upload that repeats
        /// the last bytes moves no upload count (@ref GatherInputs). Called by
        /// @ref Initialize, and by @ref OnConfigChanged when the geometry, the
        /// resolution scale or the sample count moves.
        void rebuildLoopSamples(const Config &config);

        /// Re-bake the three colour LUTs: the gradient ring (@c mGradientLUT,
        /// from the colour stops, which cross-fades to a new ring, or snaps
        /// when its width changes), the segment atlas (@c mSegmentLUT) and the
        /// arc atlas (@c mArcLUT). Each wrapper self-guards, re-baking only
        /// when what it reads moved, so this is called unconditionally on
        /// every config change; the fields an animation rewrites every frame
        /// (a segment's position, an arc's span) ride the light blocks and
        /// dirty no LUT. See the note at the definition.
        /// @pre @c mEffectiveSegments holds the merged segment list for
        ///      @p config.
        void bakeLUTs(const Config &config);

        /// Make the segment + arc UBOs current and bind them. Called before the
        /// emission pre-pass because BOTH passes read them: the pre-pass to
        /// bake the per-sample emission, the main pass for the continuous
        /// filament gate.
        ///
        /// Repacks only when @c mLightBlocks.dirty says the config moved; the
        /// bind is unconditional. See the definition for why the two are
        /// treated differently.
        /// @pre @c mEffectiveSegments is current for @p config - i.e.
        ///      @ref OnConfigChanged has run for any change since the last
        ///      frame, which the effect guarantees by calling Update before
        ///      Render. This method deliberately does not refill it.
        void packLightBlocks(const Config &config);

        /// The pack half of @ref packLightBlocks, split out so the gate reads
        /// as one branch rather than wrapping forty lines of std140 packing:
        /// @c mEffectiveSegments into @c mLightBlocks.segment, one
        /// @c vec4(position, invSigma, boost, hasStops) per segment, and
        /// @c NeonConfig::arcs into @c mLightBlocks.arc, @c .w the PackArcFlags
        /// bitmask. An upload that repeats the last bytes is skipped and
        /// moves no upload count (@ref EmissionInputs).
        void packLightBlockData(const Config &config);

        // --- Geometry: the meshes the passes draw ----------------------------
        /// Upload the static NDC quad the fullscreen passes draw. Called once
        /// from @ref Initialize: the quad is in clip space, so unlike
        /// @ref setupGeometry's it is independent of the geometry, the
        /// viewport and the resolution scale, and never needs rebuilding.
        void setupFullscreenQuad();

        /// Build @c mGlowMesh, pass 1b's quad - the rect plus the glow's reach,
        /// in scaled rect-local px, with a hole where the glow cannot reach -
        /// and the margins, corner skips and inner reach the shading fades and
        /// skips against (@c mGlowLimits.quadMargin, @c mRingLimits.quadMargin, @c mGlowLimits.cornerSkip,
        /// @c mRingLimits.cornerSkip, @c mGlowInnerReach), plus @c mGlowBounds.
        void setupGeometry(const Config &config);

        /// Build @c mFillMesh: the geometry that BOUNDS the opaque
        /// fill, as a rectangular annulus in full-resolution rect-local pixels.
        ///
        /// The fill used to rasterise the whole viewport and let the fragment
        /// shader @c discard everything outside the band. That is the pattern
        /// @ref setupGeometry exists to avoid ("geometry bounds the far region
        /// instead of a per-fragment discard"), and it cost the same whether
        /// the band was 20 px or the entire screen - measurably, a fixed
        /// full-viewport charge on every frame with @c opaqueMode set.
        ///
        /// Builds nothing (@c mFillMesh count 0) whenever the fill's coverage
        /// is 1 at every pixel - @c ALL by definition, and @c BOTH with both
        /// of the fill's cutoffs (@c NeonConfig::opaqueInsideCutoff /
        /// @c opaqueOutsideCutoff) disabled by arithmetic (their default state,
        /// so the common way in). Those modes need no bounding geometry either way:
        /// @ref renderFillPass clears for them, and on the rare state where
        /// a clear would not clip like a draw it falls back to the static
        /// fullscreen quad, never to a ring. Both passes ask one shared
        /// predicate so they cannot disagree; see @c FillsWholeViewport.
        void setupFillGeometry(const Config &config);

        /// Build the two FULL-RES partition meshes, from one set
        /// of box coordinates: @c mRingMesh, a rectangular annulus over
        /// the edge ring (GetRingWidth either side of the edge, clipped to the
        /// band the glow can still be lit in), which pass 2c re-shades at full
        /// resolution; and @c mBlitMesh, which pass 2b composites the
        /// reduced buffer over - the ring's complement, bounded to where the
        /// glow can be non-zero (its lit band and its fade margin), and so
        /// possibly empty.
        ///
        /// The two share their boundary vertices bit for bit, so the rasteriser
        /// gives every pixel to at most one of them - both composite
        /// premultiplied-over, and a pixel drawn by both would composite twice.
        /// It also builds the gather pass's quad (@c mGatherMesh), and records
        /// the ring's and the gather quad's boxes (@c mRingBounds,
        /// @c mGatherBounds) and what the blit reads (@c mScaledOuter). Reads
        /// @c mGlowLimits.quadMargin, so it runs after @ref setupGeometry.
        void setupRingGeometry(const Config &config);

        /// The packing for the ring @p ring centred at @p centerFull in
        /// a viewport of @p viewportWidth x @p viewportHeight. The hole is
        /// CONSERVATIVE - only pixels whose centre lies more than a pixel
        /// inside it - so every pixel the ring can draw falls in a strip.
        static RingFieldLayout computeRingFieldLayout(const Annulus &ring, const glm::vec2 &centerFull,
                                                      int viewportWidth, int viewportHeight);

        // --- Skip keys: whether a pass has to run ----------------------------
        /// The pass-0 inputs other than time, as @ref renderEmissionPass would
        /// upload and bind them now: compared against the last bake's by
        /// @ref isEmissionTableStale, and recorded by the bake.
        /// @pre @ref packLightBlocks has run this frame: the blocks' upload
        ///      counts are part of the key.
        EmissionInputs getEmissionInputs(const Config &config) const;

        /// Whether @c mEmission.buffer's contents still describe this frame,
        /// i.e. whether pass 0 has to run at all.
        ///
        /// The table is a pure function of what @ref renderEmissionPass hands
        /// neon-emission.frag - the same invariant the pre-pass itself rests on
        /// - and the buffer is allocated once for the renderer's lifetime and
        /// written by nothing else, so a frame whose inputs all match the last
        /// bake's can read what is already in it. The pre-pass hoists the
        /// gather's fragment-invariant half out of every FRAGMENT; this is what
        /// hoists it out of every FRAME as well.
        ///
        /// Stale when any of three things holds:
        ///   - the table was never baked (@c mEmission.bake);
        ///   - an input other than time moved (@ref EmissionInputs): the two
        ///     value uniforms compared by value, and the three LUTs and two
        ///     light blocks by their upload counts. Version numbers on the GL
        ///     objects the pass binds, NOT a list of the config fields that
        ///     feed them - so a config change that moves none of them (an
        ///     intensity, bloom, glow, geometry or other-layer animation)
        ///     re-bakes nothing, and nothing that does move one can be missed
        ///     by a gate falling out of step with the shader. A cross-fade
        ///     frame moves the ring's count like a re-bake does.
        ///   - @c uTime moved at a non-zero hue rate. It reaches the shader
        ///     exactly once, as `si - uTime * uHueRotationRate`, so at a rate
        ///     of 0 it drops out of the table altogether.
        ///
        /// It used to be "any config change" (@c mEmissionDirty), deliberately
        /// wide because a field list would drift from the shader; the version
        /// numbers have no field list. See docs/emission-prepass.md section 3.
        /// @pre @ref packLightBlocks has run this frame: the blocks' counts
        ///      are part of the key, and only the pack moves them.
        bool isEmissionTableStale(float time, const Config &config) const;

        /// Pass 1a's inputs besides its region and quad, as
        /// @ref renderGatherPass would hand them over now, for a viewport of
        /// @p viewportWidth x @p viewportHeight.
        /// @pre Pass 0 has run this frame if it is going to: the table's
        ///      version is part of the key.
        GatherInputs getGatherInputs(const Config &config, int viewportWidth, int viewportHeight) const;

        // --- Uniform uploads the passes share --------------------------------
        /// The transform and the rect's shape - uMVP, uRectSize,
        /// uCornerRadius - for @p shader at @p scale: everything the gather
        /// pass (neon-gather.frag) reads besides its own inputs, and the first
        /// thing @ref uploadNeonUniforms uploads for every other program.
        /// @pre @p shader is in use.
        void uploadShapeUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale, const Config &config);

        /// Every SHADING uniform the neon programs read, for @p shader at
        /// @p scale, fading against @p limits.quadMargin and skipping corner
        /// arcs past @p limits.cornerSkip (px in the same space, both). One
        /// copy for every program built from neon.frag, so the variants cannot
        /// drift apart in what they are told. Not for neon-gather.frag, which
        /// shades nothing - it takes @ref uploadShapeUniforms alone. The
        /// gather's own inputs - the sample block, the count and the emission
        /// table - are not here: only the gathering programs have them, and
        /// @ref bindGatherInputs binds them. @p blitOwnsCut is true for a pass
        /// the blit composites (pass 1b and its field bake), which leaves the
        /// one-sided cut and the cutoffs to it, false for the edge ring and its
        /// field bake, which apply both after the grade (uBlitOwnsCut).
        /// @pre @p shader is in use.
        void uploadNeonUniforms(ShaderProgram &shader, const glm::mat4 &mvp, float scale, float time,
                                const ShadeLimits &limits, bool blitOwnsCut, const Config &config);

        /// Bind the gather loop's own inputs - the sample block, the count and
        /// the emission table - for @p shader, which is in use. Only the
        /// gather pass runs the loop, so only it has them: @c mLoopSamplesBlock
        /// at its binding, @c uNumSamples (clamped), and @c mEmission.buffer on
        /// texture unit 3 as @c uEmission.
        void bindGatherInputs(ShaderProgram &shader, const Config &config);

        /// Bind @c mGather.buffer's attachments for @p shader (a neon.frag
        /// program, in use), with @p gatherUV, the map from the pass's vPos
        /// onto the buffer's uv: attachment 0 on texture unit 3 as
        /// @c uGather, attachment 1 - the segments' - on unit 4 as
        /// @c uGatherSeg. With no segments unit 4 gets attachment 0 again,
        /// never read but complete: Apple's driver warns about a sampler bound
        /// to texture 0 whether or not it is read.
        void bindGatherBuffer(ShaderProgram &shader, const UVMap &gatherUV);

        // --- The passes, in the order Render runs them -----------------------
        // The order is the frame's (docs/neon-shader-outputs.html), and the
        // .cpp defines them in the same order - keep the three in step. The
        // data dependency is the real contract: pass 0 bakes the
        // table the gather reads, pass 1a's gather buffer is what pass 1b and
        // pass 2c shade from, pass 2a's fill must land before pass 2b
        // composites the glow over it. 2b and 2c cover disjoint areas, so
        // their order between themselves is free. Every offscreen pass (0, 0b,
        // 1a, 1b, and the fields' bakes) runs first, then everything on the
        // caller's framebuffer, so that target is drawn in one unbroken run.

        // STATE OWNERSHIP. `Render` owns blend state - enable and func - and
        // sets it immediately before each pass that depends on it, so no pass
        // touches GL_BLEND and the whole blend timeline reads in one place.
        // A pass owns its shader, and a pass that RETARGETS the framebuffer
        // restores it (an excursion, unlike a mode). Preconditions each pass
        // relies on are stated in its @pre below.
        //
        // The passes take the pieces of the frame transform they actually use.
        // @ref Render derives them once, in SCALED space, so the gather quad
        // and the sample positions agree; the opaque fill is the exception and
        // takes the viewport height, since it always draws at full resolution
        // on the caller's framebuffer.

        /// Pass 0: bake the fragment-invariant half of the gather into
        /// @c mEmission.buffer, at the clamped sample count so texel i here is
        /// sample i in the gather. Retargets the framebuffer and viewport, so
        /// it restores both before returning - see docs/emission-prepass.md.
        ///
        /// Records what it baked (@c mEmission.bake) on the way out, so
        /// @ref isEmissionTableStale reads a snapshot written by the only
        /// thing that ever writes the buffer.
        /// @pre Blending disabled - a table write is not a composite.
        /// @pre @c mEmission.buffer is allocated, which @ref Initialize
        ///      guarantees for the renderer's lifetime - hence no failure to
        ///      report and nothing to allocate here.
        void renderEmissionPass(int viewportWidth, int viewportHeight, float time, const Config &config);

        /// Pass 0b: bake the glow coverage table into @c mGlowCover.buffer -
        /// for each piece of the emitter, the arcs' and the segments' coverage
        /// over that piece's own extent, weighted by the halo's and the bloom's
        /// kernel about a fragment's foot on it (neon-glow-cover.frag, V20 and
        /// V21 in docs/review-findings.md).
        /// Retargets the framebuffer and viewport, so it restores both before
        /// returning, and clears both of @c mGlowCover's dirty masks on the
        /// way out. Bakes only the pieces they name, by drawing its quad onto
        /// their rectangles of the table - all of it in one draw when all are
        /// dirty - and, for a piece only one light type dirtied, integrates
        /// that type alone and writes its channels under @c glColorMask,
        /// restoring the host's mask after.
        /// @pre Blending disabled, and the light blocks packed this frame.
        void renderGlowCoverPass(const Config &config);

        /// Pass 1a: the gather, into @c mGather.buffer (@p texels, at
        /// GetGatherScale), over @c mGatherMesh, drawn through @p mvp - the
        /// gather region's projection from pass 1's scaled rect-local space.
        /// Takes no time:
        /// the loop reads time only through the emission table. Leaves the
        /// buffer bound; @ref Render restores the target.
        /// @pre Blending disabled - the buffer is data.
        /// @return false if the buffer could not be allocated, in which case
        ///         every later glow pass must be skipped.
        bool renderGatherPass(const glm::mat4 &mvp, const glm::ivec2 &texels, float scale, const Config &config);

        // P1f and P1c - the shading, factored into a hue-invariant field.
        // On a config with no segments whose colour-stop alpha cannot move
        // with time, pass 1b's output is tonemap(col * Fa), with only the
        // gathered hue col changing from frame to frame (neon-field.frag) - the
        // blit applies the cut and the cutoffs after it. So the shading splits
        // in two: 1f bakes Fa once the config has held for a frame, offscreen,
        // after the gather; 1c composites it with the gathered hue in pass 1b's
        // place, into the reduced buffer, under a rotating hue (1.1-2.0x on
        // those frames on an AMD Radeon Pro 5300M, I50) and an intensity
        // animation (I56).

        /// Pass 1f: bake the field into @c mGlowField.buffer, @p texels, drawn
        /// through @p mvp - the projection of the reduced buffer's region at
        /// @p scale, which pass 1c reads it over - reading the gather through
        /// @p gatherUV, as pass 1b reads it. R16F, Fa alone. Leaves the buffer bound;
        /// @ref Render restores the target.
        /// @pre Blending disabled; pass 1a has run or its buffer is current.
        /// @return false if the buffer could not be allocated - the field is
        ///         then never used.
        bool renderFieldPass(const glm::mat4 &mvp, const glm::ivec2 &texels, const UVMap &gatherUV, float scale,
                             float time, const Config &config);

        /// Pass 1b: the neon on the tight glow quad, shaded from the gather
        /// pass's result, so it must run after it - into @c mScaledBuffer,
        /// @p texels drawn through @p mvp (its region's projection) with
        /// @c mShadeShader, reading @c mGather.buffer through @p gatherUV. Clears that
        /// buffer and leaves it bound; @ref Render restores the target before
        /// pass 2a.
        /// @pre Blending DISABLED: the buffer was just cleared and the quad
        ///      covers each texel once, so over would only have added zero, for
        ///      a destination read per texel.
        /// @return false if the reduced target could not be allocated, in
        ///         which case nothing was drawn and passes 2b and 2c must be
        ///         skipped too - they would otherwise composite a stale or
        ///         undefined buffer.
        bool renderShadePass(const glm::mat4 &mvp, const glm::ivec2 &texels, const UVMap &gatherUV, float time,
                             const Config &config);

        /// Pass 1c: in pass 1b's place, the glow quad drawn with
        /// neon-field.frag - the field times the gathered hue, tone-mapped -
        /// into pass 1b's target, @c mScaledBuffer at @p texels, sized, bound
        /// and cleared here. The same arguments as @ref renderShadePass; the
        /// field is multiplied by @p gain
        /// (@c LazyBake::Decision::gain).
        /// @pre Blending disabled.
        /// @return false if the reduced buffer could not be allocated.
        bool renderFieldCompositePass(const glm::mat4 &mvp, const glm::ivec2 &texels, const UVMap &gatherUV,
                                      float gain);

        // P1r and P2r - the edge ring, factored into a field of its own.
        // The ring is re-shaded on EVERY frame, still frames included: it
        // draws onto the caller's framebuffer, so there is nothing to reuse.
        // With no segments its output is
        // mask * tonemap(col * Fa) - the mask being the one-sided cut and the
        // cutoffs, which the ring applies after its tone map at full
        // resolution, and a function of the pixel's position alone - so it
        // splits like pass 1b's: pass 1r bakes Fa over the ring alone, at full
        // resolution and packed into its four strips (@ref RingFieldLayout),
        // once the config has held for a frame; pass 2r composites it with the
        // gathered hue in pass 2c's place, and multiplies the mask back in.
        // 1.4-1.9x on still frames and 1.2-1.6x on hue frames on an AMD Radeon
        // Pro 5300M, within 1/255 (I52); the mask since I53.

        /// Pass 1r: bake the ring's field into @c mRingField.buffer, laid out
        /// as @c mRingField.layout - the ring's shading uploaded exactly as
        /// @ref renderRingPass uploads it, with the hue at 1, drawn once per
        /// strip through that strip's atlas rows as the viewport and a
        /// projection onto them (transposed for the two side strips). Reads
        /// the gather through the full-res map @p gatherUV. Leaves the buffer
        /// bound; @ref Render restores the target.
        /// @pre Blending disabled; pass 1a has run or its buffer is current.
        /// @return false if the buffer could not be allocated.
        bool renderRingFieldPass(const UVMap &gatherUV, float time, const Config &config);

        /// Pass 2a: opaque-mode background fill (its band ring, or a clear), at
        /// FULL resolution on the caller's framebuffer regardless of the
        /// resolution scale - it is a flat shape from an analytic SDF, so
        /// scaling it would only cost it its clean edges. The fragment shader
        /// reads @c gl_FragCoord, so the shape is still derived in window
        /// space - the transform only places the bounding geometry, and the
        /// viewport is what both are expressed in. Caller guards on
        /// @c opaqueMode != NONE.
        ///
        /// Draws @c mFillMesh (the band ring from
        /// @ref setupFillGeometry) for every mode whose coverage is shaped.
        /// A fill that covers every pixel at coverage 1 runs no shader: it is
        /// a scissored @c glClear, bounded by the intersection of the queried
        /// viewport with the host's own scissor. That substitution is dropped -
        /// for the fullscreen quad, shader and all - when @c GL_STENCIL_TEST
        /// or @c GL_DEPTH_TEST is enabled, since a clear ignores both and would
        /// paint through a mask the host set up to clip this pass.
        void renderFillPass(int viewportWidth, int viewportHeight, const Config &config);

        /// Pass 2b: bilinear composite of the reduced buffer onto the caller's
        /// framebuffer, AND the one-sided glow cut and the inside/outside
        /// cutoffs, at full resolution, for what pass 1b drew (the ring applies
        /// its own). Covers what can still be lit outside
        /// the edge ring (@c mBlitMesh), which pass 2c draws - and
        /// draws nothing when that is empty.
        ///
        /// The cut lives here rather than in the gather because the gather's
        /// output is upsampled: an edge drawn at resolutionScale is smeared
        /// 1/scale destination pixels each way, across the line as well as
        /// along the lit side. Draws through @p mvp, the full-res transform
        /// @ref Render builds once for this pass and pass 2c together - the
        /// partition depends on both using one matrix, and reads the reduced
        /// buffer through @p scaledUV. @p centerFull is the rect centre in
        /// gl_FragCoord's y-up space, mirrored as @ref renderFillPass mirrors it.
        /// @pre Premultiplied-over blending, and the caller's framebuffer and
        ///      full-resolution viewport are restored.
        void renderBlitPass(const glm::mat4 &mvp, const glm::vec2 &centerFull, const UVMap &scaledUV,
                            const Config &config);

        /// Pass 2c: the edge ring. Re-shades the ring at FULL resolution with
        /// @c mRingShader, which takes the gather's result from
        /// @c mGather.buffer instead of running the gather - so the filament,
        /// the cut and the cutoffs near the line come out at full resolution,
        /// while the expensive loop ran once, coarsely. Reads it through
        /// @p gatherUV, the gather region's map from full-res rect-local px.
        /// Draws @c mRingMesh, which
        /// shares its edges with what pass 2b drew, through the same @p mvp.
        /// @pre Premultiplied-over blending; the caller's framebuffer and
        ///      full-resolution viewport are restored; pass 1 succeeded.
        void renderRingPass(const glm::mat4 &mvp, const UVMap &gatherUV, float time, const Config &config);

        /// Pass 2r: in pass 2c's place, @c mRingMesh drawn through
        /// @p mvp - the matrix the blit drew with, so the partition holds -
        /// with the ring field's composite: the field times @p gain times the
        /// gathered hue, tone-mapped, times the one-sided cut and the cutoffs,
        /// which it takes from @p config as the ring's own shading does.
        /// @pre Premultiplied-over blending; the caller's framebuffer and
        ///      full-resolution viewport are restored.
        void renderRingFieldCompositePass(const glm::mat4 &mvp, const UVMap &gatherUV, float gain,
                                          const Config &config);

    private:
        /// Bits of @c mFailedPrograms, one per program, each passed to
        /// @ref ensureProgram with its program. The values only have to be
        /// distinct; nothing stores or orders them.
        static constexpr unsigned int PROGRAM_GATHER = 1u << 0;               ///< @c mGatherShader, P1a.
        static constexpr unsigned int PROGRAM_SHADE = 1u << 1;                ///< @c mShadeShader, P1b.
        static constexpr unsigned int PROGRAM_BLIT = 1u << 2;                 ///< @c mBlitShader, P2b.
        static constexpr unsigned int PROGRAM_RING = 1u << 3;                 ///< @c mRingShader, P2c.
        static constexpr unsigned int PROGRAM_GLOW_COVER = 1u << 4;           ///< @c mGlowCoverShader, P0b.
        static constexpr unsigned int PROGRAM_FIELD_BAKE = 1u << 5;           ///< @c mFieldBakeShader, P1f and P1r.
        static constexpr unsigned int PROGRAM_FIELD_COMPOSITE = 1u << 6;      ///< @c mFieldCompositeShader, P1c.
        static constexpr unsigned int PROGRAM_RING_FIELD_COMPOSITE = 1u << 7; ///< @c mRingFieldCompositeShader, P2r.
        static constexpr unsigned int PROGRAM_FILL = 1u << 8;                 ///< @c mFillShader, P2a.
        static constexpr unsigned int PROGRAM_EMISSION = 1u << 9;             ///< @c mEmissionShader, P0 - built in Initialize, whose failure fails it.

        /// Uniform blocks a program declares, for @ref ensureProgram.
        static constexpr unsigned int BLOCK_SEGMENT = 1u << 0; ///< SegmentBlock
        static constexpr unsigned int BLOCK_ARC = 1u << 1;     ///< ArcBlock
        static constexpr unsigned int BLOCK_SAMPLES = 1u << 2; ///< LoopSamplesBlock

        // --- Lifecycle, and what a config change leaves for the next frame ----
        /// The configuration the last @ref OnConfigChanged was handed. That
        /// call compares the next one against it, field by field, to tell
        /// what moved and so what to rebuild; @ref Initialize builds the
        /// geometry and the LUTs from it. The passes take the frame's config
        /// as an argument instead - the effect hands every call the same
        /// composited config.
        Config mCurrentConfig;

        /// Set at the end of @ref Initialize. Gates the rebuilds in
        /// @ref OnConfigChanged, which can run before it; the neon.frag programs
        /// cannot be that gate any more, since they are built on first draw.
        bool mInitialized = false;

        /// @c PROGRAM_* bits of the programs that failed to build - see
        /// @ref ensureProgram.
        unsigned int mFailedPrograms = 0;

        /// A config change since the last frame moved NeonConfig::intensity.
        /// Set by @ref OnConfigChanged, consumed by the next @ref Render: the
        /// fields serve a scaled intensity only while it moves (LazyBake).
        bool mIntensityMoving = false;

        // --- Programs: every one built through ensureProgram -----------------
        ShaderProgram mEmissionShader;           ///< P0, neon-emission.frag. Built in Initialize.
        ShaderProgram mGlowCoverShader;          ///< P0b, neon-glow-cover.frag. Built on the first frame that bakes the table.
        ShaderProgram mGatherShader;             ///< P1a, neon-gather.frag. Built on the first frame.
        ShaderProgram mShadeShader;              ///< P1b, neon.frag, into the reduced buffer. Built on the first frame.
        ShaderProgram mFieldBakeShader;          ///< P1f and P1r, neon.frag + NEON_FIELD_BAKE. Built on the first field-eligible frame.
        ShaderProgram mFieldCompositeShader;     ///< P1c, neon-field.frag. Built with it.
        ShaderProgram mFillShader;               ///< P2a, black-rect.frag. Built the first time a fill draws through it.
        ShaderProgram mBlitShader;               ///< P2b, neon-blit.frag. Built on the first frame.
        ShaderProgram mRingShader;               ///< P2c, neon.frag again, its own object - see ensureGlowPrograms. Built on the first frame.
        ShaderProgram mRingFieldCompositeShader; ///< P2r, neon-field.frag + NEON_FIELD_RING. Its own object - it draws the caller's framebuffer, blended, in the frame pass 1c draws the reduced buffer unblended. Built on the first ring-field-eligible frame.

        // --- CPU-side inputs: uniform blocks and colour LUTs -----------------
        LightBlocks mLightBlocks; ///< The segment and arc uniform blocks; see @ref LightBlocks.

        /// Backs neon.frag's std140 `LoopSamplesBlock` - vec4[NUM_LOOP_SAMPLES]
        /// where .xy holds the perimeter point in scaled rect-local pixels.
        /// Always allocated at full size; only the first @c uNumSamples entries
        /// are filled, and the shader stops there.
        UniformBuffer mLoopSamplesBlock{"NeonRenderer.LoopSamplesBlock"};

        /// Reusable scratch for the merged transient+preserved segment list
        /// (Config::FillEffectiveSegments). Held as a member so the per-frame
        /// UBO pack / dirty check do no heap allocation after warmup.
        std::vector<SegmentBoost> mEffectiveSegments;

        /// Baked colour ring (@c NeonConfig::gradientLutSize x 1 RGBA8, sampled
        /// at v = 0.5). The wrapper owns the bake, the cross-fade and the guard
        /// behind them - see @ref GradientRingLUT.
        GradientRingLUT mGradientLUT;

        /// Per-segment gradient atlas (SEGMENT_LUT_WIDTH x MAX_SEGMENT_BOOSTS),
        /// one row per segment. The wrapper owns the bake, the dirty check and
        /// the snapshot behind it - see @ref SpanAtlasLUT.
        SpanAtlasLUT<SegmentBoost> mSegmentLUT;

        /// Per-arc gradient atlas (ARC_LUT_WIDTH x MAX_ARCS), one row per arc.
        /// Same shape and purpose as mSegmentLUT.
        SpanAtlasLUT<Arc> mArcLUT;

        // --- Geometry: built by the setup*Geometry methods -------------------
        /// Pass 1b's quad (rect + glow reach), scaled rect-local px: 6 vertices
        /// for the plain quad, 24 when @ref setupGeometry cuts a hole - by a
        /// cutoff, the one-sided cut, or the glow's own reach on a rect larger
        /// than twice it.
        Mesh mGlowMesh{"NeonRenderer.Glow", 6};

        /// The opaque fill's band ring (rect +- the fill's cutoffs), FULL-RES
        /// rect-local px: 24 vertices, or none when the fill covers every pixel
        /// (@ref setupFillGeometry) - a count that doubles as the "is it built"
        /// flag, so the fill and its geometry cannot disagree.
        Mesh mFillMesh{"NeonRenderer.Fill"};
        Mesh mRingMesh{"NeonRenderer.Ring"};                           ///< The edge ring's annulus, FULL-RES rect-local px; see setupRingGeometry.
        Mesh mBlitMesh{"NeonRenderer.BlitArea"};                       ///< The lit area outside the ring, the same; empty whenever nothing there can be lit.
        Mesh mGatherMesh{"NeonRenderer.GatherArea"};                   ///< The gather pass's quad - pass 1b's and the ring's, padded - in SCALED rect-local px.
        VertexArray mFullscreenVertexArray{"NeonRenderer.Fullscreen"}; ///< NDC quad: emission bake, and the ALL-mode opaque fill when a clear cannot stand in.
        Annulus mGlowBounds;                                           ///< Pass 1b's quad; setupGeometry.
        Annulus mGatherBounds;                                         ///< The gather quad; setupRingGeometry.
        Annulus mRingBounds;                                           ///< The edge ring; setupRingGeometry.
        glm::vec2 mScaledOuter{0.0f};                                  ///< What the blit reads of mScaledBuffer, half-extents in FULL-RES px; setupRingGeometry.
        ShadeLimits mGlowLimits;                                       ///< Pass 1b and its field bake, scaled px; see @ref ShadeLimits.
        ShadeLimits mRingLimits;                                       ///< The edge ring and its field bake, full-res px.

        /// How deep inside the edge the glow can still write a non-zero pixel,
        /// FULL-RES px - the interior counterpart of @c mGlowLimits.quadMargin, which
        /// @ref setupGeometry cuts the quad's hole at and @ref setupRingGeometry
        /// bounds the blit with. See GetGlowInnerReach.
        float mGlowInnerReach = 0.0f;

        /// The emission bound mGlowInnerReach was solved for (GetGlowEmissionBound).
        /// It moves with the arcs' intensities and the segments' boosts, which
        /// are not otherwise geometry inputs - @ref OnConfigChanged compares
        /// against it rather than rebuilding on every arc or segment change.
        float mGlowEmission = 0.0f;

        // --- Offscreen buffers, in pass order, each with what it holds ------
        EmissionTable mEmission;   ///< P0 - see @ref EmissionTable.
        GlowCoverTable mGlowCover; ///< P0b - see @ref GlowCoverTable.
        GatherTarget mGather;      ///< P1a - see @ref GatherTarget.
        GlowField mGlowField;      ///< P1f, read by P1c - see @ref GlowField.
        RingField mRingField;      ///< P1r, read by P2r - see @ref RingField.
        OffscreenState mOffscreen; ///< Whether P1a and P1b can be skipped - see @ref OffscreenState.

        /// Pass 1b's target: the composited colour, one RGBA8 attachment at the
        /// reduced scale - full size at 1.0 - covering what the blit reads
        /// (@c mScaledOuter) and never more than the whole reduced viewport -
        /// see GetBufferRegion. Released when the layer is disabled.
        Framebuffer mScaledBuffer{"NeonRenderer.Scaled"};
    };
}

#endif
