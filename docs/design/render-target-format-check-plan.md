# Checking render-target formats before allocating them: implementation plan

Status: **built 2026-10-27 but for step 6** (reading the report on the Tizen
device), not committed; **section 10 is what was built and measured.** Revised
the same day to one central `RenderTargetFormats`. Steps 1-3 were built
first in the FIRST design - each renderer probing its own lists in its
`Initialize`, and a process-wide refusal hook on `Framebuffer` - and step 3R
reworked them into the design below the same day; steps 4-7 follow it. Section 4.0 says why
the design changed, and the built notes under steps 1-3 say what the first
design verified. The measurements in section 3 come from a standalone probe
run on an AMD Radeon Pro 5300M (macOS GL 4.1, Release, `-O2`), three runs;
step 1's `IsColorRenderable` reproduces its table.

The short version: one function asks the driver whether it can render to a
format (a 4 x 4 texture, an FBO, `glCheckFramebufferStatus`). One object per
effect - `RenderTargetFormats`, owned by `EdgeLightingEffect` and handed to
every renderer the way the shared-buffer registry is - asks it once per format
and keeps the answer. The startup report logs those answers; every format walk
asks the object at allocation and starts at the first row the driver renders;
the half-float fields ask it before their programs are built; and a refusal
hook on the object lets `neon-scale-check` refuse formats for one effect, so
the 8-bit fallback rows finally run somewhere.

## 1. The problem

The library allocates ten kinds of offscreen buffer. Seven ask for a format a
conforming driver may refuse:

| buffer | owner, allocated | formats, best first | if none renders | drawn | read |
| ------ | ---------------- | ------------------- | --------------- | ----- | ---- |
| emission table `mEmission.buffer` | neon, `Initialize` | RGBA16F, RGBA8 | `Initialize` fails | unblended | `texelFetch` |
| glow coverage table `mGlowCover.buffer` | neon, first bake | RGBA16F / RG16F, RGBA8 / RG8 (one tier for both lists) | that frame's pass fails | unblended | linear |
| segment table `mGlowCover.segBuffer` | neon, first segment bake | RG16F, RG8 (the main table's tier) | bake direct | unblended | linear |
| gather `mGather.buffer` | neon, every frame that gathers | RGBA16F, RGBA8 | that frame's pass fails | unblended | linear |
| reduced / shared `mScaled` | neon, via `SharedBuffer::BeginWrite` | RGBA16F, RGBA8 | not ready | unblended | linear (blit, readers) |
| glow field `mGlowField.buffer` | neon, first bake | R16F, or RG16F with segments | **no row**: shade directly | unblended | `NEAREST` |
| ring field `mRingField.buffer` | neon, first bake | R16F, or RG16F with segments | **no row**: shade directly | unblended | `NEAREST` |
| flare scaled `mScaledBuffer` | flare, below scale 1.0 | RGBA16F, RGBA8 | not drawn | **blended** (premultiplied over) | linear |
| spotlight scaled `mScaledBuffer` | spotlight, below scale 1.0 | RGBA16F, RGBA8 | not drawn | **blended** (screen) | linear |
| capture `mCaptureBuffer` | `OffscreenCapture` | RGBA8 | capture fails | - | read-back |

The last one asks only for RGBA8, which GL 3.3 core and GLES 3.0 both require
to be colour-renderable; it has nothing to fall back from or to, and stays as
it is.

Before step 1, a driver's support for a format was learned one way:
`Framebuffer::Resize` allocates the buffer at its full size, attaches it, and
`glCheckFramebufferStatus` says no. The walk (`ResizeInBestFormat` in the neon,
`SharedBuffer::BeginWrite`, the flare's and the spotlight's
`resizeScaledBuffer`) then logs the fallback, tries the next row, and keeps the
refusal in its tier so it never asks again. That is correct and it stays - a
format can still fail at full size for reasons a probe cannot see (memory, a
size limit). What it does not give:

1. **No report.** On a device, the only sign that half float does not render
   is two ERROR lines per buffer (`Framebuffer[...] incomplete after resize
   ... (status=0x8cd6)` or `0x8cdd`, then the fallback line), spread over
   whichever frames first allocate each buffer, and nothing at all for a buffer
   the running config never allocates. The progress log's open item "Measure on
   the target: ... half-float render targets" has no tool that answers it.
2. **The fields find out last.** `mGlowField` and `mRingField` have no 8-bit
   row (8 bits would move whole levels: `Fa` runs well past 1 and the tone map
   is steep near 0), so a driver that cannot render R16F / RG16F shades
   directly. But it learns that at the first bake, AFTER
   `ensureFieldPrograms` / `ensureRingFieldPrograms` built three programs
   (bake, composite, ring composite) on the first frame a field-eligible
   config is drawn - the frame that already carries the first-draw compile
   stall (~125 ms on the AMD). `LazyBake::OnBaked(false)` then marks the field
   `unavailable`, and the programs are never used.
3. **The fallback rows never run here.** Both Macs render every format the
   library asks for (section 3), so the RGBA8 / RG8 rows - the path a GLES 3.0
   device without `EXT_color_buffer_half_float` takes on every buffer - run on
   neither. V32 forced the flare's once, by refusing RGBA16F in a scratch copy
   (progress log, 2026-10-21, "The lens flare is dithered"); nothing in the
   tree could force any of them again, and no `neon-scale-check` mode covers
   them.

## 2. What the check is, and what it is not

**The check:** a 4 x 4 texture in the format, attached as colour attachment 0
of a new framebuffer, and `glCheckFramebufferStatus`. `GL_FRAMEBUFFER_COMPLETE`
means `Resize` will get the format, memory permitting. It is the only test that
works on every GL this library targets:

| alternative | why not |
| ----------- | ------- |
| `glGetInternalformativ(GL_TEXTURE_2D, f, GL_FRAMEBUFFER_RENDERABLE, ...)` | GL 4.3 or `ARB_internalformat_query2`. macOS stops at GL 4.1, and GLES 3.0's `glGetInternalformativ` answers only `GL_SAMPLES` and `GL_NUM_SAMPLE_COUNTS`. |
| the extension strings | A hint, not the answer. Desktop GL 3.3 and GLES 3.2 have float rendering in core with no string to look for - the AMD 5300M lists none of the four while rendering every float format - and the probe makes the same calls the allocation will make, so it cannot disagree with it. The report (4.5) still logs the strings beside the answers. |
| `glTexImage2D` succeeding | GLES 3.0 accepts a half-float TEXTURE with no extension; only rendering to it needs one. A texture that allocates says nothing about an FBO. |

**What it does not say** is whether the format is blendable or
linear-filterable. The table in section 1 says what each buffer needs: two are
drawn blended (the flare's and the spotlight's), five are read through a linear
filter. Every format the library allocates is 8-bit fixed point or 16-bit
float, and for those renderability is the only one of the three a driver can
withhold: half float is texture-filterable in GLES 3.0 core, and both
extensions that make it renderable (`EXT_color_buffer_half_float`,
`EXT_color_buffer_float`) allow blending it; desktop GL 3.3 and GLES 3.2 have
all three in core. 32-bit float is where they part (`EXT_float_blend`,
`OES_texture_float_linear`), and the library allocates no 32-bit target. A
32-bit target added later needs a check of its own (open decision 2). A driver
that breaks the spec here would show in step 6: a blended draw into a target it
cannot blend is `GL_INVALID_OPERATION` on GLES.

## 3. Measured on the AMD 5300M

The probe of section 4.1, standalone, against every format worth knowing about.
"Clear to 2.0 reads" clears the target to 2.0 and reads one texel back as
float: 2.0 is real float storage, 1.0 is fixed point clamping.

| format | renderable | status | clear to 2.0 reads |
| ------ | ---------- | ------ | ------------------ |
| RGBA8, RG8, R8 | yes | COMPLETE | 1.000 |
| RGB10_A2, SRGB8_ALPHA8 | yes | COMPLETE | 1.000 |
| RGBA16F, RG16F, R16F | yes | COMPLETE | 2.000 |
| RGB16F, R11F_G11F_B10F, RGBA32F | yes | COMPLETE | 2.000 |
| RGB9_E5 | **no** | `GL_FRAMEBUFFER_UNSUPPORTED` | - |

So the refusal path works (RGB9_E5), and desktop GL renders everything the
library asks for: this table predicts nothing about a GLES device, which is why
the report is logged at startup there (step 6) rather than measured here. Note
RGB16F: renderable here, but not under `EXT_color_buffer_float` on GLES - one
more reason never to infer a device's answer from a desktop run. The library
uses no 3-channel float format.

**Cost**, wall clock around the call plus a `glFinish`, three runs: the first
probe of a process 0.10-0.15 ms, every later one 0.027-0.033 ms, refused or
not. In the design below each effect probes each of the five formats it uses
once, at startup, for the report - **0.22 ms for the five** (step 2, built) -
and every later question is a lookup. The first design probed about 14 times
(each walk probing its own lists), about 0.5 ms.

## 4. Design

### 4.0 Why one central object: what changed from the first design

The first design (steps 1-3 as built) had every renderer probe its own format
lists in its `Initialize`, kept the refusal hook as process-wide state on
`Framebuffer`, and rejected sharing the answers: it would need a home per GL
context, handed to every renderer, and "at 0.03 ms a probe, the 14 probes cost
less than that plumbing is worth". That judgement was wrong on the plumbing and
missed what sharing buys:

- **The plumbing already exists.** `SharedBufferRegistry` is exactly an object
  the effect owns and hands to each renderer in `AddRenderer`, behind a private
  setter and a protected getter. A second object follows the same path. Every
  tool builds its renderers through `EdgeLightingEffect` (`harness.cpp`,
  `sequence.cpp`, `flare-reuse.cpp`, `clock-check.cpp`, `figures.cpp`), so none
  is left without one.
- **One answer per format, shared.** The report and every allocation read the
  same cached answer: five probes per effect instead of about fourteen, and
  nothing that could disagree.
- **The check sits where the format is used.** A walk asks the object at its
  own allocation, so there is no `Initialize` step to keep beside each walk -
  step 3's pre-checks, `SharedBuffer::SkipUnrenderableFormats` and the neon's
  special case for the glow coverage table's two lists all go.
- **The refusal hook stops being global.** It belongs to one effect, so a test
  cannot leak refusals into another, and `Resize` no longer has to know about
  it. That also makes the hook a better test (4.7).
- **A home for what the plan left open:** the host query (open decision 1) and
  any capability beyond renderability (open decision 2).

What does not change: the probe itself (4.1), the report's output (4.5), every
walk's tier and its full-size fallback, and every frame.

One thing the central object does NOT buy, against a first impression: it does
not leave unused formats unprobed. The report asks all five formats at
startup, used or not, because the device log has to say what the driver can do
before anything is drawn. Laziness only matters for a renderer outside an
effect, and for a format added to a list but not to the report - which is
still probed, correctly, on its first allocation.

### 4.1 The probe: `Framebuffer::IsColorRenderable` (built in step 1)

A static method of `Framebuffer` (`lib/include/gl/framebuffer.h`): the
wrappers are where `glGen*` / `glDelete*` belong, and this is the class whose
`Resize` the probe predicts. In this design it is the raw, uncached check -
`RenderTargetFormats` is its only caller in the library - and it knows nothing
of any refusal hook.

```cpp
/// Whether this driver can render to @p internalFormat: a 4 x 4 texture
/// allocated as Resize allocates one (@p format / @p type are its transfer
/// format), attached to a new framebuffer, and glCheckFramebufferStatus.
/// @p status, if given, receives that status - 0 when glTexImage2D itself
/// refused the format.
static bool IsColorRenderable(GLint internalFormat, GLenum format, GLenum type, GLenum *status = nullptr);
```

The rules its body follows, each for a reason:

- **The same calls as `Resize`**: `glTexImage2D` with the caller's format and
  type, `glFramebufferTexture2D`, `glCheckFramebufferStatus`. Not
  `glTexStorage2D` and not a renderbuffer - a probe that allocates differently
  predicts a different allocation.
- **The draw binding only.** It binds `GL_DRAW_FRAMEBUFFER`, attaches and
  checks through it, and puts the previous draw binding back; the read binding
  is never touched. (`Resize` binds `GL_FRAMEBUFFER`, which moves both - open
  decision 4.)
- **No pixel-unpack buffer during the upload.** With a host buffer bound to
  `GL_PIXEL_UNPACK_BUFFER`, the `nullptr` data pointer is an offset into it,
  and a 4 x 4 read past the end of a small one is `GL_INVALID_OPERATION` - a
  false "no" (measured: step 1). The probe unbinds it for the `glTexImage2D`
  and binds it back. `GL_TEXTURE_BINDING_2D` of the active unit is restored the
  same way.
- **Errors.** It has to read `glGetError` after `glTexImage2D` to tell a
  format the driver does not know at all (status 0) from one it knows but
  cannot render. So an error pending on entry would be misread as its own; it
  drains it first, but logs each one (`LOG_W`, with its code), since it is the
  caller's. It leaves none behind.
- **Logs nothing else.** The object decides what to log.

### 4.2 `RenderTargetFormats`

New, in `lib/include/core/render-target-formats.h` and
`lib/src/core/render-target-formats.cpp` (added to `lib/CMakeLists.txt`):
`core/`, beside `SharedBufferRegistry`, the other object the effect owns and
hands out. GL thread only, like the registry.

As built after the architecture review - the header,
`lib/include/core/render-target-formats.h`, is the source of truth, and
sections 5 and 10 record how the API got here (the first sketch had
`IsRenderable` as the walk's question, no `uploadError`, no unknown answer,
one hook and a tier kept beside every buffer):

```cpp
/// What this effect's GL context can render to. Each format asked about is
/// probed once (Framebuffer::IsColorRenderable) and its answer kept: every
/// later ask is a lookup. It also keeps every full-size allocation that failed,
/// as the smallest size per format. Owned by EdgeLightingEffect - one per
/// effect, so one per GL context - and handed to every renderer it registers
/// (BaseRenderer::GetRenderTargetFormats).
class RenderTargetFormats
{
public:
    /// What the probe says - decided once, in Ask: the one place the rule lives.
    typedef enum class Verdict
    {
        RENDERABLE, ///< The probe's framebuffer was complete.
        REFUSED,    ///< Incomplete, or the upload raised GL_INVALID_ENUM / GL_INVALID_VALUE. Final.
        UNKNOWN,    ///< No status, for a reason that says nothing about the format: the allocation decides.
    } Verdict;

    typedef struct Answer
    {
        GLint internalFormat = 0;
        Verdict verdict = Verdict::UNKNOWN; ///< The only field a decision reads.
        GLenum status = 0;                  ///< For the log.
        GLenum uploadError = GL_NO_ERROR;   ///< For the log.
    } Answer;

    Answer Ask(GLint internalFormat, GLenum format, GLenum type);
    /// The verdict alone: RENDERABLE. What a host reports.
    bool IsRenderable(GLint internalFormat, GLenum format, GLenum type);
    /// Not REFUSED, and no allocation of it that large has failed - what a
    /// buffer asks before it commits (the half-float fields' gates, 4.6).
    bool MayAllocate(GLint internalFormat, GLenum format, GLenum type, int width, int height, int attachments);

    void LogReport();                              ///< The startup report (4.5).
    void AskReportFormats();                       ///< Its asks without the log, for a renderer outside an effect.
    const std::vector<Answer> &GetAnswers() const; ///< Every answer kept, in the order asked.

    /// Verification only - a host never calls these (4.7). Per effect.
    void SetRefusedFormats(std::vector<GLint> formats);    ///< Ask says REFUSED, without a probe.
    bool IsRefused(GLint internalFormat) const;
    void SetFailedAllocations(ForcedFailure failure);   ///< The walk's full-size allocations it selects fail with its error, kept by cause.
    void SetFailedAllocations(std::vector<GLint> formats, int64_t fromTexels = 0); ///< The same, out of memory, from a size.
    void SetMaxTextureSize(int size);                   ///< A lower size limit: larger buffers refused before any row.
    void SetUnknownFormats(std::vector<GLint> formats);    ///< Ask says UNKNOWN, without a probe.

private:
    /// The walk's question, private to it: the first of @p count @p rows, at
    /// or after @p from, not REFUSED and not failed at @p texels or less;
    /// @p count if none. Logs each refused row once.
    template <typename Row>
    size_t firstToTry(const Row *rows, size_t count, size_t from, int64_t texels, const char *owner,
                      const char *what);
    bool hasFailedAt(GLint internalFormat, int64_t texels) const;
    void recordFailedAllocation(GLint internalFormat, int64_t texels); ///< Keeps the smallest size that failed.
    // ... the hooks' lookups, the logged skips
};

/// THE format walk, beside the class, and STATELESS: everything it learns is
/// the object's (4.4). Starts at firstToTry, records and falls back on a
/// full-size allocation that fails, returns false when nothing may be tried or
/// allocates; @p allocated, if given, receives the row that allocated.
template <typename Row>
bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows, size_t count,
                        int width, int height, GLint filter, int attachments, const char *owner, const char *what,
                        const char *consequence, size_t *allocated = nullptr);
```

`GetAnswers().size()` is the number of probes the effect has made, which is how
step 3R and section 6 check that the five report probes are the only ones.

### 4.3 Who owns it, and how a renderer reaches it

- **`EdgeLightingEffect`** holds a `std::shared_ptr<RenderTargetFormats>`,
  made in its constructor - which needs no GL, since the object probes nothing
  until asked - and hands it to each renderer in `AddRenderer`, beside
  `setSharedBufferRegistry` and before the renderer's `Initialize`. It also
  gets a public `const std::shared_ptr<RenderTargetFormats>
  &GetRenderTargetFormats() const`: the tools need it for the hook (4.7), and a
  host that wants the answers reads them there (open decision 1, for C++).
- **`BaseRenderer`** gets a private `setRenderTargetFormats` (the effect is
  already its `friend`, for the registry) and a protected
  `RenderTargetFormats &GetRenderTargetFormats()` that never returns null: a
  renderer no effect has registered makes a private object on its first ask,
  so every call site has one path. That getter is defined out of line, in a new
  `lib/src/renderer/base-renderer.cpp`, because:
- **`base-renderer.h` and `edge-lighting.h` only forward-declare the class.**
  They reach every host, and `render-target-formats.h` brings the GL loader
  with it, which collides with a host's own GL headers - the reason
  `shared-buffer-registry.h` only declares `SharedBuffer`. A
  `std::shared_ptr` member to a declared-only type is fine (its deleter is
  captured where it is made); the renderers and tools that call into the object
  include `core/render-target-formats.h` themselves.
- **One per effect, so one per context.** Two effects in one context probe
  twice, which is harmless. A host that loses its context has to make a new
  effect anyway - every GL object died with the old one - and a new object comes
  with it, so a stale answer cannot outlive its context.

### 4.4 Asked at allocation: the walks

*Built as below in step 3R; the architecture review (section 10) took the tier
out. The walk now keeps nothing: it asks the object for the first row to try
at the size it allocates, and a full-size failure is recorded in the object
with that size, so a format is skipped at that size and above and tried again
below - and, out of memory, at any size once the failure expires, with every
buffer that fell back past it moving back up (the code review at the end of
section 10). What this section says about where each walk asks still holds;
what it says about tiers is the record of the step-3R design.*

Each walk asks at its top, from its current tier, then runs its loop exactly as
now - `Resize`, and the full-size fallback for a refusal the probe did not
predict:

```cpp
tier = formats.FirstRenderable(rows, count, tier, owner, what);
for (; tier < count; ++tier) { /* Resize, fall back - unchanged */ }
```

| walk | where it asks | what step 3R changes from the first design |
| ---- | ------------- | ------------------------------------------ |
| `ResizeInBestFormat` (the neon's emission table, glow coverage table, segment table and gather buffer) | inside the helper, which gains a `RenderTargetFormats &` parameter | `NeonRenderer::Initialize`'s pre-checks go; the emission table's `emissionTier` starts at 0 again, since the helper asks |
| `SharedBuffer::BeginWrite` (the neon's shared buffer) | top of its walk; gains a `RenderTargetFormats &` parameter, passed by the producer | `SkipUnrenderableFormats` goes; `GetInternalFormat` stays |
| the flare's and the spotlight's `resizeScaledBuffer` | top of the walk | the `Initialize` pre-checks go |

**The glow coverage table's shared tier.** One index serves the RGBA list (the
main table with segments) and the RG list (the main table without segments,
and the segment table). Asked at allocation, the tier advances past whatever
the list being allocated cannot render, and since it is shared and only
advances, a refusal on either list moves both - exactly what a full-size
refusal already does to it. The first design took the larger of both lists'
answers up front instead. The difference shows only on a driver that renders
RGBA16F but not RG16F (both extensions cover R, RG and RGBA alike, so it is
unlikely): there the four-channel table stays half float until the first
two-channel allocation, then both drop.

**When nothing in a list renders.** `FirstRenderable` answers `count` and the
walk's loop tries nothing; its failure path puts the tier back on the last row,
as after any failure, so no index reaches past the list. Built first as
`count - 1` - stay on the last row, as the walks' own rule has it - which sent
the walk on to `Resize` a format the object had refused: with the hook no
longer in `Resize`, that allocated it, so case C of section 6 could not make
the neon fail. The object's "no" is final; the walks' full-size fallback is
for the other case, a format it passed. A walk resumed on a refused last row
asks again every frame, so the object remembers the skips it has logged
(`mLoggedSkips`, by the literal names) and logs each once.

**Per frame.** The walks run every frame - `Resize`'s early-out is inside the
loop - so each asks every frame: one lookup at its current tier, a scan of at
most five answers, no GL call and no string built (`owner` and `what` are
literals). At most five walks run in a frame.

**Logging.** A skipped row logs one WARN, `<owner>: <row> <what> target is not
renderable on this driver (status ...), using <next>.`, adding `refused by
RenderTargetFormats::SetRefusedFormats` when the refusal is the hook's. Once
per walk per effect, since the tier never moves back - and not at all for a
walk whose tier is already past the row: with half float refused, step 3R's
run logged six WARNs where the first design logged seven, because the segment
table found the glow coverage table's shared tier already on its 8-bit row. The WARNs now come at
each buffer's first allocation rather than at `Initialize`, so a walk the
config never reaches logs nothing - the report still lists every format at
startup.

### 4.5 The startup report: `RenderTargetFormats::LogReport`

Built in step 2 as `GLUtils::LogRenderTargetFormats`; step 3R moves it into the
object, unchanged in what it prints, still called from
`EdgeLightingEffect::Initialize` right after `LogCaps`. Its five probes are now
the effect's: every walk later finds its answer kept.

```text
--- Render-target formats ---
RGBA16F : renderable
RG16F   : renderable
R16F    : renderable
RGBA8   : renderable
RG8     : renderable
EXT_color_buffer_half_float: no, EXT_color_buffer_float: no, EXT_float_blend: no, OES_texture_float_linear: no (desktop GL: float targets are core)
5 probes, 0.22 ms
-----------------------------
```

One row per format a library buffer asks for (section 1's table). A format
added to any list should get a row here - a comment at each list says so - but
one that does not is still answered correctly, on first ask; it is only missing
from the device log. A refused row prints its status and, for the hook's
refusals, `refused by RenderTargetFormats::SetRefusedFormats`, so a
verification run's log never reads as a device without the format. The
extension line says "desktop GL: float targets are core" when `GL_VERSION`
does not start with "OpenGL ES". `GLUtils::GetFramebufferStatusName` stays in
`gl-utils.h`, which also keeps `LogExtensions` and `LogCaps`.

### 4.6 The fields ask too (step 4)

Both eligibility gates in `NeonRenderer::RenderOffscreen` - the glow field's
and the ring field's - gain one term, `GetRenderTargetFormats().MayAllocate`
(with the field's size since the architecture review) on the field's row (`GetFieldTarget`: R16F, RG16F with segments; built first
as `IsRenderable`, which the third review narrowed to the verdict alone - an
unknown answer leaves the field's allocation to decide, as in every walk),
placed BEFORE `ensureFieldPrograms()` / `ensureRingFieldPrograms()` in their
`&&` chains, so a driver that cannot render the field never compiles its
programs. The first design kept two bools (`mFieldTargets`) probed in
`Initialize`; the object's cache replaces them.

A gate term, not `schedule.unavailable = true`: `unavailable` is permanent, and
the format depends on whether there are segments. A config that gains segments
on a driver that renders R16F and not RG16F shades directly, and composites
again when it loses them. `LazyBake::OnBaked(false)` stays for a refusal the
probe did not predict; the `unavailable` comment ("a program failed, or no
half-float target") changes to say which of the two can still set it.

### 4.7 The refusal hook moves to the object

`RenderTargetFormats::SetRefusedFormats`: per effect, verification only - the
precedent is `NeonRenderer::SetBypass`, a hook with no config field and no C ABI
mirror. A tool reaches it through `effect.GetRenderTargetFormats()` and sets it
before the first allocation it should affect (tiers never move back up, so
before `Initialize` to cover the emission table). Step 3R removes from
`Framebuffer` everything the first design put there for it: `SetRefusedFormats`,
`GetRefusedFormats`, `refusedFormats()`, `isRefusedFormat()` and the refusal at
the top of `Resize`'s allocation path - and `FirstColorRenderable`, which
becomes the object's `FirstRenderable`.

**Why this is a better test, not only a tidier one.** The first design's hook
also failed `Resize`, so a walk that forgot to pre-check still landed on its
8-bit row - through a full-size failure and an ERROR, but with the right
frames. Its step 5 noted the consequence: the mode could not see a missing
pre-check. With the hook on the object alone, a buffer that allocates without
asking GETS the refused format, and section 6's scan of the texture formats
catches it. The hook now tests that every allocation asks.

**What is lost - and won back.** Nothing could force "the probe passed, the
full-size allocation failed" once the hook left `Resize`: that path is the
walks' own fallback, which no driver the project runs on produces. The second
review's fixes added a twin hook for it, `SetFailedAllocations`, inside the one
shared walk (`ResizeInBestFormat`), and `formats` drives it as case D (section
6, open decision 5).

### 4.8 What does not change

On a driver that renders every format - both Macs - every answer is yes, every
tier starts at 0 and the field gate is always true, so every frame is
byte-identical to the build before step 1, and `Initialize` takes the report's
0.22 ms longer. Under the hook, or on a device without half float, every frame
is the one the pre-step-1 fallback path drew: the walks land on the rows they
landed on, one frame sooner and without a failed full-size allocation, and the
fields shade directly as they did, without the three programs. No shader
changes, so nothing here can move a Tizen compile.

## 5. Steps

Each step builds, and passes its verification, on its own. Steps 1-3 are the
first design as built; step 3R turns them into section 4's design.

**Step 1 - the probe and the hook (built, first design).** `framebuffer.h`:
`IsColorRenderable`, `FirstColorRenderable`, `SetRefusedFormats` /
`GetRefusedFormats`, and the refusal at the top of `Resize`'s allocation path.
*Kept by 3R:* `IsColorRenderable`. *Moved by 3R:* `FirstColorRenderable` and
the hook, to `RenderTargetFormats`; `Resize`'s refusal is removed.
*Built 2026-10-27.* Everything builds (Release, tools on). A throwaway program
against the header passed 33 checks on the AMD 5300M:
- section 3's table, row for row (RGB9_E5 `GL_FRAMEBUFFER_UNSUPPORTED`);
- the draw binding, the active unit's texture and the unpack buffer restored,
  and the read binding untouched;
- `RGBA16F` still renderable with a 1-byte unpack buffer bound - the control,
  the same upload made with that buffer bound, raises `GL_INVALID_OPERATION`,
  so the unbind is load-bearing;
- an error pending on entry logged and cleared;
- an unknown internal format reported as status 0 with no error left;
- `FirstColorRenderable` on both row shapes, under no refusal, one and all;
- `Resize` under the hook: refused and empty, RGBA8 unaffected, the early-out
  keeping an allocation made before the hook, and a refused reallocation
  destroying the buffer and dropping its binding to 0, as the real failure
  path does.

`neon-scale-check check` passes unchanged. Not run on the M2.

**Step 2 - the startup report (built, first design).** `gl-utils.h`
(`LogRenderTargetFormats`, `GetFramebufferStatusName`), one call in
`EdgeLightingEffect::Initialize` after `LogCaps`. *Moved by 3R:* the report, to
`RenderTargetFormats::LogReport`; `GetFramebufferStatusName` stays.
*Built 2026-10-27.* Verified on the AMD 5300M through the two entry points the
demos use, from headless programs rather than the demos' windows:
`EdgeLightingEffect::Initialize` (C++) and `el_effect_init_with_renderers`
(the C ABI dylib) print the same table - every row renderable, the four
extensions "no (desktop GL: float targets are core)", **5 probes, 0.22 ms**.
Under the hook, half float refused and then everything refused, each refused
row names the hook, the time drops to 0.08 and 0.01 ms (a refused probe makes
no GL call), and `glGetError` is 0 afterwards. `neon-scale-check check` passes
unchanged. Not run on the M2; the GLES branch of the extension line is
unexercised until step 6.

**Step 3 - the walks' pre-checks in `Initialize` (built, first design;
superseded by 3R).** `NeonRenderer::Initialize` (the emission table on its own
walk, then the glow coverage tier as the larger of its two lists' first
renderable rows, the gather tier and `mScaled->SkipUnrenderableFormats()`),
`LensFlareRenderer::Initialize`, `SpotlightRenderer::Initialize`;
`SharedBuffer::SkipUnrenderableFormats` and `GetInternalFormat`; a pointer to
the report at every format list. *Kept by 3R:* `SharedBuffer::GetInternalFormat`
and the comments (renamed to point at `LogReport`). Everything else goes.
*Built 2026-10-27.* On the AMD 5300M:
- `check`, `partition`, `shared-buffer`, `flare-reuse`, `clock` and `sequence`
  all pass, with no WARN or ERROR line in any of their logs.
- 300 frames at 1280 x 720 from a headless program - neon at 0.5 with a
  partial arc, a segment and the hue rotating, the flare and two spotlight
  lamps at 0.5, and a probe renderer reading the shared buffer - run three
  ways:
  - **Nothing refused:** no WARN, no ERROR; seven RGBA16F and three RG16F
    textures alive; the shared buffer RGBA16F.
  - **Half float refused, both fields bypassed** (`NeonBypass::GLOW_FIELD |
    RING_FIELD`): exactly seven WARN lines at startup - emission, glow
    coverage, glow coverage (two channels), gather,
    `SharedBuffer[NeonRenderer.Scaled]`, the flare, the spotlight - and no
    ERROR; no half-float texture alive in the context (RGBA8 x10, RG8 x1); the
    shared buffer RGBA8 and ready on all 300 frames; `glGetError` 0.
  - **Half float refused, fields as shipped:** the same seven WARNs, plus four
    ERROR lines, two per field (`NeonRenderer.Field` at 640 x 360,
    `NeonRenderer.RingField` at 922 x 120, each refused once and then shading
    directly) - what step 4 removes.
- A first look at what the 8-bit rows cost, for step 5's bound: the last frame
  with half float refused against the same frame without, both with the fields
  bypassed - at most 4 levels, 2.37% of lit channels off by more than 1.

Not run on the M2.

**Step 3R - the central object.** The rework, in one step, since its pieces
only build together:
- new `lib/include/core/render-target-formats.h` and
  `lib/src/core/render-target-formats.cpp` (4.2), in `lib/CMakeLists.txt`;
- `base-renderer.h`: forward declaration, private `setRenderTargetFormats`,
  protected `GetRenderTargetFormats()`; new `lib/src/renderer/base-renderer.cpp`
  for the getter, in `lib/CMakeLists.txt` (4.3);
- `edge-lighting.{h,cpp}`: the member, its hand-over in `AddRenderer`, the
  public getter, and `mRenderTargetFormats->LogReport()` in place of
  `GLUtils::LogRenderTargetFormats()`;
- `framebuffer.h`: remove `FirstColorRenderable`, the hook and `Resize`'s
  refusal (4.7);
- `gl-utils.h`: remove `LogRenderTargetFormats`, and the includes only it
  needed;
- `neon-renderer.cpp`: `ResizeInBestFormat` gains the object and asks (4.4);
  remove `Initialize`'s pre-checks; `emissionTier` starts at 0;
- `shared-buffer.{h,cpp}`: `BeginWrite` gains the object and asks; remove
  `SkipUnrenderableFormats`;
- `lens-flare-renderer.cpp`, `spotlight-renderer.cpp`: ask at the top of
  `resizeScaledBuffer`; remove the `Initialize` pre-checks;
- the comment at every format list names `RenderTargetFormats::LogReport`.

*Verify:*
- Everything builds, and a translation unit that includes the host's own GL
  header (`<OpenGL/gl3.h>`) and then `core/edge-lighting.h` still compiles -
  the forward declarations held.
- On both Macs, the six gates of step 3 pass with no WARN or ERROR line.
- The report prints as in step 2 through the C++ API and the C ABI, and after
  300 frames of step 3's program `GetAnswers().size()` is still 5: the walks
  asked, and found every answer kept.
- Step 3's 300-frame program, run against the object's hook: with the fields
  bypassed, one WARN per walk that meets a refused row (now at each buffer's
  first allocation), no ERROR, no half-float texture alive, the shared buffer RGBA8. With the
  fields as shipped, the scan now FINDS R16F / RG16F textures - the hook no
  longer fails `Resize`, and the fields do not ask until step 4. That is the
  expected state between 3R and 4, and the first demonstration of 4.7: a
  buffer that does not ask is visible.
- Two effects in one process: refusing formats on one leaves the other's
  answers and frames alone.

*Built 2026-10-27.* As listed above: `RenderTargetFormats` (`Ask`,
`IsRenderable`, `FirstRenderable`, `LogReport`, `GetAnswers`, the hook,
`IsRefused`), `BaseRenderer::GetRenderTargetFormats` in the new
`base-renderer.cpp`, the effect's member, hand-over and public getter; the
walks ask in `ResizeInBestFormat` (owner `NeonRenderer`, its existing `what`),
`SharedBuffer::BeginWrite` (owner the buffer's own name, `what` "shared") and
both `resizeScaledBuffer`s (`what` "reduced-scale"); `Framebuffer` is back to
the probe alone and `gl-utils.h` to `GetFramebufferStatusName` alone. On the
AMD 5300M:
- Everything builds with no new warning, and no name from the first design is
  left in `lib/`, `tools/` or the demos.
- A host translation unit including `<OpenGL/gl3.h>` and then
  `core/edge-lighting.h` compiles; the control - including
  `core/render-target-formats.h` itself after the host's header - fails in
  the GL loader, so the check can fail.
- `check`, `partition`, `shared-buffer`, `flare-reuse`, `clock` and
  `sequence` all pass, with no WARN or ERROR line in any of their logs.
- The report prints as in step 2 through the C++ API and the C ABI dylib
  (5 probes, 0.26-0.36 ms this run); a second `LogReport` on the same effect
  reports **0 probes**; with half float refused, each refused row names the
  hook and the report makes 2 probes.
- Step 3's 300-frame program, moved to the object's hook:
  - **Nothing refused:** no WARN, no ERROR, seven RGBA16F and three RG16F
    textures, the shared buffer RGBA16F, **5 probes after 300 frames** - every
    walk found its answer kept.
  - **Half float refused, fields bypassed:** six WARNs (see 4.4: emission,
    glow coverage, gather, `NeonRenderer.Scaled`, the flare, the spotlight),
    no ERROR, no half-float texture alive (RGBA8 x10, RG8 x1), the shared
    buffer RGBA8 and ready on all 300 frames, 2 probes, `glGetError` 0. The
    last frame against nothing refused: max 4 levels, 2.372% of lit channels
    off by more than 1 - the first design's figures exactly.
  - **Half float refused, fields as shipped:** the same six WARNs and **no
    ERROR** - but two RG16F textures alive: the fields allocated a refused
    format, since they do not ask until step 4 and `Resize` no longer refuses.
    The expected state between 3R and 4, and 4.7's point made: a buffer that
    does not ask is visible.
- Two effects in one context, 60 frames each, half float refused on one: its
  shared buffer RGBA8, the other's RGBA16F with all five answers renderable and
  an empty refused list; only the refused effect's five WARNs (neon and flare
  only in that run), no ERROR, `glGetError` 0.

Not run on the M2.

**Step 4 - the field gate.** `neon-renderer.cpp` (the two gate terms of 4.6),
`neon-renderer.h` (the `LazyBake::unavailable` comment).
*Verify:* on both Macs `check` and `sequence` pass and the `cover` set diffs to
zero (`generate --set cover`, then `diff` against the build before). Under the
hook: no R16F / RG16F texture alive after step 3R's program, no ERROR line,
and section 6's field assertions.
*Built 2026-10-27.* One term in each gate of `NeonRenderer::RenderOffscreen` -
a local `fieldTargetRenderable`, asking the effect's `RenderTargetFormats` for
`GetFieldFormat(mEffectiveSegments)` with `GL_HALF_FLOAT` - placed before
`ensureFieldPrograms()` in the glow field's chain and in the ring field's outer
condition, ahead of `ensureRingFieldPrograms()`. The `LazyBake` comments
(`unavailable`, `OnBaked`, who calls what) and the two bake failures' log lines
now say that a bake can fail only on an allocation the probe did not predict.
On the AMD 5300M, step 3R's 300-frame program, extended to count the field
programs' links (`ShaderProgram[NeonRenderer.Field...] linked`) and to compare
the two refused runs byte for byte:
- **Nothing refused:** the three field programs link, as before; 5 probes.
- **Half float refused, fields as shipped:** **no field program linked**, no
  R16F / RG16F texture alive (RGBA8 x10, RG8 x1 - the same as with the fields
  bypassed; step 3R had two RG16F here), no ERROR, the six WARNs, 2 probes,
  `glGetError` 0 - and the last frame **byte-identical** to the fields-bypassed
  run (0 bytes differ), so the gate turns the fields off and nothing else.
- `check` and `sequence` pass, with no WARN or ERROR line in their logs.
- The `cover` set from this build against the same set from step 3R's
  (`generate --set cover`, then `diff`): all 306 frames byte-identical, 0
  pixels differ - as expected, since both Macs render half float and the gate
  is always true there.

Not run on the M2.

**Step 5 - `neon-scale-check formats`.** New `src/format-check.{h,cpp}`, a
`formats` branch in `main.cpp`, the source in `CMakeLists.txt`; the counting
of program links goes in `tools/common/` beside `PassRecorder`, which already
wraps `glad_glDrawArrays` the same way. Section 6 is its specification.
*Verify:* it passes on both Macs, and it FAILS against a build with step 4's
gate term removed (the field programs link, and the field allocates a refused
format) and against one where any walk does not ask - say the gather's - (a
draw lands in a refused format): the two failures 4.7 makes visible.
*Built 2026-10-27.* `tools/neon-scale-check/src/format-check.{h,cpp}`, a
`formats` branch and usage line in `main.cpp`, and in `CMakeLists.txt` the
source when the library has `core/render-target-formats.h` (as `sequence` and
`flare-reuse` are kept from checkouts without their API), plus
`tools/common/draw-target-watch.{h,cpp}`; the README has a `formats` section.
Building it found one defect in step 3R, fixed in the library: with nothing in
a list rendering, `FirstRenderable` returned the last row and the walk
allocated it - see 4.4, "When nothing in a list renders". On the AMD 5300M:
- **PASS in about 10 s**, twice, with identical output but for the report's
  timing line.
- A: probes 5; half-float draws (RGBA16F x1157, R16F x618, RG16F x67); all
  three field programs linked; the shared buffer RGBA16F on all 504 ready
  frames.
- B: probes 2; offscreen draws RGBA8 x1157 and RG8 x10 only, no refused
  texture alive, the shared buffer RGBA8 on all 504 ready frames, no field
  program linked, six WARNs and no ERROR, and all 110 cells byte-identical to
  the fields-bypassed run.
- B against A, worst cells: still 2 levels / 0.021%, hue 2 / 0.108%,
  intensity 3 / 0.458%, `segments` 53 / 8.919%, flare 1 / 0, spotlight
  2 / 0.003% - the bounds are these plus one level, and the share plus a
  quarter of it plus 0.25 points (`FORMAT_QUALITY_BOUNDS`).
- C: probes 0, the neon dropped, no frame drew anything, the shared buffer
  never ready, no GL error.
- It FAILS on both broken builds, each made in a scratch copy of the tree:
  - **Step 4's gate removed** (`fieldTargetRenderable` returning true): 675
    draws into R16F / RG16F, all three field programs linked, 108 cells
    differing from the fields-bypassed run - three checks.
  - **The gather's walk not asking** (`ResizeInBestFormat` skipping
    `FirstRenderable` for "gather"): 378 draws into RGBA16F - and the gather
    in half float moved the intensity frames past their bound (0.841% against
    0.83%) - two checks.
  In both, no refused texture was alive at the end: the last cells (the flare
  and the spotlight, with the neon off) release the buffers that broke the
  rule, so the per-draw watch is the check that catches them.
- After the `FirstRenderable` fix, `check`, `partition`, `shared-buffer`,
  `flare-reuse`, `clock` and `sequence` pass again, with no WARN or ERROR line.

Not run on the M2.

**Step 6 - on the device.** Build for the Tizen target and read the startup
report. Record the table, `GL_RENDERER` and the report's time in the progress
log; that answers the "half-float render targets" part of its open "Measure on
the target" item. If RGBA16F is refused there, the frames the device draws are
the ones section 6's case B checks on the Macs; look at a hue-rotating frame
and a spotlight at scale 0.5 on the device itself for what the bounds cannot
show (banding the dither cannot break, R7 and V27).

**Step 7 - documents.** Section 7.
*Built 2026-10-27.* `CLAUDE.md` (the reading list, a paragraph on
`RenderTargetFormats` beside the shared-buffer one, the field paragraph, the
RAII wrappers, the tools paragraph and the add-a-renderer recipe),
`docs/pages/cpp-design.html` (sections 3, 5, 12, 13 and 17 and the `LazyBake`
figure's caption; Figure 2 names the object as not drawn rather than squeezing
a node and two crossing edges into it), `docs/guides/implementation.md`
(the effect's members and what `Initialize` logs),
`docs/guides/neon-onboarding-guide.md` (completeness, `Initialize` step 2, the
buffer table), I93 in `docs/records/review-findings.md`, the progress log's
2026-10-27 entry, this plan's section 10, and its row in `docs/README.md`. The
tool's README gained its `formats` section in step 5.

## 6. `neon-scale-check formats`

1. Print `GL_RENDERER` and the report (`RenderTargetFormats::LogReport`, the
   same function `Initialize` calls).
2. Run these hook sets, each on a fresh effect, set through
   `effect.GetRenderTargetFormats()->SetRefusedFormats` before `Initialize`,
   over the twelve `check` scenes at scales 1.0, 0.5 and 0.25, still and under
   `--mode hue` and `--mode intensity` (the field paths serve only those), plus
   one flare scene and one spotlight scene at 0.5:
   - **A** - nothing refused: the reference.
   - **B** - `{RGBA16F, RG16F, R16F}`: a GLES 3.0 driver with neither
     extension.
   - **C** - B plus `{RGBA8, RG8}`: nothing renders.
   - **D** - nothing refused, but half float's full-size allocations failed
     (`SetFailedAllocations`, added after the second review): the walks'
     own fallback.
   - **E** - nothing refused, but half float's answers unknown
     (`SetUnknownFormats`, added after the third review): a probe whose
     upload failed for a passing reason.
   - **D2** and **D3** - D from a size (`SetFailedAllocations`' `fromTexels`,
     added after the first review of the stateless walk): half float, then
     every format, fails only from halfway between the viewport at 0.25 and
     at 0.5, on retry cells of their own (the flare and the spotlight at 1.0,
     0.5 and 0.25, the neon on `arcs` at 0.5 and 0.25) beside A and B drawn
     on the same cells. Each viewport-sized scaled buffer fails at 0.5 and
     must allocate again, in half float, at 0.25 - the failure memory held at
     the size that failed and above, never below - and in D3 the flare and
     the spotlight draw their 0.5 cells at 1.0 and say when they return. A
     build whose failure memory ignores size fails both.
   - **D4**, **D5**, **D6** and **S** - added with the failure causes (a
     failure kept by its cause: out of memory by total size, anything else
     by format and attachment count; a buffer past `GL_MAX_TEXTURE_SIZE`
     refused before any row, keeping nothing). D4: RGBA16F incomplete with
     two attachments - no one-attachment buffer may lose half float (a build
     keeping every failure by size alone fails it). D7: the same failure
     under a growing rect, whose gather buffer shrinks every cell - an
     incomplete framebuffer is kept at every size and fails once, out of
     memory is kept by size and is tried again below (a build keeping the
     first by size fails it 7 times, which was this design's first cut). D5: a size limit under
     the viewport at 0.5 (`SetMaxTextureSize`) - refused before tried, then
     allocated at 0.25. D6: the segment coverage table refused, then failing
     - its gate and its fill program. S: the neon, the flare and the
     spotlight outside an effect - probed in `Initialize`, none after, the
     same frames. `tools/neon-scale-check/src/format-check.h` lists every
     check.
3. Assert, exiting non-zero on any failure as the other modes do:
   - **Probes.** `GetAnswers().size()` after every run is the report's five
     less the refused formats (a refusal is the hook's answer, made without a
     probe, and so is a hook's unknown): 5 for A, D, D2 and D3, 2 for B and
     E, 0 for C. Anything more means something probed
     a format outside the report.
   - **B, formats.** No draw ever lands in a refused format: before every
     offscreen draw, the bound draw framebuffer's colour attachments'
     `GL_TEXTURE_INTERNAL_FORMAT` (`glGetTexLevelParameteriv`, desktop GL -
     the tool runs on the Macs) is not in the set; and after the run no
     texture alive in the context has a refused format. With the hook on the
     object alone (4.7), this is what fails when an allocation does not ask.
     And `mScaled`'s `GetInternalFormat()` is `GL_RGBA8`. As built, the draws
     are read by a wrapper of its own in front of `glad_glDrawArrays`
     (`tools/common/draw-target-watch.h`), not by `PassRecorder`, which files
     the flare's and the spotlight's draws as "other" and drops them.
   - **B, fields.** No program link for the three field programs - and A
     links all three, or the check could not fail. As built, the links are
     counted from the library's own `ShaderProgram[NeonRenderer.Field...]
     linked` log lines, which name the programs, rather than by wrapping
     `glad_glLinkProgram`. And frames are byte-identical to the same run with
     `SetBypass(NeonBypass::GLOW_FIELD | NeonBypass::RING_FIELD)` - the field
     is off either way, so any difference is a bug in the gate.
   - **B, quality.** Every frame within a bound of A's, per scale and layer.
     The bound is MEASURED in step 5 and written into the mode, so it is a
     regression gate, not a quality claim: the gather's RGBA8 row read 3/255
     off scale 1.0 on these scenes (`neon-resolution-scale-plan.md` section
     13), the reduced buffer's RGBA8 row brings back the plateaus R7 removed,
     V27 / V32 measured the spotlight's and the flare's fallbacks, and step 3's
     program saw at most 4 levels on one frame. As built (step 5): every scene
     but one within 3 levels and 0.46% of lit channels more than one off; the
     `segments` scene ~53 levels and ~9% at every scale and variant, because
     its boosts of 2.0 are clamped to 1.0 by the emission table's RGBA8 row,
     so it has a group of its own.
   - **B and C, errors.** `glGetError` is `GL_NO_ERROR` after every frame.
   - **C, failure.** `NeonRenderer::Initialize` returns false (the emission
     table has no row left), the effect drops the layer, the neon's shared
     buffer is never ready, and nothing crashes. The flare and the spotlight
     below 1.0 fall back to the 1.0 path, which needs no buffer: every frame of
     theirs byte-identical to A's at 1.0, the fallback logged once per layer
     (since the architecture review - they used to draw nothing).
   - **D, the fallback.** A fallback is logged naming the hook, no draw
     lands in half float and none is alive, the shared buffer is RGBA8,
     and every frame is byte-identical to B's: the full-size fallback
     lands every buffer where B's probe did.
   - **E, unknown.** The report prints the three rows UNKNOWN, no row is
     skipped and none falls back, every half-float target and all three field
     programs are reached, the shared buffer is RGBA16F, and every frame is
     byte-identical to A's: an unknown answer leaves the allocation to
     decide, and here it succeeds.
4. A refusal belongs to the effect it was set on, so a failed assertion cannot
   leak it into a later run: each run makes its own effect.

## 7. Documents to update in step 7

- `CLAUDE.md`: the Architecture section - the effect owns a
  `RenderTargetFormats` beside the `SharedBufferRegistry` and hands it to
  every renderer, the walks ask it at allocation, the hook is per effect and
  verification-only; the RAII wrappers paragraph (`IsColorRenderable`); the
  field paragraph ("a driver that cannot render half float shades directly" is
  now decided before the programs are built); the tools paragraph (the
  `formats` mode).
- `docs/pages/cpp-design.html`: the ownership section (who owns what - a
  second shared object), the `BaseRenderer` contract (the new getter), the GL
  wrappers section, and the field gate where it lists the eligibility terms.
- `docs/guides/implementation.md`: the effect's members and the frame's start.
- `docs/guides/neon-onboarding-guide.md`: wherever it describes a format
  fallback (search "RGBA8" and "fallback").
- `tools/neon-scale-check/README.md`: a `formats` section.
- `docs/records/review-findings.md`: an I-item for the change.
- `docs/records/progress-log.md`: the entry, with step 6's device table.
- This plan: status "built", and a closing section with what was built and
  measured. `docs/README.md`: move its row from "Proposed" to "Built plans".
- Run `python3 tools/check-doc-links.py`.

## 8. Risks

- **The probe passes, the allocation fails.** Memory, or a size limit a 4 x 4
  texture does not reach. The walks keep their full-size fallback for exactly
  this, and log it as an ERROR. `SetFailedAllocations` forces it, and
  `formats`' case D checks it (4.7, open decision 5). Since the architecture
  review the failure is recorded with its size in `RenderTargetFormats`, and
  `Resize` fails on the allocation's own GL error rather than on completeness
  alone. A driver that allocates storage lazily, at first use, reports
  neither: no check at allocation time can see that.
- **COMPLETE, but wrong.** A driver could accept the format and clamp or drop
  what is written. The probe cannot see that; section 3's read-back could
  (open decision 3).
- **A header that leaks the GL loader.** If `base-renderer.h` or
  `edge-lighting.h` ever includes `render-target-formats.h` instead of
  declaring the class, every host that includes its own GL headers breaks.
  Step 3R's verification compiles exactly that case.
- **A host that calls the hook.** The getter is public, so a host can call
  `SetRefusedFormats` and get 8-bit buffers. It is documented as verification
  only, as `SetBypass` is; nothing stops a host from calling either.
- **A walk that forgets to ask.** With the hook no longer in `Resize`, such a
  walk works on a device that renders everything and allocates a refused
  format under the hook. Section 6 fails it; nothing else would.
- **Startup time.** The report's five probes: 0.22 ms on the AMD; the device's
  figure is printed by the report itself.

## 9. Open decisions

1. **A host-visible query.** Answered for C++: `GetRenderTargetFormats()` and
   `GetAnswers()`. A C ABI mirror (the answers, or which row each layer landed
   on) is still open; decide when a host asks.
2. **A blend check.** It needs a program and a draw, since a clear does not
   blend. If built, it belongs in `RenderTargetFormats::Answer`. Worth it only
   if a 32-bit float target is added, or if step 6 finds a driver that renders
   half float but will not blend it.
3. **A read-back check** (clear to 2.0, read back). It catches a driver that
   reports COMPLETE and clamps. On GLES the float read-back may not be allowed
   (`GL_IMPLEMENTATION_COLOR_READ_FORMAT` / `_TYPE` decide), so it would be a
   best-effort line in the report, not a gate.
4. **The read binding.** The library binds `GL_FRAMEBUFFER` everywhere
   (`Resize`, `Bind`, `BindId`), which moves the read binding along with the
   draw one, so a host with distinct read and draw bindings gets its read
   binding replaced. The probe touches the draw binding alone; the rest is a
   separate change.
5. **Forcing a full-size failure.** Decided after the second review:
   `RenderTargetFormats::SetFailedAllocations`, inside the shared walk rather
   than on `Framebuffer`, and `formats`' case D, which must land every buffer
   where case B's probe did.

## 10. What was built and measured

Built 2026-10-27 on top of `dce6d45`, not committed; every step but 6. On an
AMD Radeon Pro 5300M (macOS GL 4.1), Release; **not run on the M2**.

**What is in the tree.**

| where | what |
| ----- | ---- |
| `lib/include/gl/framebuffer.h` | `Framebuffer::IsColorRenderable`, the probe (4.1) |
| `lib/include/core/render-target-formats.h`, `lib/src/core/render-target-formats.cpp` | `RenderTargetFormats`: `Ask`, `IsRenderable`, `MayAllocate`, `FirstRenderable`, `LogReport`, `GetAnswers`, the three verification hooks (4.2); `ResizeInBestFormat`, the one walk (4.4) |
| `lib/include/renderer/base-renderer.h`, `lib/src/renderer/base-renderer.cpp` | the private setter and the never-null protected getter (4.3) |
| `lib/include/core/edge-lighting.h`, `lib/src/core/edge-lighting.cpp` | the effect's object, its hand-over in `AddRenderer`, the public getter, `LogReport` in `Initialize` |
| `lib/include/util/gl-utils.h` | `GetFramebufferStatusName` |
| `lib/include/core/shared-buffer.h`, `lib/src/core/shared-buffer.cpp` | `BeginWrite` asks; `GetInternalFormat` |
| `lib/src/renderer/neon-renderer.cpp`, `lens-flare-renderer.cpp`, `spotlight-renderer.cpp` | every walk asks at its allocation (4.4); the fields' gate (4.6) |
| `tools/neon-scale-check/src/format-check.{h,cpp}`, `tools/common/draw-target-watch.{h,cpp}` | the `formats` mode (section 6) |

**Measured.** A probe 0.027-0.033 ms (the first of a process 0.10-0.15); the
startup report 0.22-0.36 ms for its five probes, and every later question a
lookup - `GetAnswers()` is still 5 after 300 frames of every layer. Frames on
a driver that renders everything: unchanged - the `cover` set byte-identical
across the field gate, `check` passing after every step, and all six gates
after steps 3, 3R and 5 and the review fixes (steps 1 and 2 ran `check` alone,
step 4 `check`, `sequence` and the `cover` diff). What the 8-bit
rows cost, half float refused against nothing refused (worst cell, max levels
/ share of lit channels more than one off):

| | 1.0 | 0.5 | 0.25 |
| --- | --- | --- | --- |
| still | 2 / 0.021% | 2 / 0.003% | 2 / 0.001% |
| hue rotating | 2 / 0.092% | 2 / 0.108% | 2 / 0.006% |
| intensity moving | 3 / 0.458% | 2 / 0.303% | 2 / 0.338% |
| `segments` (every variant) | 53 / 8.919% | 53 / 8.737% | 53 / 8.774% |

The flare at 0.5: 1 / 0; the spotlight at 0.5: 2 / 0.003%. `segments` is the
emission table's RGBA8 row clamping boosts of 2.0 to 1.0 - the cost a GLES 3.0
device without half-float targets pays on any config whose arcs or stacked
segments run above 1.0.

**Where the build departed from the plan as first written.**

- The design itself: the first version (each renderer probing in
  `Initialize`, a process-wide hook in `Framebuffer`) was built and replaced
  the same day by the central object - section 4.0.
- `FirstRenderable` answers the list's size, not its last row, when nothing
  renders, and logs each skip once: found by building case C (4.4).
- The tool counts field-program links from the library's log lines, and reads
  draw targets with a watcher of its own, since `PassRecorder` drops the
  flare's and the spotlight's draws (section 6).
- The `segments` scene has a quality bound of its own; one bound per group
  would have let every other scene drift by 50 levels.
- Six WARN lines, not seven, on a driver without half float: the glow
  coverage table's shared tier is already past RG16F when the segment table
  first allocates.

**After a deep review, the same day:** thirteen findings, twelve fixed.

- A probe with no status (a failed upload, a status query that raised an
  error) is no longer kept. It was cached as a permanent "no", and since a
  walk's "no" is final, a passing GL_OUT_OF_MEMORY on a 4 x 4 texture could
  disable a buffer for the effect's life - and a caller's wrong transfer pair
  could condemn a format that renders. `Answer::uploadError` records what the
  upload raised, and the logs say "no status" rather than "texture upload
  refused".
- A renderer outside an effect asks the report's formats into its own object
  when it makes it (`AskReportFormats`), and the flare and the spotlight make
  it in `Initialize`, so no probe lands mid-frame, where it cleared the frame's
  pending GL errors.
- The four copied walks are one `ResizeInBestFormat` in
  `core/render-target-formats.h`, which owns the "nothing renders, stay on the
  last row" reset that each copy had to remember.
- Skipped rows are remembered by text, not pointer; the effect's const getter
  hands the object out const.
- In the tool: one shared-buffer probe for three modes
  (`shared-buffer-probe.h`); the draw watcher reads each attachment's format
  from its own parameters (renderbuffers and any texture target, nothing
  bound); the quality bounds padded by two levels and half again, for the M2.
- Not changed: the report still probes at every `Initialize`. Its point is the
  device log at startup; 0.22 ms on the AMD, and step 6 measures the device.

Verified after the fixes, on the AMD 5300M: `check`, `partition`,
`shared-buffer`, `flare-reuse`, `clock`, `sequence` and `formats` all pass with
no WARN or ERROR line. `formats` reads the same counts as before the watcher
was rewritten (A: RGBA16F x1157, R16F x618, RG16F x67; B: RGBA8 x1157, RG8 x10)
and the same worst cells to the digit, so the shared walk moved no frame. A
throwaway program checked what the gates cannot see: a first ask with a pair
the GL rejects (`GL_RGBA16F` with `GL_RGBA_INTEGER`) records the upload's
error and keeps nothing, and the next, correct ask keeps RGBA16F renderable; a
flare outside any effect holds all five answers after `Initialize`; one walk's
skip, named from two addresses, is logged once; a failed upload is logged as
"no status" with its error; and a const effect's
`GetRenderTargetFormats()->SetRefusedFormats` does not compile. The broken
builds of step 5 were not rebuilt against the rewritten watcher; it classifies
every draw as the old one did.

**After a second deep review, the same day:** seven findings, all fixed.

- A probe with no status is kept as UNKNOWN (`Answer::IsUnknown`) rather
  than dropped: the first fix still let `FirstRenderable` skip such a row,
  and a walk's tier only advances, so a passing failure on a row before the
  last moved that buffer past the format for good, while an unknown last
  row was re-probed every frame, mid-frame. Now an unknown row is tried - the
  full-size allocation decides, as before any probe existed - and nothing is
  probed twice. `IsRenderable` is true for it; the report prints it as
  UNKNOWN.
- `FirstRenderable` is private, the walk its friend; `GetRefusedFormats`,
  which nothing called, is gone; the comments and the skip warning no longer
  promise a second ask.
- `SetFailedAllocations` drives the walk's full-size fallback, and the two
  half-float fields allocate through the walk too (lists of one), so
  `formats`' case D covers every buffer.

Verified after these fixes, on the AMD 5300M: the six gates and `formats` all
pass with no WARN or ERROR line. Case D: 5 probes, six fallbacks logged naming
the hook (the emission table, the glow coverage and segment tables, the gather
buffer, the shared buffer, the flare and the spotlight walk once each - the
coverage table's tier is shared), two field bakes failing (their programs build,
as on a driver whose allocation fails), no half-float draw or texture, the
shared buffer RGBA8 on all 504 ready frames, and all 110 cells byte-identical
to case B. A throwaway program checked the rest: a first ask with a pair the
GL rejects is kept as unknown, `IsRenderable` says a buffer may try it, nothing
probes it again, and a walk over it allocates RGBA16F at tier 0; with RGBA16F's
allocation failed, a walk falls back to RGBA8 once, logs it once naming the
hook, and resumes there silently; two walks naming the same skip from two
addresses log it once; and calling `FirstRenderable` from outside the class
does not compile.

**After a third deep review, the same day:** four findings, all fixed.

- An upload error is read, not only recorded. One naming an argument the GL
  does not accept at all - `GL_INVALID_ENUM` or `GL_INVALID_VALUE`, which for
  a 4 x 4 upload of a library row means the internal format - is a definitive
  "no"; out of memory, `GL_INVALID_OPERATION` (a transfer pair the GL rejects)
  and a status query that raised an error stay unknown. The spec names
  `GL_INVALID_VALUE` for an unknown internal format, and the first build of
  this fix took it alone: a throwaway program then found Apple's GL raising
  `GL_INVALID_ENUM` for one (`GL_TEXTURE_2D` passed as the format), which
  stayed unknown and was tried at full size.
- `IsRenderable` is the verdict alone, false for an unknown answer;
  `MayAllocate` - renders or unknown - is what a buffer asks before it commits
  to a format, and the half-float fields' gate asks it. `IsRenderable` used to
  mean "may try", which a host reading it as "supported" would misreport.
- A third hook, `SetUnknownFormats`, and `formats`' case E: half float
  unknown must draw exactly what nothing refused draws.
- Section 4.2's sketch is the API as built.

Verified, on the AMD 5300M: the six gates and `formats` pass with no WARN or
ERROR line. Case E: 2 probes, the report's three half-float rows UNKNOWN and
naming the hook, no skip or fallback, every half-float target reached as in
A (RGBA16F x1157, R16F x618, RG16F x67) and all three field programs built,
the shared buffer RGBA16F on all 504 ready frames, and all 110 cells
byte-identical to A. It can fail: built with the fields' gate asking
`IsRenderable`, E fails (no field program, 108 cells differ from A); built
with the walk skipping unknown rows, it fails four ways (8 skips and
fallbacks, no half-float target, the shared buffer RGBA8, all 110 cells
differ); every other case passes on both. A throwaway program checked the
rest, 18 checks: an unknown internal format raises `GL_INVALID_ENUM` here, is
kept as a "no", is never probed again and is skipped by a walk without a try
or a GL error left; `GL_RGBA16F` with `GL_RGBA_INTEGER` raises
`GL_INVALID_OPERATION` and is unknown - `IsRenderable` false, `MayAllocate`
true; `SetUnknownFormats` makes no probe, a walk over it allocates RGBA16F, a
refusal wins over it, and the report names it.

**After an architecture review, the same day:** six findings; the first four
fixed, the last two left open.

1. **The walk is stateless.** Since the probe, a walk's tier only remembered
   "this format passed the probe and failed at full size" - and kept that
   forever, at every size, so one failed 4K allocation sent the shared buffer
   to RGBA8 for the effect's life. `RenderTargetFormats` now records every
   full-size failure as the smallest size (texels x attachments) that failed
   per format; a walk skips a format at that size or larger and tries it again
   below. Every tier member is gone (`mGlowCover.format`, `mGather.format`,
   `SharedBuffer::mTier`; the flare's and the spotlight's `mScaledFormat` now
   only say which row the buffer holds, through the walk's optional
   `allocated` out parameter), and so is the coverage table's shared tier. The
   half-float fields follow the same rule: their gates ask `MayAllocate` with
   the field's size, and a failed bake no longer sets `LazyBake::unavailable`
   (left for a program that fails to build). A row skipped because another
   walk's allocation of it failed this large is logged once per buffer, as a
   refused row is.
2. **`Resize` reads the allocation's error.** It judged failure by
   completeness alone - which the spec leaves undefined after
   `GL_OUT_OF_MEMORY` - and left the error pending: built against the old
   header, a texture one texel past `GL_MAX_TEXTURE_SIZE` returned false and
   left `GL_INVALID_VALUE` for the host's next `glGetError`. It now drains the
   errors pending on entry (logged as the caller's), unbinds the pixel-unpack
   buffer for its upload as the probe does, and fails on any error the
   allocation raises - on the allocation path only, never on its early-out.
3. **An explicit verdict.** `Answer::verdict` (RENDERABLE, REFUSED, UNKNOWN) is
   decided once, in `Ask` (`ClassifyProbe`); `renderable` and `IsUnknown()`
   are gone, and `IsRenderable`, `MayAllocate`, the walk and the report all
   read the verdict. The walk's question is `firstToTry` (private, camelCase,
   as the conventions want).
4. **The flare and the spotlight fall back to 1.0.** Below 1.0, no scaled
   buffer in any format used to mean the layer drew nothing; it now draws the
   1.0 path, which needs no buffer, and logs the switch once each way.
   `formats` case C checks it: the 0.5 and 1.0 cells of both layers (two new
   1.0 cells, 112 a run) byte-identical to A's 1.0 frames.

Open: (5) the report's format list is a hand-kept copy of seven lists in
three renderers, declared through three near-identical row types - a format
missed there probes mid-frame, which `formats` catches by its probe count;
(6) the three verification hooks could be one per-format override.

Verified, on the AMD 5300M: the six gates and `formats` pass with no WARN or
ERROR line; `formats` reads the same draws per format in every run and the
same B-against-A worst cells to the digit as the round before, plus the two
new 1.0 rows at 0 / 0. Case C: all four flare and
spotlight cells byte-identical to A's at 1.0, the fallback logged once per
layer. Case D: 5 probes; one RGBA16F failure (the emission table, 128 x 2, at
`Initialize`) recorded, so every larger RGBA16F buffer skips it - five skip
lines, one per buffer, rather than five more failed allocations; the edge
ring's field tried at three successively smaller sizes as the cells shrank,
where it used to give up after the first; all 112 cells byte-identical to B. A
throwaway program checked the rest, 27 checks: the verdicts and the hooks'
precedence; a failure at 512 x 512 lands on RGBA8 with one fallback line, the
same size again reallocates nothing and logs nothing, `MayAllocate` says no at
and above the failed size and yes below, a smaller size gets RGBA16F back once
the memory returns and a larger one still skips it, and a second buffer learns
from the first; `Resize` past the size limit fails and leaves no GL error, and
a walk there fails on every row and still gets RGBA16F at a normal size after;
a 1-byte host unpack buffer no longer fails `Resize` and is bound again after;
an error pending on entry is logged as the caller's and cleared.

**Open.** Step 6, the device; the M2; open decisions 1-4 of section 9; the
architecture review's findings 5 and 6.

**After a code review, 2026-10-28: out of memory expires.** The stateless walk
fixed "failed at 4K, so a smaller buffer is refused too", but an out-of-memory
failure still held at its size and above for the effect's life - and, kept per
format by total size, against every walk of that format, not only the one that
failed. `formats` case D7 showed it: a forced failure of the two-attachment
gather (~61k texels) put the one-attachment shared buffer (230k) on RGBA8 for
all 24 frames. On a TV, where a video decoder can take memory for a moment,
one passing shortage would leave the shared buffer at 8 bits - and the contour
rings R7's dither removes back - for good. Memory comes back; a refusal or an
incomplete framebuffer does not, so only out of memory changed:

- **Expiry, with backoff.** `RenderTargetFormats` keeps a frame time
  (`Advance`, fed by `EdgeLightingEffect::Update` with the host's frame delta -
  not the effect's `Clock`, which a host may pause - and, for a renderer
  outside an effect, by its own `Update` through
  `BaseRenderer::AdvanceOwnRenderTargetFormats`). A failure expires
  `OUT_OF_MEMORY_RETRY_SECONDS` (2 s) after it; then the first walk past it
  tries the format at any size. A retry that runs out again re-arms it at its
  own size for twice the wait, up to `OUT_OF_MEMORY_RETRY_MAX_SECONDS`
  (512 s): a shortage that never passes costs one failed allocation every
  ~8.5 minutes in the end. A smaller failure while it is armed restarts the
  wait without doubling it - one shortage, not a retry. An allocation as large
  as the failure that succeeds clears it, logged once, and with it every
  walk's logged skip of that row, so the next shortage is logged again. Frame
  time rather than wall time so a run is reproducible; a host that never calls
  `Update` never lets a failure expire.
- **Buffers move back up.** Expiry is no use to a buffer whose caller skips
  the walk while it holds. `IsBelowBestRow` (free while a buffer holds its
  first row) says whether a walk at its size would now land higher, and every
  such caller asks it: the emission table, which is never resized and is now
  re-walked by `ensureEmissionBuffer` (marked unbaked when it moves); the glow
  coverage and segment tables' hysteresis; the gather cache; the neon's reused
  offscreen frame (`SharedBuffer::IsBelowBestFormat`); the flare's cache. The
  spotlight and the fields walk on every frame they draw.
- **Verification.** `ForcedFailure::count` fails only the first N allocations
  it selects - a shortage that passes - and `SetOutOfMemoryRetry` sets the
  first wait (infinity keeps a failure for the run, which cases D and D6's
  failing run now ask for: D's moving cells pass ~7 s of frame time, and a
  retry re-runs a gather B reuses, a level off in places). Case D8: with only
  the first RGBA16F allocation failing - the emission table's, in `Initialize`
  - the shared buffer, the gather and the flare's and spotlight's buffers draw
  in RGBA8, a cell passing 3 s of frame time lets the failure expire, and the
  neon, flare and spotlight cells drawn after it are A's byte for byte; with
  every RGBA16F allocation failing over 40 one-second frames, the format is
  tried exactly 5 times (0, 2, 6, 14, 30 s) and ends waiting 32 s. With
  `IsBelowBestRow` forced false the passing run's neon cell differs from A's
  and the case fails.

**After an architecture review of that change, the same day.** Two of its
sites had no gate: disabling the coverage tables' `IsBelowBestRow` checks left
`formats` passing, since D8 failed only RGBA16F and its `arcs` coverage table
is RG16F. D8's passing shortage now runs six ways - RGBA16F on `arcs`,
`segments`, the flare and the spotlight (the four-channel coverage table too);
RG16F on `arcs` (the two-channel coverage table); RG16F on `segments` (the
segment table); RGBA16F from the viewport's size on `arcs` (the shared buffer
alone) and on the flare alone; RGBA16F at two attachments on `segments` (the
gather alone) - and each must leave the cells drawn after the expiry A's, byte
for byte, having drawn in the 8-bit row meanwhile. The last three exist because
disabling each site in turn showed the first way hid three of them: the
emission table's move re-runs pass 1b and the gather anyway (its version is in
the gather's key), and its neon cells switch the flare off, which gives its
buffer back. The RG16F-on-`arcs`
way found a defect the first two ways could not: the edge ring's field, baked
from the 8-bit table, was still composited after the table moved up and was
re-baked, because a field is invalidated by config changes and nothing else,
and a buffer moving rows is not one. A field now records the formats its bake
read its inputs in (`FieldInputFormats`: the emission, coverage and segment
tables and the gather buffer) and is composited only while they hold
(`LazyBake::DropIfInputsMoved`, checked once the gather has run, so a gather
that moved rows in this very frame counts); otherwise the frame shades
directly and the next one bakes. That covers a buffer moving down, too.
Disabling any one of the seven checks - the emission table's, the gather
cache's, the reused frame's, the flare cache's, the two coverage tables' and
the fields' - fails `formats`. The review's other findings - each buffer's format policy
spread across five places (one type owning a buffer and its list would close
earlier item 5 too), the five verification setters standing in for a seam,
the gate's reliance on log wording - are open, and the host-visible changes
are now section 8 of [`upgrade-notes.md`](../guides/upgrade-notes.md).

Also from the code review: `ResizeInBestFormat` now releases the buffer on an
invalid size, as its return promises; and `formats` scans the live textures up
to the highest name `glGenTextures` returned (`DrawTargetWatch`) rather than a
fixed 16384, and counts an error the scan raises.

Verified on the Apple M2 Pro: `formats` (every case's output unchanged but the
new D8), `check`, `shared-buffer`, `partition`, `clock`, `flare-reuse` and
`sequence` pass. Not run on the AMD 5300M or the device. Still open from that
review: a failure other than out of memory is kept at every size, for good,
which is right only if the driver reports a shortage as `GL_OUT_OF_MEMORY` -
read step 6's log for "target unavailable ... (GL_FRAMEBUFFER_...)".

**After a second code review, 2026-10-29: three defects in the expiry.** Each
was confirmed by a probe against `20b9e4e` before it was fixed.

- **A failed retry left the shared buffer undefined outside the glow's
  window.** `SharedBuffer::BeginWrite` clears only this write's window and the
  last write's, and the whole buffer only when its texels are undefined - which
  it told by a change of size or format. A retry that runs out of memory again
  destroys the buffer (`Resize` frees the old allocation first), and the walk
  then allocates the format it had at the size it had: a fresh texture that
  looks like the old one, and GL handed back the very same name. So the clear
  stayed windowed, every other texel stayed whatever the driver gave, and every
  later write kept them, clearing windows only. The shared buffer is the
  largest buffer - the one a lasting shortage hits and whose retries keep
  failing - and any reader of it saw those texels, the neon's own frame
  included (reverted, the check below finds 86,400 of the buffer's 230,400
  texels never written on `small_rect` at 0.5, and the frame differing from a
  run that never retried). `Framebuffer::GetAllocationCount` now moves on every
  allocation, and `BeginWrite` clears whole whenever it moved.
- **A reused still frame never asked about the gather.** `reuseOffscreen`
  skips passes 1a and 1b but asked only whether the shared buffer was below its
  best format, so a gather that fell alone stayed in RGBA8 until the config
  changed. Normally the gather's failure reaches the shared buffer - the larger
  allocation, so blocked too - whose move re-runs the gather; that hid it, and
  hid it from D8, whose gather way at 0.5 passed only because the shared buffer
  fell with it. At a scale at or below `GATHER_MIN_SCALE` the gather runs at
  the reduced buffer's own scale, and on a rect near the viewport its region -
  rounded up to whole `REGION_ALLOC_STEP`s, with a footprint, two attachments
  with segments - outgrows the shared buffer (96 x 64 x 2 texels against
  80 x 45 at 0.0625 and 720p). The reused frame now asks about the gather too.
- **A dropped field waited two frames to bake again.** A field used straight
  after its first-frame bake was never marked `settled`, so after
  `DropIfInputsMoved` the next frame took the "config just changed" row and
  shaded directly again. The drop now marks it settled - the config held.

The gate could see none of the three. A desktop GL hands back zeros for a
texture allocated with no data, so the first was invisible on both Macs; the
second needed a buffer to fall alone; the third cost one frame, and D8 compared
only the cells drawn after the waiting one. Now `DrawTargetWatch` fills every
texture allocated with no data with 0x3C in every byte - every `formats` case
passes with it, so no buffer the gate reaches reads a texel it never wrote -
D8's gather way runs at scale 1/16 and requires the shared buffer to stay
RGBA16F, the waiting cell's last frame (the one after the move) must be A's
too, and a new check, on `small_rect` - a 640 x 360 rect's glow covers the
whole buffer, so every write clears it all - fails the shared buffer's retry
again and compares its frame and every texel, read back, with a run whose
failure is held for the run.

Verified on the Apple M2 Pro: with the fill, every earlier `formats` case
passes unchanged; each fix reverted alone fails `formats` - the whole clear
(86,400 of the 230,400 texels never written, the frame different; with the
fill off too, the check passes), the reused frame's and the gather cache's
gather checks (both caught by the 1/16 way), the drop marking the schedule
settled - and `formats`, `check`, `shared-buffer`, `partition`, `clock`,
`flare-reuse` and `sequence` pass. Not run on the AMD 5300M or the device.
Left as a trade-off rather than changed: a retry destroys the buffer that fell
back before it tries the better format (`Resize` frees the old allocation
first), which gives the retry the most memory, but if the fallback's own
allocation then fails too, the layer loses its buffer until the next wait
ends.

**After a third review, the same day: the gates that read the shared buffer.**
No library defect; `formats` passes under ASan and UBSan. But the failed-retry
check proved something only where the glow's window leaves texels outside it,
and nothing checked that; and `sequence` and `shared-buffer` compared
shared-buffer texels without the fill, so a texel nobody wrote read 0 in every
run alike. `SharedBuffer::GetLastWriteWindow` now says where the last write
drew; the failed-retry check and every `shared-buffer` cell require each texel
outside it to be 0, and some to lie outside it; and both modes install the
fill (`DrawTargetWatch::InstallFreshTextureFill`). With the whole clear after
a reallocation removed, `shared-buffer` fails nearly every cell and
`sequence` 338 frames of 6 sequences - each only with the fill. One measured
correction to the reasoning above: the blit reads two texels past the window
on `small_rect` at 0.5, so an uncleared texel does show in the frame there,
at the window's edge.
