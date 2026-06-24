// Blittable mirrors of the EL_* POD structs in lib/capi/edge-lighting-c.h.
//
// Every field is a 4-byte float/int (no padding), so LayoutKind.Sequential
// produces a byte-for-byte identical layout to the C structs and the whole
// ElConfig marshals with zero copies — pass it by `in`/`ref`/`out`.
//
// The colour-stop arrays use `fixed float` buffers (a C# `fixed` buffer can
// only hold primitives), so the config structs are `unsafe`. SetColorStops /
// GetColorStops hide that detail.
using System;
using System.Runtime.InteropServices;

namespace EdgeLighting
{
    /// <summary>One colour stop along the perimeter. RGBA in [0,1].</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElColorStop
    {
        public float Position;
        public float R;
        public float G;
        public float B;
        public float A;

        public ElColorStop(float position, float r, float g, float b, float a = 1.0f)
        {
            Position = position;
            R = r;
            G = g;
            B = b;
            A = a;
        }
    }

    /// <summary>Geometry of the target rounded rectangle.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElRectGeometry
    {
        public float Width;
        public float Height;
        public float PosX;
        public float PosY;
        public float CornerRadius;
        public int WindingRaw;

        public ElWinding Winding
        {
            get => (ElWinding)WindingRaw;
            set => WindingRaw = (int)value;
        }
    }

    /// <summary>Single-pass neon renderer settings.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ElNeonConfig
    {
        public const int MaxColorStops = 16;

        public int EnableRaw;
        public float LineWidth;
        public float Intensity;
        public float GlowRadius;
        public float BloomStrength;
        public int GlowSideRaw;
        public float GlowSideSoftness;
        public int BlendSpaceRaw;
        public int ColorStopCount;
        public fixed float ColorStops[MaxColorStops * 5];
        public float HueRotationRate;
        public float SegmentPosition;
        public float SegmentLength;
        public float SegmentBoost;
        public float ArcStart;
        public float ArcLength;

        public bool Enabled
        {
            get => EnableRaw != 0;
            set => EnableRaw = value ? 1 : 0;
        }

        public ElGlowSide GlowSide
        {
            get => (ElGlowSide)GlowSideRaw;
            set => GlowSideRaw = (int)value;
        }

        public ElBlendSpace BlendSpace
        {
            get => (ElBlendSpace)BlendSpaceRaw;
            set => BlendSpaceRaw = (int)value;
        }

        /// <summary>Replace the colour stops (up to 16; extras are ignored).</summary>
        public void SetColorStops(ReadOnlySpan<ElColorStop> stops)
        {
            int n = Math.Min(stops.Length, MaxColorStops);
            ColorStopCount = n;
            fixed (float* p = ColorStops)
            {
                for (int i = 0; i < n; i++)
                {
                    p[i * 5 + 0] = stops[i].Position;
                    p[i * 5 + 1] = stops[i].R;
                    p[i * 5 + 2] = stops[i].G;
                    p[i * 5 + 3] = stops[i].B;
                    p[i * 5 + 4] = stops[i].A;
                }
            }
        }

        /// <summary>Read back the active colour stops.</summary>
        public ElColorStop[] GetColorStops()
        {
            int n = Math.Clamp(ColorStopCount, 0, MaxColorStops);
            var result = new ElColorStop[n];
            fixed (float* p = ColorStops)
            {
                for (int i = 0; i < n; i++)
                {
                    result[i] = new ElColorStop(
                        p[i * 5 + 0], p[i * 5 + 1], p[i * 5 + 2], p[i * 5 + 3], p[i * 5 + 4]);
                }
            }
            return result;
        }
    }

    /// <summary>Multi-pass (gradient-to-FBO + separable blur) neon settings.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ElMultiPassNeonConfig
    {
        public const int MaxColorStops = 16;

        public int EnableRaw;
        public int ShowPerimeterGradientRaw;
        public int ShowFullGradientRaw;
        public float LineWidth;
        public float Intensity;
        public float GlowRadius;
        public float BloomStrength;
        public int GlowSideRaw;
        public float GlowSideSoftness;
        public int BlendSpaceRaw;
        public int ColorStopCount;
        public fixed float ColorStops[MaxColorStops * 5];
        public float HueRotationRate;

        public bool Enabled
        {
            get => EnableRaw != 0;
            set => EnableRaw = value ? 1 : 0;
        }

        public bool ShowPerimeterGradient
        {
            get => ShowPerimeterGradientRaw != 0;
            set => ShowPerimeterGradientRaw = value ? 1 : 0;
        }

        public bool ShowFullGradient
        {
            get => ShowFullGradientRaw != 0;
            set => ShowFullGradientRaw = value ? 1 : 0;
        }

        public ElGlowSide GlowSide
        {
            get => (ElGlowSide)GlowSideRaw;
            set => GlowSideRaw = (int)value;
        }

        public ElBlendSpace BlendSpace
        {
            get => (ElBlendSpace)BlendSpaceRaw;
            set => BlendSpaceRaw = (int)value;
        }

        public void SetColorStops(ReadOnlySpan<ElColorStop> stops)
        {
            int n = Math.Min(stops.Length, MaxColorStops);
            ColorStopCount = n;
            fixed (float* p = ColorStops)
            {
                for (int i = 0; i < n; i++)
                {
                    p[i * 5 + 0] = stops[i].Position;
                    p[i * 5 + 1] = stops[i].R;
                    p[i * 5 + 2] = stops[i].G;
                    p[i * 5 + 3] = stops[i].B;
                    p[i * 5 + 4] = stops[i].A;
                }
            }
        }
    }

    /// <summary>Half-resolution optimised neon settings (shares neon visuals).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElOptimizedNeonConfig
    {
        public int EnableRaw;
        public float ResolutionScale;
        public int NumSamples;
        public int GradientLutSize;
        public int ShowHalfResRaw;

        public bool Enabled
        {
            get => EnableRaw != 0;
            set => EnableRaw = value ? 1 : 0;
        }

        public bool ShowHalfRes
        {
            get => ShowHalfResRaw != 0;
            set => ShowHalfResRaw = value ? 1 : 0;
        }
    }

    /// <summary>Wireframe debug overlay settings.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElWireframeConfig
    {
        public int EnableRaw;
        public float R;
        public float G;
        public float B;
        public float A;

        public bool Enabled
        {
            get => EnableRaw != 0;
            set => EnableRaw = value ? 1 : 0;
        }
    }

    /// <summary>Top-level configuration: one sub-config per renderer.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElConfig
    {
        public ElRectGeometry Geometry;
        public ElNeonConfig Neon;
        public ElMultiPassNeonConfig MultipassNeon;
        public ElOptimizedNeonConfig OptimizedNeon;
        public ElWireframeConfig Wireframe;
    }
}
