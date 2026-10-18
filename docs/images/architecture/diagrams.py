"""Draws the diagrams in docs/guides/architecture.md as SVG files beside this script.

    python3 docs/images/architecture/diagrams.py

Plain Python, no dependencies. Every diagram is a function below that places
boxes, arrows and text by hand; the styles at the top are shared, so the
diagrams agree with each other. Each SVG carries its own light background, so
it reads the same on a light or a dark page.
"""

import os

OUT = os.path.dirname(os.path.abspath(__file__))

FONT = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "SFMono-Regular, Menlo, Consolas, monospace"

# fill, stroke
STYLES = {
    "host": ("#f6f8fa", "#8c959f"),
    "effect": ("#ddf4ff", "#0969da"),
    "renderer": ("#dafbe1", "#1a7f37"),
    "context": ("#fbefff", "#8250df"),
    "gl": ("#eaeef2", "#6e7781"),
    "phase1": ("#fff8c5", "#9a6700"),
    "phase2": ("#ddf4ff", "#0969da"),
    "warn": ("#ffebe9", "#cf222e"),
    "public": ("#dafbe1", "#1a7f37"),
    "debug": ("#f6f8fa", "#8c959f"),
    "plain": ("#ffffff", "#8c959f"),
}

TEXT = "#1f2328"
MUTED = "#57606a"
LINE = "#57606a"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, width, height, title):
        self.w = width
        self.h = height
        self.title = title
        self.parts = []

    def rect(self, x, y, w, h, style="plain", radius=8, dashed=False, stroke_width=1.5):
        fill, stroke = STYLES[style]
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{stroke_width}"{dash}/>'
        )

    def text(self, x, y, s, size=13, weight="normal", color=TEXT, anchor="start", mono=False, italic=False):
        family = MONO if mono else FONT
        style = ' font-style="italic"' if italic else ""
        self.parts.append(
            f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" '
            f'fill="{color}" text-anchor="{anchor}"{style}>{esc(s)}</text>'
        )

    def box(self, x, y, w, h, title, lines=(), style="plain", center=False, dashed=False, title_mono=False):
        """A rounded box with a bold title and lines under it. A line that
        starts with '`' is set in monospace (the backtick dropped); one that
        starts with '~' is muted."""
        self.rect(x, y, w, h, style, dashed=dashed)
        anchor = "middle" if center else "start"
        tx = x + w / 2 if center else x + 12
        ty = y + 22
        if not title:
            # No title: the lines sit in the middle of the box.
            ty = y + h / 2 + 4.5 - 9 * (len(lines) - 1)
        if title:
            self.text(tx, ty, title, size=14, weight="bold", anchor=anchor, mono=title_mono)
            ty += 20
        for line in lines:
            mono = line.startswith("`")
            muted = line.startswith("~")
            content = line[1:] if (mono or muted) else line
            self.text(tx, ty, content, size=12.5, anchor=anchor, mono=mono, color=MUTED if muted else TEXT)
            ty += 18

    def arrow(self, points, label=None, dashed=False, color=LINE, label_at=0.5, label_dx=6, label_dy=-6,
              both=False, label_anchor="start", width=1.6):
        d = "M " + " L ".join(f"{px} {py}" for px, py in points)
        dash = ' stroke-dasharray="6 4"' if dashed else ""
        marker_id = "arrow-red" if color != LINE else "arrow"
        start = f' marker-start="url(#{marker_id})"' if both else ""
        self.parts.append(
            f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}"{dash} '
            f'marker-end="url(#{marker_id})"{start}/>'
        )
        if label:
            # On the segment holding the label_at share of the path's length.
            lengths = []
            for (ax, ay), (bx, by) in zip(points, points[1:]):
                lengths.append(((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5)
            target = sum(lengths) * label_at
            acc = 0.0
            for i, seg in enumerate(lengths):
                if acc + seg >= target or i == len(lengths) - 1:
                    t = (target - acc) / seg if seg else 0.0
                    (ax, ay), (bx, by) = points[i], points[i + 1]
                    lx, ly = ax + (bx - ax) * t, ay + (by - ay) * t
                    break
                acc += seg
            for k, part in enumerate(label.split("\n")):
                self.text(lx + label_dx, ly + label_dy + 16 * k, part, size=12, color=MUTED, anchor=label_anchor)

    def note(self, x, y, s, size=12, anchor="start", color=MUTED, italic=True):
        for k, part in enumerate(s.split("\n")):
            self.text(x, y + 16 * k, part, size=size, color=color, anchor=anchor, italic=italic)

    def write(self, name):
        head = (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}" role="img" aria-label="{esc(self.title)}">\n'
            f"<title>{esc(self.title)}</title>\n"
            "<defs>\n"
            f'<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" '
            f'orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="{LINE}"/></marker>\n'
            '<marker id="arrow-red" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" '
            'orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#cf222e"/></marker>\n'
            "</defs>\n"
            f'<rect x="0.5" y="0.5" width="{self.w - 1}" height="{self.h - 1}" rx="12" fill="#ffffff" '
            f'stroke="#d0d7de"/>\n'
        )
        body = "\n".join(self.parts)
        with open(os.path.join(OUT, name), "w") as f:
            f.write(head + body + "\n</svg>\n")


# -----------------------------------------------------------------------------
# 1. The system at a glance
# -----------------------------------------------------------------------------
def overview():
    s = Svg(900, 672, "The system at a glance")
    s.box(60, 30, 300, 66, "C++ host", ["demo/, an app, the tools"], "host")
    s.box(540, 30, 300, 66, "C, C#, Python ... host", ["demo-capi/, FFI, P-Invoke"], "host")
    s.box(540, 128, 300, 66, "C ABI | lib/capi/", ["libedge-lighting-c: el_* calls,", "a staging Config per handle"], "gl")
    s.arrow([(690, 96), (690, 128)])
    s.box(60, 230, 780, 80, "EdgeLightingEffect | core/edge-lighting.h",
          ["Config base + active | Clock | AnimationManager | PrepareSchedule",
           "`SetConfig, Attach, Update(dt), Render(w, h)"], "effect")
    s.arrow([(210, 96), (210, 230)], "SetConfig, Update, Render", label_dx=8, label_dy=0)
    s.arrow([(690, 194), (690, 230)])
    s.box(60, 360, 330, 190, "Renderers (compositing order)",
          ["`NeonRenderer", "`DropletsRenderer", "`LensFlareRenderer", "`SpotlightRenderer", "`DebugRenderer",
           "~a host's own (RendererLayer::CUSTOM)", "~each one visual layer; none calls another"], "renderer")
    s.box(510, 360, 330, 190, "ResourceContext (one per effect)",
          ["`ShaderCache", "~   compiled stages, shared", "`BufferRegistry", "~   records of every offscreen buffer",
           "`ReadList", "~   who reads which buffer this frame", "fullscreen quad"], "context")
    s.arrow([(225, 310), (225, 360)], "owns, calls in order", label_dx=8, label_dy=4)
    s.arrow([(675, 310), (675, 360)], "creates, shares", label_dx=8, label_dy=4)
    s.arrow([(390, 455), (510, 455)], "BuildProgram\nRegisterBuffer\nMarkBufferCurrent\nFind", both=True,
            label_dx=0, label_dy=-62, label_anchor="middle")
    s.box(60, 590, 780, 52, "", ["gl/ RAII wrappers | util/ | ShaderSource::* (shaders embedded at configure time)"],
          "gl", center=True)
    s.arrow([(225, 550), (225, 590)])
    s.arrow([(675, 550), (675, 590)])
    s.write("overview.svg")


# -----------------------------------------------------------------------------
# 2. Components and layering
# -----------------------------------------------------------------------------
def layering():
    s = Svg(1100, 600, "Layering: every band uses only the bands below it")
    bands = [
        ("Hosts", "demo/, demo-capi/, tools/ (layer-check, neon-scale-check, neon-guide-figures)", "host"),
        ("C ABI", "lib/capi/: libedge-lighting-c, flat el_* calls (C++ hosts skip it)", "gl"),
        ("Orchestrator", "core/edge-lighting.h: EdgeLightingEffect | animation/: Modulator, Animation", "effect"),
        ("Renderers", "renderer/: BaseRenderer and the five layers, their LUTs and tuning headers", "renderer"),
        ("Shared services", "core/: ResourceContext, BufferRegistry, ReadList | gl/: ShaderCache", "context"),
        ("Data", "core/config.h: Config, one plain sub-struct per layer, each with operator==", "context"),
        ("GL and helpers", "gl/: RAII wrappers | util/ | lib/shaders/ -> ShaderSource::*", "gl"),
        ("Third party", "GLAD | GLM | GLFW (demos and tools only)", "host"),
    ]
    x, w, h, gap = 200, 640, 50, 18
    y = 30
    centres = []
    for name, body, style in bands:
        s.rect(x, y, w, h, style)
        s.text(x - 16, y + 30, name, size=14, weight="bold", anchor="end")
        s.text(x + 14, y + 30, body, size=12.5)
        centres.append(y + h / 2)
        y += h + gap
    s.arrow([(1060, 40), (1060, y - gap - 10)], "uses", label_dx=-8, label_dy=0, label_anchor="end")
    # The one name that points up: BaseRenderer befriends the effect.
    s.arrow([(840, centres[3]), (880, centres[3]), (880, centres[2]), (842, centres[2])], dashed=True)
    s.note(892, centres[2] + 18, "the one upward name:\nBaseRenderer befriends the\neffect, so only it calls\nPrepare", size=11.5)
    s.write("layering.svg")


# -----------------------------------------------------------------------------
# 3. A frame
# -----------------------------------------------------------------------------
def frame():
    s = Svg(1000, 760, "One frame: Update, then Render in two phases")
    s.text(30, 36, "Update(dt)", size=15, weight="bold")
    steps = ["clock.Update(dt)", "animations:\nUpdate + Apply", "active config\nchanged?", "OnConfigChanged\n(each renderer)",
             "Update\n(each renderer)"]
    x = 30
    for i, st in enumerate(steps):
        lines = st.split("\n")
        s.box(x, 50, 170, 58, "", lines, "effect", center=True)
        if i:
            s.arrow([(x - 18, 79), (x, 79)])
        x += 188
    s.note(540, 128, "only when the composited config changed - an idle effect never calls it", size=11.5, anchor="middle")

    s.text(30, 172, "Render(w, h)", size=15, weight="bold")
    pre = ["registry.BeginFrame()\nnothing is current", "CollectReads\n(each renderer)", "PrepareSchedule::Build\nthe Prepare order"]
    x = 30
    for i, st in enumerate(pre):
        s.box(x, 186, 220, 58, "", st.split("\n"), "context", center=True)
        if i:
            s.arrow([(x - 22, 215), (x, 215)])
        x += 242
    s.arrow([(624, 244), (624, 300)])

    def lanes(y0, label):
        s.text(30, y0 - 10, label, size=13.5, weight="bold")
        s.rect(170, y0, 800, 50, "plain", radius=6)
        s.rect(170, y0 + 60, 800, 50, "plain", radius=6)
        s.text(160, y0 + 30, "own buffers", size=12.5, anchor="end", color=MUTED)
        s.text(160, y0 + 90, "caller's framebuffer", size=12.5, anchor="end", color=MUTED)

    def block(x, y, w, label, style):
        s.rect(x, y + 7, w, 36, style, radius=5)
        s.text(x + w / 2, y + 30, label, size=12, anchor="middle")

    # Now: two phases.
    y = 330
    lanes(y, "Now: every Prepare, then every Render")
    block(178, y, 230, "neon: tables, gather, 1b, fields", "phase1")
    block(414, y, 120, "flare (< 1.0)", "phase1")
    block(540, y, 130, "spotlight (< 1.0)", "phase1")
    xs = [(678, 120, "neon: fill, blit, ring"), (802, 52, "drops"), (858, 36, "flare"), (898, 36, "spot"),
          (938, 28, "dbg")]
    for bx, bw, lab in xs:
        block(bx, y + 60, bw, lab, "phase2")
    s.arrow([(674, y + 52), (674, y + 64)], "")
    s.note(178, y + 136, "Phase 1 (Prepare -> RenderOffscreen) marks each buffer current in the registry.", size=12)
    s.note(178, y + 154, "Phase 2 (Render -> RenderOnscreen) draws the caller's target in ONE run: on a tiler, one store per frame.",
           size=12)

    # Before: one phase per layer.
    y = 560
    lanes(y, "Before S3: each layer's offscreen pass inside its Render")
    seq = [(178, 150, "neon offscreen", 0, "phase1"), (332, 130, "neon on target", 1, "phase2"),
           (466, 40, "drops", 1, "phase2"), (510, 110, "flare offscreen", 0, "phase1"), (624, 70, "flare blit", 1, "phase2"),
           (698, 120, "spot offscreen", 0, "phase1"), (822, 70, "spot blit", 1, "phase2"), (896, 40, "dbg", 1, "phase2")]
    for bx, bw, lab, lane, style in seq:
        block(bx, y + 60 * lane, bw, lab, style)
    s.note(178, y + 136, "Every switch away from the caller's target and back stores its tiles out and loads them in again.",
           size=12, color="#cf222e")
    s.write("frame.svg")


# -----------------------------------------------------------------------------
# 4. Sharing between renderers
# -----------------------------------------------------------------------------
def sharing():
    s = Svg(1000, 600, "What a renderer gets from its ResourceContext")
    s.rect(30, 40, 290, 520, "renderer")
    s.text(48, 66, "a renderer (BaseRenderer)", size=14, weight="bold")
    calls = [
        (110, "BuildProgram(vert, frag)", "Initialize / first use"),
        (230, "RegisterBuffer(framebuffer)", "constructor, once"),
        (266, "MarkBufferCurrent(handle)", "phase 1, each frame"),
        (302, "Find(key), IsCurrent, GetTexture", "a reader, each frame"),
        (382, "CollectReads(config, out)", "top of each frame"),
        (482, "GetFullscreenQuad()", "draw time"),
    ]
    for cy, call, when in calls:
        s.text(48, cy, call, size=12.5, mono=True)
        s.text(48, cy + 15, when, size=11.5, color=MUTED, italic=True)

    s.rect(400, 30, 570, 540, "context", dashed=True)
    s.text(418, 56, "ResourceContext (one per effect, shared_ptr)", size=14, weight="bold")

    s.box(420, 76, 530, 96, "ShaderCache", ["stages compiled once per effect, matched by text (length + 64-bit hash)",
                                            "neon.vert: 1 compile, not 15 | a failed stage stays failed",
                                            "~programs are never shared: one per pass target and blend state"], "plain")
    s.box(420, 190, 530, 132, "BufferRegistry", ["records: key -> the owner's Framebuffer attachment or LUT",
                                                 "read live: texture, size, format | never a copy",
                                                 "PUBLIC (a product, a contract)  |  DEBUG (an intermediate)",
                                                 "current = marked since BeginFrame | version moves on change",
                                                 "~slots in a deque: a Find pointer survives later registrations"], "plain")
    s.box(420, 340, 530, 96, "ReadList  ->  PrepareSchedule", ["each renderer's declared reads: key and phase (PREPARE / RENDER)",
                                                              "orders a Prepare after the producer it reads",
                                                              "~logs a missing key, or a DEBUG buffer read outside debug"], "plain")
    s.box(420, 454, 530, 96, "Fullscreen quad", ["six NDC vertices, built on first request",
                                                 "every whole-target pass; the debug markers and view",
                                                 "~a renderer without a context builds its own"], "plain")
    s.arrow([(300, 114), (420, 114)])
    s.arrow([(300, 234), (420, 234)])
    s.arrow([(300, 270), (420, 270)])
    s.arrow([(420, 300), (300, 300)])
    s.arrow([(300, 386), (420, 386)])
    s.arrow([(300, 486), (420, 486)])
    s.write("sharing.svg")


# -----------------------------------------------------------------------------
# 5. A buffer record's life
# -----------------------------------------------------------------------------
def record():
    s = Svg(980, 450, "A buffer record's life in the registry")
    s.parts.append('<circle cx="225" cy="40" r="9" fill="#1f2328"/>')
    s.arrow([(225, 49), (225, 150)], "RegisterBuffer (constructor)", label_dx=10, label_dy=4)
    s.box(100, 150, 250, 80, "listed, not current", ["Find returns it; IsCurrent false", "~no texture yet, or not marked"],
          "plain")
    s.box(650, 150, 250, 80, "current this frame", ["IsCurrent true; version moved", "~if the bytes changed"], "public")
    s.arrow([(350, 172), (650, 172)], "MarkBufferCurrent in phase 1,\nonce it holds what the frame used",
            label_dx=0, label_dy=-30, label_anchor="middle")
    s.arrow([(650, 210), (350, 210)], "next registry.BeginFrame()", label_dx=0, label_dy=20, label_anchor="middle")
    s.box(40, 340, 260, 80, "in another context", ["SetResourceContext(other) moved it:", "~listed in that effect's registry"],
          "plain", dashed=True)
    s.arrow([(170, 230), (170, 340)], "renderer joins\nanother effect", label_dx=8, label_dy=-10)
    s.box(360, 340, 280, 80, "removed", ["~slot free for a later key", "~~BaseRenderer, SetResourceContext(nullptr)"],
          "plain")
    s.arrow([(320, 230), (320, 290), (500, 290), (500, 340)], "renderer destroyed, or fails Initialize",
            label_at=0.24, label_dx=0, label_dy=-8)
    s.box(680, 340, 280, 80, "closed: never current again", ["~the effect is gone: registry.Close()",
                                                             "~a kept renderer still draws, alone"], "warn")
    s.arrow([(775, 230), (775, 340)], "effect destroyed\n(from either state)", label_dx=8, label_dy=-10)
    s.write("record-lifecycle.svg")


# -----------------------------------------------------------------------------
# 6. Ownership
# -----------------------------------------------------------------------------
def ownership():
    s = Svg(980, 560, "Who owns what")
    s.box(40, 40, 220, 60, "Host", ["~or an el_effect handle"], "host", center=True)
    s.box(360, 40, 240, 60, "EdgeLightingEffect", ["~Config, Clock, animations"], "effect", center=True)
    s.box(360, 210, 240, 60, "a renderer", ["~BaseRenderer subclass"], "renderer", center=True)
    s.box(360, 400, 240, 76, "its GL objects", ["programs, Framebuffers,", "LUTs, VAOs, UBOs (RAII)"], "gl", center=True)

    s.rect(690, 180, 260, 330, "context", dashed=True)
    s.text(820, 206, "ResourceContext", size=14, weight="bold", anchor="middle")
    s.text(820, 224, "cache, registry, reads, quad", size=12, color=MUTED, anchor="middle")
    s.box(710, 270, 220, 60, "buffer records", ["~in the registry's slots"], "plain", center=True)
    s.box(710, 410, 220, 60, "compiled stages", ["~in the ShaderCache"], "plain", center=True)

    s.arrow([(260, 70), (360, 70)], "owns")
    s.arrow([(480, 100), (480, 210)], "shared_ptr (the list)", label_dx=8, label_dy=4)
    s.arrow([(150, 100), (150, 240), (360, 240)], "may keep a\nshared_ptr", label_at=0.3, label_dx=8, label_dy=0)
    s.arrow([(600, 70), (820, 70), (820, 180)], "shared_ptr", label_dx=-40, label_dy=-8)
    s.arrow([(600, 232), (690, 232)], "shared_ptr", label_dx=-34, label_dy=-8)
    s.arrow([(480, 270), (480, 400)], "owns (RAII members)", label_dx=-8, label_dy=4, label_anchor="end")
    s.arrow([(600, 255), (650, 255), (650, 300), (710, 300)], "registers", dashed=True, label_at=0.55,
            label_dx=-6, label_dy=4, label_anchor="end")
    s.arrow([(710, 318), (600, 420)], "refers to its\nFramebuffer", dashed=True, label_dx=-12, label_dy=-6,
            label_anchor="end")
    s.arrow([(600, 450), (710, 440)], "links", dashed=True, label_dx=0, label_dy=-10, label_anchor="middle")

    s.rect(40, 440, 280, 96, "plain", radius=6)
    s.parts.append(f'<path d="M 54 464 L 104 464" stroke="{LINE}" stroke-width="1.6" marker-end="url(#arrow)"/>')
    s.text(114, 468, "owns / strong reference", size=12)
    s.parts.append(f'<path d="M 54 490 L 104 490" stroke="{LINE}" stroke-width="1.6" stroke-dasharray="6 4" '
                   'marker-end="url(#arrow)"/>')
    s.text(114, 494, "refers, does not own", size=12)
    s.note(54, 520, "a linked program keeps its stages alive in GL", size=11.5)
    s.write("ownership.svg")


# -----------------------------------------------------------------------------
# 7. How data moves between layers
# -----------------------------------------------------------------------------
def channels():
    s = Svg(1060, 600, "The two channels between layers")
    s.text(40, 40, "1. The caller's framebuffer", size=15, weight="bold")
    s.note(40, 60, "drawn bottom to top, in compositing order", size=12)
    stack = [("DebugRenderer", "overlays, on top", "renderer"), ("SpotlightRenderer", "screen", "renderer"),
             ("LensFlareRenderer", "premultiplied over", "renderer"), ("DropletsRenderer", "premultiplied over", "renderer"),
             ("NeonRenderer", "premultiplied over; fill under it", "renderer"), ("the host's backdrop", "", "host")]
    y = 80
    for name, how, style in stack:
        s.rect(40, y, 340, 50, style)
        s.text(56, y + 30, name, size=13, weight="bold", mono=style == "renderer")
        s.text(364, y + 30, how, size=12, color=MUTED, anchor="end")
        y += 60
    s.arrow([(20, 420), (20, 90)], "")
    s.note(40, 460, "every layer writes a coverage alpha, never a literal 0:\na compositor that blends the surface by alpha\nwould drop the layer otherwise", size=12)

    s.text(440, 40, "2. The registry", size=15, weight="bold")
    s.note(440, 60, "producers mark records in phase 1; readers find and sample them", size=12)
    s.rect(440, 80, 250, 370, "renderer")
    s.text(456, 104, "NeonRenderer", size=13, weight="bold", mono=True)
    recs = ["GradientRing", "Emission", "GlowCover(Segments)", "Gather, Gather[1]", "Field, RingField", "Scaled"]
    ry = 118
    for r in recs:
        s.rect(456, ry, 218, 30, "debug", radius=5)
        s.text(466, ry + 20, r, size=12, mono=True)
        s.text(664, ry + 20, "DEBUG", size=10.5, color=MUTED, anchor="end")
        ry += 36
    public_top = ry
    for r in ["Glow", "Ring", "Composite"]:
        s.rect(456, ry, 218, 30, "public", radius=5)
        s.text(466, ry + 20, r, size=12, mono=True, weight="bold")
        s.text(664, ry + 20, "PUBLIC", size=10.5, color="#1a7f37", anchor="end", weight="bold")
        ry += 36
    public_bottom = ry - 6

    s.rect(440, 462, 250, 46, "renderer")
    s.rect(456, 470, 218, 30, "debug", radius=5)
    s.text(466, 490, "LensFlare.Scaled", size=12, mono=True)
    s.text(664, 490, "DEBUG", size=10.5, color=MUTED, anchor="end")
    s.rect(440, 518, 250, 46, "renderer")
    s.rect(456, 526, 218, 30, "debug", radius=5)
    s.text(466, 546, "Spotlight.Scaled", size=12, mono=True)
    s.text(664, 546, "DEBUG", size=10.5, color=MUTED, anchor="end")

    s.box(820, 110, 210, 104, "DebugRenderer", ["buffer view: any key", "LUT strip: GradientRing", "~(declared in CollectReads)"],
          "renderer")
    s.box(820, 300, 210, 140, "a host or its layer", ["Glow + Ring: neonLight,", "~via GetSharedLight and", "~UploadSharedLight", "Composite: one texture", "~(or Glow alone, masked)"],
          "host")
    s.arrow([(674, 133), (820, 150)], "")
    s.arrow([(690, 250), (770, 250), (770, 195), (820, 195)], "")
    s.note(700, 243, "any record", size=11.5, italic=False)
    s.rect(680, public_top, 6, public_bottom - public_top, "public", radius=2)
    s.arrow([(686, (public_top + public_bottom) / 2), (820, (public_top + public_bottom) / 2)], "")
    s.arrow([(674, 485), (820, 432)], "logged", dashed=True, color="#cf222e", label_at=0.6, label_dx=6,
            label_dy=22)
    s.note(820, 480, "reading a DEBUG record outside the\ndebug layer is logged: its producer\nmay change it with any optimisation",
           size=11.5)
    s.write("channels.svg")


if __name__ == "__main__":
    overview()
    layering()
    frame()
    sharing()
    record()
    ownership()
    channels()
    print("wrote 7 diagrams to", OUT)
