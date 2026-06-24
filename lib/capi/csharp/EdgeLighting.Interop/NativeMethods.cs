// Raw P/Invoke surface over libedge-lighting-c. One extern per el_* symbol.
//
// All entry points use Cdecl (the C ABI on macOS/Linux/Windows-x64). The
// library name "edge-lighting-c" is resolved by .NET to libedge-lighting-c.dylib
// / .so / edge-lighting-c.dll automatically — copy that native binary next to
// the managed assembly (the sample .csproj does this).
using System;
using System.Runtime.InteropServices;

namespace EdgeLighting
{
    internal static class NativeMethods
    {
        private const string Lib = "edge-lighting-c";
        private const CallingConvention Cc = CallingConvention.Cdecl;

        // --- Library-level ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern uint el_abi_version();

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_load_gl(IntPtr getProc);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_last_error();

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_config_default(out ElConfig outCfg);

        // --- Effect lifecycle ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_effect_create();

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_destroy(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_effect_initialize(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_effect_set_config(IntPtr fx, in ElConfig cfg);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_effect_get_config(IntPtr fx, out ElConfig outCfg);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_effect_update(IntPtr fx, float deltaTime);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_effect_render(IntPtr fx, int viewportWidth, int viewportHeight);

        // --- Clock ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_play(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_pause(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_stop(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_reset(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_effect_set_time(IntPtr fx, float time);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern float el_effect_get_time(IntPtr fx);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern int el_effect_is_playing(IntPtr fx);

        // --- Animations: preset + parametric factories ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_create(ElAnimationPreset preset);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_intensity_pulse(float duration, float min, float max);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_intensity_strobe(float duration, float offIntensity, float onIntensity);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_intensity_fade_in(float target, float duration, ElEasing easing);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_intensity_fade_out(float start, float duration, ElEasing easing);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_glow_radius_breath(float duration, float minRadius, float maxRadius);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_bloom_pulse(float duration, float min, float max);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_hue_rotation_reverse(float baseRate, float duration);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_hue_rotation_ease_reverse(float maxRate, float duration);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_segment_travel(float duration, float length, float boost);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_segment_bounce(float duration, float length, float boost);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern IntPtr el_animation_outline_tracer(float duration, ElEasing easing);

        // --- Animation control ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_animation_destroy(IntPtr anim);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElResult el_animation_apply(IntPtr anim, ref ElConfig cfg, float elapsed);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern int el_animation_is_complete(IntPtr anim, float elapsed);

        // --- Playback mode (orthogonal to duration) ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern ElPlaybackMode el_animation_get_playback_mode(IntPtr anim);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_animation_set_playback_mode(IntPtr anim, ElPlaybackMode mode);

        // --- Duration (wall-clock seconds) ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern float el_animation_get_duration(IntPtr anim);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_animation_set_duration(IntPtr anim, float seconds);

        // --- Speed (playback rate multiplier) ---
        [DllImport(Lib, CallingConvention = Cc)]
        public static extern float el_animation_get_speed(IntPtr anim);

        [DllImport(Lib, CallingConvention = Cc)]
        public static extern void el_animation_set_speed(IntPtr anim, float speed);
    }
}
