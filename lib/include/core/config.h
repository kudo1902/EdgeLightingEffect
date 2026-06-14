#ifndef _EDGE_LIGHTING_CONFIG_H_
#define _EDGE_LIGHTING_CONFIG_H_

#include <glm/glm.hpp>
#include <vector>

namespace EdgeLighting
{
    /// Source of the lighting path geometry.
    typedef enum class PathSource
    {
        RECT = 0,   ///< Rectangle edge (current analytic SDF)
        CUSTOM = 1, ///< User-provided polyline points
        MASK = 2,   ///< Contour extracted from a binary mask
    } PathSource;

    /// Controls whether the stroke boundary sits on, inside, or outside the rectangle edge.
    typedef enum class StrokeAlignment
    {
        CENTER, ///< Stroke straddles the rectangle edge
        INNER,  ///< Stroke sits entirely inside the rectangle
        OUTER   ///< Stroke sits entirely outside the rectangle
    } StrokeAlignment;

    /// Determines the stroke animation mode.
    typedef enum class StrokeAnimation
    {
        STATIC,  ///< Full stroke around the entire perimeter
        MOVING,  ///< A moving segment travels around the perimeter
        FLASHING ///< Full stroke blinks on/off at speed frequency
    } StrokeAnimation;

    /// Controls which side of the stroke boundary gets the soft fade.
    typedef enum class FadeMode
    {
        SINGLE, ///< Fade applies only to the far edge (away from the boundary)
        DOUBLE  ///< Fade applies to both edges of the stroke
    } FadeMode;

    /// Direction of traversal around the perimeter.
    /// For RECT mode, this controls the physical direction around the rectangle edge.
    /// For CUSTOM/MASK modes, the vertex order in Path::points defines the CCW direction;
    /// CLOCKWISE simply reverses it (traverses points in reverse order).
    /// There is no automatic winding detection — the vertex order is taken as-is.
    typedef enum class Winding
    {
        CLOCKWISE,
        COUNTER_CLOCKWISE
    } Winding;

    /// Determines how colors are computed for the stroke.
    typedef enum class StrokeColorMode
    {
        STATIC,       ///< Single fixed color (@ref Stroke::primaryColor)
        GRADIENT,     ///< Blends primaryColor -> secondaryColor -> primaryColor around the perimeter
        RAINBOW,      ///< Hue shifts along the perimeter (0-360°)
        RAINBOW_TIME, ///< Uniform hue shift over time across the entire stroke
        PULSE         ///< Uniform oscillation between primaryColor and secondaryColor over time
    } StrokeColorMode;

    /// Top-level configuration for the EdgeLightingEffect pipeline.
    typedef struct Config
    {
        /// Geometry of the target rectangle.
        typedef struct Geometry
        {
            float width = 800.0f;                       ///< Rectangle width in pixels
            float height = 600.0f;                      ///< Rectangle height in pixels
            glm::vec2 position = glm::vec2(0.0f, 0.0f); ///< Top-left corner in viewport coordinates
            float borderRadius = 40.0f;                 ///< Corner radius in pixels (0 = sharp corners)
            /// Traversal direction around the perimeter.
            /// For CUSTOM/MASK, vertex order defines CCW; CW reverses it.
            Winding winding = Winding::COUNTER_CLOCKWISE;
        } Geometry;

        /// Stroke rendering configuration.
        typedef struct Stroke
        {
            bool enable = true; ///< Enable or disable the stroke renderer

            // --- Appearance ---

            /// Stroke thickness in pixels (diameter for CENTER alignment, full width for INNER/OUTER).
            float thickness = 3.0f;
            /// Global opacity multiplier [0-1].
            float intensity = 1.0f;
            /// Whether the stroke sits centered on, inside, or outside the rectangle edge.
            StrokeAlignment alignment = StrokeAlignment::CENTER;

            // --- Color ---

            /// Primary color (used in STATIC mode, and as first blend color in GRADIENT/PULSE modes).
            glm::vec4 primaryColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            /// Secondary color (used as second blend color in GRADIENT/PULSE modes).
            glm::vec4 secondaryColor = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            /// Active color rendering mode.
            StrokeColorMode colorMode = StrokeColorMode::STATIC;

            // --- Edge feathering ---

            /// Soft fade distance from the stroke edge in pixels (0 = hard edge, up to thickness).
            float fadeRange = 0.0f;
            /// Which side(s) of the stroke receive the fade.
            FadeMode fadeMode = FadeMode::SINGLE;

            // --- Animation ---

            /// Whether the stroke is static or has a moving segment.
            StrokeAnimation animation = StrokeAnimation::STATIC;
            /// Length of the moving segment as a fraction of the total perimeter [0-1].
            float segmentLength = 0.25f;
            /// Travel speed of the moving segment (revolutions per second).
            /// No enforced limit. Very low values (< 0.01) appear frozen;
            /// very high values (> 100) may flicker.
            float speed = 0.5f;
            /// Number of simultaneous moving segments spread evenly around the perimeter.
            int lineCount = 1;

            // --- Glow ---

            /// Enable a wider, fainter glow behind the core stroke.
            bool glowEnable = false;
            /// Extra pixels extending beyond the stroke edge.
            float glowSize = 5.0f;
            /// Opacity multiplier for the glow pass.
            float glowIntensity = 0.25f;
        } Stroke;

        /// Wireframe debug overlay configuration.
        typedef struct Wireframe
        {
            bool enable = true;                                  ///< Show or hide the wireframe bounding box
            glm::vec4 color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); ///< Wireframe color
        } Wireframe;

        /// Particle trail configuration.
        typedef struct Particles
        {
            bool enable = true;     ///< Enable or disable particle emission
            int maxCount = 200;     ///< Maximum number of particles alive at once
            float size = 6.0f;      ///< Particle point size in pixels
            float intensity = 1.0f; ///< Global opacity multiplier for particles
            /// Particle color override. Zero alpha (= default) uses auto color
            /// derived from the stroke color mode at the spawn point.
            glm::vec4 color = glm::vec4(0.0f);
        } Particles;

        /// Lighting path definition.
        typedef struct Path
        {
            /// Source of the path geometry.
            PathSource source = PathSource::RECT;
            /// Whether the path forms a closed loop.
            bool closed = true;
            /// Starting position of the light head along the path [0-1].
            float startPos = 0.0f;
            /// End position of the light head along the path [0-1].
            float endPos = 1.0f;
            /// Polyline points in local space (for CUSTOM and MASK modes).
            /// Vertex order defines the CCW traversal direction.
            /// When Winding::CLOCKWISE is set, points are traversed in reverse.
            std::vector<glm::vec2> points;
        } Path;

        Geometry geometry;   ///< Rectangle geometry
        Stroke stroke;       ///< Stroke rendering settings
        Wireframe wireframe; ///< Wireframe overlay settings
        Particles particles; ///< Particle trail settings
        Path path;           ///< Path source + start/end configuration
    } Config;

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_CONFIG_H_
