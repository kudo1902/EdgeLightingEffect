// Mirrors the enums in lib/capi/edge-lighting-c.h. Underlying type is int32 to
// match the C ABI (C# enums marshal as their underlying type by default).
using System;

namespace EdgeLighting
{
    /// <summary>Result codes returned by fallible <c>el_*</c> calls.</summary>
    public enum ElResult
    {
        Ok = 0,
        NullArg = 1,
        GlNotLoaded = 2,
        InitFailed = 3,
        Exception = 4,
    }

    /// <summary>Direction of traversal around the perimeter.</summary>
    public enum ElWinding
    {
        Clockwise = 0,
        CounterClockwise = 1,
    }

    /// <summary>Which side of the edge the glow is emitted from.</summary>
    public enum ElGlowSide
    {
        Both = 0,
        Inside = 1,
        Outside = 2,
    }

    /// <summary>Colour space used to interpolate between colour stops.</summary>
    public enum ElBlendSpace
    {
        Rgb = 0,
        Hsv = 1,
    }

    /// <summary>Playback mode of an animation (mirror EdgeLighting::PlaybackMode).</summary>
    public enum ElPlaybackMode
    {
        /// <summary>Plays forever; never completes.</summary>
        Loop = 0,
        /// <summary>Plays for the configured duration, then stops.</summary>
        OneShot = 1,
    }

    /// <summary>
    /// Identifies one of the built-in renderers for enable/disable toggling.
    /// Values match the registration order in <c>el_effect_create</c>.
    /// </summary>
    public enum ElRendererKind
    {
        Wireframe = 0,
        Neon = 1,
        MultipassNeon = 2,
        OptimizedNeon = 3,
    }

    /// <summary>Easing curves (mirror EdgeLighting::EasingFunction::*).</summary>
    public enum ElEasing
    {
        Linear = 0,
        InQuad = 1,
        OutQuad = 2,
        InOutQuad = 3,
        InCubic = 4,
        OutCubic = 5,
        InOutCubic = 6,
        InSine = 7,
        OutSine = 8,
        InOutSine = 9,
        InExpo = 10,
        OutExpo = 11,
        InOutExpo = 12,
    }

    /// <summary>Ready-made animation presets (the demo's showcase set).</summary>
    public enum ElAnimationPreset
    {
        None = 0,
        Breathing = 1,
        Strobe = 2,
        Heartbeat = 3,
        Shimmer = 4,
        Aurora = 5,
        ReverseSweep = 6,
        FadeIn = 7,
        SegmentTravel = 8,
        SegmentBounce = 9,
        Comet = 10,
        OutlineTracer = 11,
        FadeOut = 12,
        HueReverse = 13,
    }

    /// <summary>Thrown when a native <c>el_*</c> call reports failure.</summary>
    public sealed class EdgeLightingException : Exception
    {
        public ElResult Result { get; }

        public EdgeLightingException(string message, ElResult result = ElResult.Exception)
            : base(message)
        {
            Result = result;
        }
    }
}
