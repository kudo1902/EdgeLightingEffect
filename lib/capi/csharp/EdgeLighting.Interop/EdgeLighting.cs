// Idiomatic, safe wrappers over the raw NativeMethods surface:
//   - EdgeLightingLibrary : process-level helpers (GL bootstrap, defaults).
//   - GlLoader            : turns a managed resolver into a C function pointer.
//   - EdgeLightingEffect  : IDisposable handle around an EL_Effect.
//   - NeonAnimation       : IDisposable handle around an EL_Animation.
//
// GL THREAD AFFINITY: every call that touches GL (Initialize / Update / Render
// and, crucially, Dispose — which deletes GL resources) must run on the thread
// that owns the GL context. Dispose the effect on that thread.
using System;
using System.Runtime.InteropServices;

namespace EdgeLighting
{
    /// <summary>Process-level helpers that aren't tied to a single effect.</summary>
    public static class EdgeLightingLibrary
    {
        /// <summary>ABI version of the loaded native binary.</summary>
        public static uint AbiVersion => NativeMethods.el_abi_version();

        /// <summary>Last error message recorded on the current thread.</summary>
        public static string LastError =>
            Marshal.PtrToStringAnsi(NativeMethods.el_last_error()) ?? string.Empty;

        /// <summary>
        /// Bootstrap the native library's GL function pointers. Call once, after
        /// the host GL context is current, before creating/initialising effects.
        /// </summary>
        /// <param name="getProcAddress">
        /// A C function pointer of the form <c>void* (*)(const char*)</c>. Build
        /// one from a managed resolver with <see cref="GlLoader.Create"/>.
        /// </param>
        public static void LoadGl(IntPtr getProcAddress)
        {
            Check(NativeMethods.el_load_gl(getProcAddress), nameof(NativeMethods.el_load_gl));
        }

        /// <summary>A fresh config populated with the library's defaults.</summary>
        public static ElConfig CreateDefaultConfig()
        {
            Check(NativeMethods.el_config_default(out ElConfig cfg), nameof(NativeMethods.el_config_default));
            return cfg;
        }

        internal static void Check(ElResult result, string op)
        {
            if (result != ElResult.Ok)
            {
                throw new EdgeLightingException($"{op} failed: {result} ({LastError})", result);
            }
        }
    }

    /// <summary>
    /// Adapts a managed <c>name → address</c> resolver (e.g. GLFW.GetProcAddress)
    /// into a Cdecl C function pointer suitable for <see cref="EdgeLightingLibrary.LoadGl"/>.
    /// </summary>
    public static class GlLoader
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate IntPtr GetProcDelegate(IntPtr namePtr);

        // Keep the delegate alive for the process lifetime so the GC can't
        // collect it while native code still holds the function pointer.
        private static GetProcDelegate? _keepAlive;

        /// <summary>
        /// Wrap <paramref name="resolver"/> and return a native function pointer.
        /// The resolver is called with each GL symbol name GLAD wants to load.
        /// </summary>
        public static IntPtr Create(Func<string, IntPtr> resolver)
        {
            if (resolver is null)
            {
                throw new ArgumentNullException(nameof(resolver));
            }

            _keepAlive = namePtr =>
            {
                string name = Marshal.PtrToStringAnsi(namePtr) ?? string.Empty;
                return resolver(name);
            };
            return Marshal.GetFunctionPointerForDelegate(_keepAlive);
        }
    }

    /// <summary>Managed handle around a native EL_Effect.</summary>
    public sealed class EdgeLightingEffect : IDisposable
    {
        private IntPtr _handle;

        /// <summary>Create an effect with all renderers registered (not yet initialised).</summary>
        public EdgeLightingEffect()
        {
            _handle = NativeMethods.el_effect_create();
            if (_handle == IntPtr.Zero)
            {
                throw new EdgeLightingException(
                    $"el_effect_create failed ({EdgeLightingLibrary.LastError})");
            }
        }

        private IntPtr Handle =>
            _handle != IntPtr.Zero
                ? _handle
                : throw new ObjectDisposedException(nameof(EdgeLightingEffect));

        /// <summary>Compile shaders + build geometry. Requires a current GL context.</summary>
        public void Initialize() =>
            EdgeLightingLibrary.Check(NativeMethods.el_effect_initialize(Handle), "el_effect_initialize");

        /// <summary>Replace the active configuration and notify all renderers.</summary>
        public void SetConfig(in ElConfig config) =>
            EdgeLightingLibrary.Check(NativeMethods.el_effect_set_config(Handle, config), "el_effect_set_config");

        /// <summary>Copy out the active configuration.</summary>
        public ElConfig GetConfig()
        {
            EdgeLightingLibrary.Check(NativeMethods.el_effect_get_config(Handle, out ElConfig cfg), "el_effect_get_config");
            return cfg;
        }

        /// <summary>Advance animation time by <paramref name="deltaTime"/> seconds.</summary>
        public void Update(float deltaTime) =>
            EdgeLightingLibrary.Check(NativeMethods.el_effect_update(Handle, deltaTime), "el_effect_update");

        /// <summary>Render all enabled renderers into the current framebuffer.</summary>
        public void Render(int viewportWidth, int viewportHeight) =>
            EdgeLightingLibrary.Check(NativeMethods.el_effect_render(Handle, viewportWidth, viewportHeight), "el_effect_render");

        // --- Clock ---
        public void Play() => NativeMethods.el_effect_play(Handle);
        public void Pause() => NativeMethods.el_effect_pause(Handle);
        public void Stop() => NativeMethods.el_effect_stop(Handle);
        public void Reset() => NativeMethods.el_effect_reset(Handle);

        /// <summary>Accumulated clock time in seconds (settable for scrubbing).</summary>
        public float Time
        {
            get => NativeMethods.el_effect_get_time(Handle);
            set => NativeMethods.el_effect_set_time(Handle, value);
        }

        public bool IsPlaying => NativeMethods.el_effect_is_playing(Handle) != 0;

        // --- Per-renderer enable/disable ---
        //
        // Each renderer's `enable` flag lives inside its sub-config. These
        // helpers wrap the GetConfig → mutate → SetConfig dance so the host
        // can flip a single renderer without rebuilding the whole snapshot.

        /// <summary>Enable or disable one of the built-in renderers.</summary>
        public void SetRendererEnabled(ElRendererKind kind, bool enabled) =>
            EdgeLightingLibrary.Check(
                NativeMethods.el_effect_set_renderer_enabled(Handle, kind, enabled ? 1 : 0),
                "el_effect_set_renderer_enabled");

        /// <summary>True if the given renderer is currently enabled.</summary>
        public bool IsRendererEnabled(ElRendererKind kind) =>
            NativeMethods.el_effect_is_renderer_enabled(Handle, kind) != 0;

        /// <summary>Toggle the 1px line-loop debug box.</summary>
        public bool WireframeEnabled
        {
            get => IsRendererEnabled(ElRendererKind.Wireframe);
            set => SetRendererEnabled(ElRendererKind.Wireframe, value);
        }

        /// <summary>Toggle the single-pass neon stroke.</summary>
        public bool NeonEnabled
        {
            get => IsRendererEnabled(ElRendererKind.Neon);
            set => SetRendererEnabled(ElRendererKind.Neon, value);
        }

        /// <summary>Toggle the multi-pass (FBO + separable blur) neon.</summary>
        public bool MultipassNeonEnabled
        {
            get => IsRendererEnabled(ElRendererKind.MultipassNeon);
            set => SetRendererEnabled(ElRendererKind.MultipassNeon, value);
        }

        /// <summary>Toggle the half-resolution neon for edge devices.</summary>
        public bool OptimizedNeonEnabled
        {
            get => IsRendererEnabled(ElRendererKind.OptimizedNeon);
            set => SetRendererEnabled(ElRendererKind.OptimizedNeon, value);
        }

        /// <summary>Destroy the native effect. Call on the GL thread.</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                NativeMethods.el_effect_destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }

    /// <summary>
    /// Managed handle around a native EL_Animation. Animations are pure CPU
    /// (no GL), so a finalizer backstop is safe here.
    /// </summary>
    public sealed class NeonAnimation : IDisposable
    {
        private IntPtr _handle;

        private NeonAnimation(IntPtr handle, string op)
        {
            if (handle == IntPtr.Zero)
            {
                throw new EdgeLightingException($"{op} returned null ({EdgeLightingLibrary.LastError})");
            }
            _handle = handle;
        }

        private IntPtr Handle =>
            _handle != IntPtr.Zero
                ? _handle
                : throw new ObjectDisposedException(nameof(NeonAnimation));

        /// <summary>Build a preset animation. Returns null for <see cref="ElAnimationPreset.None"/>.</summary>
        public static NeonAnimation? Create(ElAnimationPreset preset)
        {
            if (preset == ElAnimationPreset.None)
            {
                return null;
            }
            return new NeonAnimation(NativeMethods.el_animation_create(preset), "el_animation_create");
        }

        // --- Parametric factories (mirror animation/neon-animations.h) ---
        // For periodic animations, `duration` is seconds per cycle.
        public static NeonAnimation IntensityPulse(float duration = 1.0f, float min = 0.4f, float max = 1.0f) =>
            new(NativeMethods.el_animation_intensity_pulse(duration, min, max), "el_animation_intensity_pulse");

        public static NeonAnimation IntensityStrobe(float duration = 0.25f, float offIntensity = 0.0f, float onIntensity = 1.0f) =>
            new(NativeMethods.el_animation_intensity_strobe(duration, offIntensity, onIntensity), "el_animation_intensity_strobe");

        public static NeonAnimation IntensityFadeIn(float target = 1.0f, float duration = 0.4f, ElEasing easing = ElEasing.OutCubic) =>
            new(NativeMethods.el_animation_intensity_fade_in(target, duration, easing), "el_animation_intensity_fade_in");

        public static NeonAnimation IntensityFadeOut(float start = 1.0f, float duration = 0.4f, ElEasing easing = ElEasing.InCubic) =>
            new(NativeMethods.el_animation_intensity_fade_out(start, duration, easing), "el_animation_intensity_fade_out");

        public static NeonAnimation GlowRadiusBreath(float duration = 2.0f, float minRadius = 4.0f, float maxRadius = 12.0f) =>
            new(NativeMethods.el_animation_glow_radius_breath(duration, minRadius, maxRadius), "el_animation_glow_radius_breath");

        public static NeonAnimation BloomPulse(float duration = 1.5f, float min = 0.1f, float max = 0.6f) =>
            new(NativeMethods.el_animation_bloom_pulse(duration, min, max), "el_animation_bloom_pulse");

        public static NeonAnimation HueRotationReverse(float baseRate = 0.5f, float duration = 8.0f) =>
            new(NativeMethods.el_animation_hue_rotation_reverse(baseRate, duration), "el_animation_hue_rotation_reverse");

        public static NeonAnimation HueRotationEaseReverse(float maxRate = 0.5f, float duration = 6.0f) =>
            new(NativeMethods.el_animation_hue_rotation_ease_reverse(maxRate, duration), "el_animation_hue_rotation_ease_reverse");

        public static NeonAnimation SegmentTravel(float duration = 3.0f, float length = 0.15f, float boost = 4.0f) =>
            new(NativeMethods.el_animation_segment_travel(duration, length, boost), "el_animation_segment_travel");

        public static NeonAnimation SegmentBounce(float duration = 4.0f, float length = 0.20f, float boost = 4.0f) =>
            new(NativeMethods.el_animation_segment_bounce(duration, length, boost), "el_animation_segment_bounce");

        public static NeonAnimation OutlineTracer(float duration = 2.0f, ElEasing easing = ElEasing.OutCubic) =>
            new(NativeMethods.el_animation_outline_tracer(duration, easing), "el_animation_outline_tracer");

        /// <summary>Apply this animation at <paramref name="elapsed"/> seconds into <paramref name="config"/>.</summary>
        public void Apply(ref ElConfig config, float elapsed) =>
            EdgeLightingLibrary.Check(NativeMethods.el_animation_apply(Handle, ref config, elapsed), "el_animation_apply");

        /// <summary>True if a one-shot animation has reached its duration at <paramref name="elapsed"/>.</summary>
        public bool IsComplete(float elapsed) => NativeMethods.el_animation_is_complete(Handle, elapsed) != 0;

        // --- Orthogonal axes: mode, duration, speed ---

        /// <summary>Playback mode (Loop or OneShot). Setting it does NOT touch the duration.</summary>
        public ElPlaybackMode PlaybackMode
        {
            get => NativeMethods.el_animation_get_playback_mode(Handle);
            set => NativeMethods.el_animation_set_playback_mode(Handle, value);
        }

        /// <summary>
        /// Configured wall-clock duration in seconds. Consulted by
        /// <see cref="IsComplete"/> only when <see cref="PlaybackMode"/> is
        /// <see cref="ElPlaybackMode.OneShot"/>. Setting it does NOT touch the mode.
        /// </summary>
        public float Duration
        {
            get => NativeMethods.el_animation_get_duration(Handle);
            set => NativeMethods.el_animation_set_duration(Handle, value);
        }

        /// <summary>Playback rate multiplier (1.0 = normal, 0.5 = half, 2.0 = double).</summary>
        public float Speed
        {
            get => NativeMethods.el_animation_get_speed(Handle);
            set => NativeMethods.el_animation_set_speed(Handle, value);
        }

        public void Dispose()
        {
            DisposeCore();
            GC.SuppressFinalize(this);
        }

        ~NeonAnimation() => DisposeCore();

        private void DisposeCore()
        {
            if (_handle != IntPtr.Zero)
            {
                NativeMethods.el_animation_destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
