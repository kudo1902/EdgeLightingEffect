// Minimal OpenTK 4 sample driving the EdgeLighting C API.
//
// Shows the full lifecycle: bootstrap the native GL loader, create + initialise
// the effect, then per frame apply an animation into a config copy and render.
//
// Controls:  Space = play/pause   ←/→ = cycle animation   Esc = quit
using System;
using EdgeLighting;
using OpenTK.Graphics.OpenGL4;
using OpenTK.Mathematics;
using OpenTK.Windowing.Common;
using OpenTK.Windowing.Desktop;
using OpenTK.Windowing.GraphicsLibraryFramework;

internal sealed class NeonWindow : GameWindow
{
    private EdgeLightingEffect _effect = null!;
    private NeonAnimation? _anim;
    private ElConfig _baseConfig;

    // Rectangle size in framebuffer pixels; re-centred on resize.
    private const float RectWidth = 520.0f;
    private const float RectHeight = 360.0f;

    // Animation presets to cycle through with the arrow keys.
    private static readonly ElAnimationPreset[] Presets =
    {
        ElAnimationPreset.None,
        ElAnimationPreset.Breathing,
        ElAnimationPreset.SegmentTravel,
        ElAnimationPreset.Comet,
        ElAnimationPreset.OutlineTracer,
        ElAnimationPreset.Shimmer,
        ElAnimationPreset.Aurora,
        ElAnimationPreset.HueReverse,
        ElAnimationPreset.Strobe,
        ElAnimationPreset.Heartbeat,
    };
    private int _presetIndex;

    public NeonWindow()
        : base(
            GameWindowSettings.Default,
            new NativeWindowSettings
            {
                ClientSize = new Vector2i(900, 700),
                Title = "EdgeLighting — C# / OpenTK sample",
                // macOS requires a forward-compatible 3.3 core context, matching
                // how the native library was built (PLATFORM_MACOS desktop GL).
                APIVersion = new Version(3, 3),
                Profile = ContextProfile.Core,
                Flags = ContextFlags.ForwardCompatible,
            })
    {
    }

    protected override void OnLoad()
    {
        base.OnLoad();

        Console.WriteLine($"EdgeLighting ABI version: {EdgeLightingLibrary.AbiVersion}");

        // 1. Bootstrap the native library's GLAD with GLFW's resolver. The
        //    context is current here, so GL symbols resolve.
        EdgeLightingLibrary.LoadGl(GlLoader.Create(GLFW.GetProcAddress));

        // 2. Create + initialise the effect (compiles shaders — needs the context).
        _effect = new EdgeLightingEffect();

        // 3. Start from the library defaults, then tailor the look.
        _baseConfig = EdgeLightingLibrary.CreateDefaultConfig();

        _baseConfig.Geometry.Width = RectWidth;
        _baseConfig.Geometry.Height = RectHeight;
        _baseConfig.Geometry.CornerRadius = 48.0f;

        _baseConfig.Wireframe.Enabled = false;

        _baseConfig.Neon.Enabled = true;
        _baseConfig.Neon.LineWidth = 4.0f;
        _baseConfig.Neon.Intensity = 1.2f;
        _baseConfig.Neon.GlowRadius = 16.0f;
        _baseConfig.Neon.BloomStrength = 0.35f;
        _baseConfig.Neon.HueRotationRate = 0.4f;
        _baseConfig.Neon.SetColorStops(new[]
        {
            new ElColorStop(0.00f, 1.0f, 0.0f, 1.0f),
            new ElColorStop(0.50f, 0.0f, 0.8f, 1.0f),
        });

        CenterRect(FramebufferSize.X, FramebufferSize.Y);

        _effect.Initialize();
        _effect.SetConfig(_baseConfig);

        SelectPreset(0);

        // A non-black clear shows that the glow composites over the background
        // (premultiplied-alpha "over") rather than only adding light. Draw your
        // own scene objects before _effect.Render(...) and they'll show through.
        GL.ClearColor(0.05f, 0.06f, 0.09f, 1.0f);
    }

    protected override void OnRenderFrame(FrameEventArgs args)
    {
        base.OnRenderFrame(args);

        GL.Clear(ClearBufferMask.ColorBufferBit);

        // Advance the clock first so the animation reads the new time and
        // play/pause affects it too.
        _effect.Update((float)args.Time);

        // Apply the active animation into a copy of the base config, then push.
        ElConfig frameConfig = _baseConfig;
        _anim?.Apply(ref frameConfig, _effect.Time);
        _effect.SetConfig(frameConfig);

        _effect.Render(FramebufferSize.X, FramebufferSize.Y);

        SwapBuffers();
    }

    protected override void OnUpdateFrame(FrameEventArgs args)
    {
        base.OnUpdateFrame(args);

        if (KeyboardState.IsKeyPressed(Keys.Escape))
        {
            Close();
        }

        if (KeyboardState.IsKeyPressed(Keys.Space))
        {
            if (_effect.IsPlaying)
            {
                _effect.Pause();
            }
            else
            {
                _effect.Play();
            }
        }

        if (KeyboardState.IsKeyPressed(Keys.Right))
        {
            SelectPreset(_presetIndex + 1);
        }
        else if (KeyboardState.IsKeyPressed(Keys.Left))
        {
            SelectPreset(_presetIndex - 1);
        }
    }

    protected override void OnFramebufferResize(FramebufferResizeEventArgs e)
    {
        base.OnFramebufferResize(e);
        GL.Viewport(0, 0, e.Width, e.Height);
        CenterRect(e.Width, e.Height);
        if (_effect is not null)
        {
            _effect.SetConfig(_baseConfig);
        }
    }

    protected override void OnUnload()
    {
        // Dispose on the GL thread — el_effect_destroy deletes GL resources.
        _anim?.Dispose();
        _effect?.Dispose();
        base.OnUnload();
    }

    private void CenterRect(int fbWidth, int fbHeight)
    {
        _baseConfig.Geometry.PosX = (fbWidth - RectWidth) * 0.5f;
        _baseConfig.Geometry.PosY = (fbHeight - RectHeight) * 0.5f;
    }

    private void SelectPreset(int index)
    {
        _presetIndex = (index % Presets.Length + Presets.Length) % Presets.Length;
        ElAnimationPreset preset = Presets[_presetIndex];

        _anim?.Dispose();
        _anim = NeonAnimation.Create(preset);
        _effect.Reset(); // restart the clock so one-shots (FadeIn/Tracer) play from 0

        Console.WriteLine($"Animation: {preset}");
    }

    private static void Main()
    {
        using var window = new NeonWindow();
        window.Run();
    }
}
