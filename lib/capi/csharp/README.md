# EdgeLighting — C# bindings

P/Invoke bindings for the EdgeLighting C API (`lib/capi/edge-lighting-c.h`) plus
a runnable OpenTK sample.

```
EdgeLighting.Interop/   reusable binding library (no GL/windowing dependency)
  Enums.cs              ElResult / ElGlowSide / ElEasing / ElAnimationPreset …
  Structs.cs            blittable ElConfig + sub-configs (1:1 with the C structs)
  NativeMethods.cs      raw [DllImport] surface (one per el_* symbol)
  EdgeLighting.cs       EdgeLightingEffect / NeonAnimation / GlLoader wrappers
EdgeLighting.Sample/    OpenTK 4 window that drives the effect + animations
```

## 1. Build the native library

From the repo root:

```bash
cmake -S . -B build -G Ninja
cmake --build build --target edge-lighting-c
# produces build/lib/libedge-lighting-c.dylib (.so on Linux, .dll on Windows)
```

## 2. Run the sample

```bash
cd lib/capi/csharp
dotnet run --project EdgeLighting.Sample
```

The sample `.csproj` copies the native library from `build/lib/` next to the
managed output, so `DllImport("edge-lighting-c")` resolves it. **Controls:**
`Space` play/pause · `←`/`→` cycle animation · `Esc` quit.

## Usage outline

```csharp
// Once, after your GL context is current (here: OpenTK's GLFW):
EdgeLightingLibrary.LoadGl(GlLoader.Create(GLFW.GetProcAddress));

using var effect = new EdgeLightingEffect();   // registers all renderers
effect.Initialize();                           // compiles shaders

ElConfig cfg = EdgeLightingLibrary.CreateDefaultConfig();
cfg.Neon.Enabled = true;
cfg.Neon.SetColorStops(new[] {
    new ElColorStop(0.0f, 1, 0, 1),
    new ElColorStop(0.5f, 0, 0.8f, 1),
});
effect.SetConfig(cfg);

using var anim = NeonAnimation.Create(ElAnimationPreset.SegmentTravel);

// Per frame, on the GL thread:
effect.Update(dt);
var frame = cfg;
anim?.Apply(ref frame, effect.Time);
effect.SetConfig(frame);
effect.Render(framebufferWidth, framebufferHeight);
```

## Things to know

- **GL thread affinity.** Every call that touches GL — `Initialize`, `Update`,
  `Render`, and `Dispose` (it deletes GL resources) — must run on the thread
  that owns the GL context. `EdgeLightingEffect` has no finalizer for this
  reason; dispose it explicitly on the GL thread.
- **The library is context-less.** It renders into whatever framebuffer is
  current; it never clears the screen. Clear and draw your own scene first — the
  neon composites over it (premultiplied-alpha "over").
- **HiDPI.** Pass the *framebuffer* pixel size (not logical client size) to
  `Render` and position geometry in those pixels. The sample uses
  `FramebufferSize`.
- **Platform / context.** The macOS build is desktop GL 3.3 Core, so the host
  context must be 3.3 Core forward-compatible (the sample sets this). The
  Windows/Linux builds target GLES 3.0 — create a matching context there.
- **Architecture match.** The managed process and the native `.dylib`/`.so`/
  `.dll` must be the same architecture (e.g. arm64 .NET with an arm64 build).
```
