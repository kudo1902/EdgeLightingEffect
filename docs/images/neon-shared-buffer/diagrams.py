"""Draws the diagrams in docs/guides/neon-shared-buffer.md,
docs/guides/neon-shared-buffer-explained.md and
docs/plans/neon-shared-parts-plan.md as SVG files beside this script.

    python3 docs/images/neon-shared-buffer/diagrams.py

Plain Python, no dependencies. It borrows the drawing helpers and styles of
docs/images/architecture/diagrams.py, so these diagrams look like the
architecture page's. Edit this script and rerun it; do not edit the SVGs.
"""

import importlib.util
import os

OUT = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location(
    "architecture_diagrams", os.path.join(OUT, "..", "architecture", "diagrams.py"))
_arch = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_arch)

MUTED = _arch.MUTED
RED = "#cf222e"
GREEN = "#1a7f37"


class Svg(_arch.Svg):
    def write(self, name):
        # The architecture script writes beside itself; these go beside this one.
        _arch.OUT = OUT
        super().write(name)

    def chip(self, x, y, w, name, kind, new=False):
        """A buffer: a small box with its registry key and PUBLIC / DEBUG."""
        style = "public" if kind == "PUBLIC" else "debug"
        self.rect(x, y, w, 32, style, radius=5, stroke_width=2.2 if new else 1.5)
        self.text(x + 10, y + 21, name, size=12.5, mono=True, weight="bold" if kind == "PUBLIC" else "normal")
        color = GREEN if kind == "PUBLIC" else MUTED
        self.text(x + w - 10, y + 21, ("NEW  " if new else "") + kind, size=10.5, color=color, anchor="end",
                  weight="bold" if kind == "PUBLIC" else "normal")

    def legend(self, x, y, items):
        for k, (style, label) in enumerate(items):
            self.rect(x, y + 24 * k, 22, 16, style, radius=3)
            self.text(x + 32, y + 24 * k + 13, label, size=12)


# Rows shared by both architecture diagrams: (pass, lines, buffer, kind,
# style, new). A row is taller when its pass box carries two lines.
PASS_X, PASS_W = 50, 300
BUF_X, BUF_W = 380, 240
ROW_GAP = 14


def row_height(lines):
    return 56 if len(lines) <= 1 else 76


def phase1_frame(s, rows):
    """The phase 1 panel and its rows of pass -> buffer; returns each row's
    buffer centre and the panel's bottom."""
    y = 104
    centres = []
    boxes = []
    for name, lines, buf, kind, style, new in rows:
        h = row_height(lines)
        boxes.append((y, h, name, lines, buf, kind, style, new))
        y += h + ROW_GAP
    bottom = y + 4
    s.rect(30, 52, 610, bottom - 52, "phase1")
    s.text(48, 78, "Phase 1 - Prepare: offscreen, into the neon's own textures", size=14, weight="bold")
    for y, h, name, lines, buf, kind, style, new in boxes:
        s.box(PASS_X, y, PASS_W, h, name, lines, style)
        cy = y + h / 2
        s.chip(BUF_X, cy - 16, BUF_W, buf, kind, new)
        s.arrow([(PASS_X + PASS_W, cy), (BUF_X, cy)])
        centres.append(cy)
    return centres, bottom


def phase2_frame(s, passes, top=52):
    """The phase 2 panel: its passes stacked, then the screen."""
    x, w = 690, 250
    h = 40 + len(passes) * 66 + 96
    s.rect(x - 20, top, w + 40, h, "phase2")
    s.text(x - 2, top + 26, "Phase 2 - Render: the screen", size=14, weight="bold")
    ys = []
    for i, (name, lines, style) in enumerate(passes):
        y = top + 44 + i * 66
        s.box(x, y, w, 56, name, lines, style)
        ys.append(y)
        if i:
            s.arrow([(x + w / 2, y - 10), (x + w / 2, y)])
    screen_y = top + 44 + len(passes) * 66 + 14
    s.box(x, screen_y, w, 56, "the caller's framebuffer", ["~what you see; dithered"], "host", center=True)
    s.arrow([(x + w / 2, screen_y - 24), (x + w / 2, screen_y)])
    return ys, x, w, top + h


# -----------------------------------------------------------------------------
# 1. Now: what was built (steps 1-3 of the shared-layer plan)
# -----------------------------------------------------------------------------
def current():
    s = Svg(1240, 720, "The neon's shared light now: two parts, read or assembled")
    s.text(30, 34, "Now - the screen is drawn from the shared parts; a flat texture is assembled from them",
           size=16, weight="bold")
    rows = [
        ("Pass 0 / 0b - tables", ["~emission, glow coverage"], "Emission, GlowCover", "DEBUG", "plain", False),
        ("Pass 1a - gather", ["~colour arriving at each point"], "Gather", "DEBUG", "plain", False),
        ("Pass 1b / 1c - shading", ["~halo + bloom at resolutionScale"], "Scaled = Glow", "PUBLIC", "public", False),
        ("Pass 1r - ring field bake", ["~a cache, on moving frames"], "RingField", "DEBUG", "plain", False),
        ("Pass 1t - the edge ring", ["into the ring atlas: on a still frame's", "first, or while something reads it"],
         "Ring", "PUBLIC", "public", False),
        ("Pass 1s - flat texture", ["only while read; fill at s, then the", "light AVERAGED from Glow + Ring"],
         "Composite", "PUBLIC", "plain", False),
    ]
    centres, bottom = phase1_frame(s, rows)
    passes = [
        ("Pass 2a - fill", ["~the opaque fill, full res"], "plain"),
        ("Pass 2b - blit", ["~Glow scaled up, edge mask"], "plain"),
        ("Pass 2c - edge ring", ["~atlas COPY, or shaded (moving, unread)"], "public"),
    ]
    ys, px, pw, _ = phase2_frame(s, passes)
    s.arrow([(BUF_X + BUF_W, centres[2]), (655, centres[2]), (655, ys[1] + 28), (px, ys[1] + 28)])
    s.arrow([(BUF_X + BUF_W, centres[4] - 6), (668, centres[4] - 6), (668, ys[2] + 28), (px, ys[2] + 28)])

    rx, rw = 690, 250
    s.text(rx, 418, "Readers", size=14, weight="bold")
    parts_y, flat_y, view_y = 430, 544, 640
    s.box(rx, parts_y, rw, 76, "a parts reader", ["~neon-light.glsl: Glow + Ring", "~cost: the ring via its atlas"],
          "renderer")
    s.box(rx, flat_y, rw, 76, "a flat-texture reader", ["~a host, a compositor", "~cost: + one assemble pass"], "host")
    s.box(rx, view_y, rw, 60, "debug buffer view", ["~any record"], "renderer")
    s.arrow([(655, centres[2]), (655, parts_y + 24), (rx, parts_y + 24)])
    s.arrow([(668, centres[4] - 6), (668, parts_y + 44), (rx, parts_y + 44)])
    s.arrow([(BUF_X + BUF_W, centres[5]), (678, centres[5]), (678, flat_y + 38), (rx, flat_y + 38)])

    s.note(980, 440, "no pass shades the neon\ntwice: readers get the\ntextures the screen uses", size=12.5,
           italic=False)
    s.legend(980, 510, [("public", "PUBLIC: shared, screen uses it"), ("debug", "DEBUG: internal caches")])
    s.box(PASS_X, bottom + 18, 570, 60, "", ["~still frame: the ring is copied from its atlas",
                                             "~moving frame, nobody reading: the ring is shaded on screen, as before"],
          "plain", center=True)
    s.write("current.svg")


# -----------------------------------------------------------------------------
# 2. What runs, per kind of frame
# -----------------------------------------------------------------------------
def per_frame():
    cols = ["still frame", "something moved", "+ a parts reader", "+ a flat-texture reader"]
    rows = [
        ("Today", [
            ["ring SHADED on screen", "(or from its field)"],
            ["glow redrawn", "ring SHADED on screen"],
            ["(no parts to read)"],
            ["+ fill, glow copy and", "ring SHADED AGAIN at s"],
        ], ["warn", "warn", "plain", "warn"]),
        ("Proposed", [
            ["ring COPIED from atlas"],
            ["glow redrawn", "ring shaded into atlas, copied"],
            ["nothing extra"],
            ["+ one copy pass:", "fill + Glow + Ring at s"],
        ], ["public", "plain", "public", "public"]),
        ("Built", [
            ["ring COPIED from atlas", "1.04-1.07x (M2)"],
            ["as before: ring shaded", "on the screen, 1.00x"],
            ["ring into atlas, copied", "+0.01-0.03 ms"],
            ["+ one assemble pass:", "fill + averaged light"],
        ], ["public", "plain", "plain", "public"]),
    ]
    x0, y0, cw, rh, lw = 150, 70, 250, 84, 120
    s = Svg(x0 + cw * len(cols) + 30, y0 + 44 + rh * len(rows) + 60, "What the neon does on each kind of frame")
    s.text(30, 36, "What the neon does on each kind of frame (the gather, tables and glow work are the same in both)",
           size=15, weight="bold")
    for j, c in enumerate(cols):
        s.rect(x0 + j * cw + 4, y0, cw - 8, 36, "phase2", radius=6)
        s.text(x0 + j * cw + cw / 2, y0 + 23, c, size=13, weight="bold", anchor="middle")
    for i, (name, cells, styles) in enumerate(rows):
        y = y0 + 44 + i * rh
        s.text(x0 - 16, y + rh / 2 + 2, name, size=14, weight="bold", anchor="end")
        for j, (cell, style) in enumerate(zip(cells, styles)):
            s.rect(x0 + j * cw + 4, y + 4, cw - 8, rh - 8, style, radius=6)
            ty = y + rh / 2 + 4 - 9 * (len(cell) - 1)
            for k, line in enumerate(cell):
                s.text(x0 + j * cw + cw / 2, ty + 18 * k, line, size=12.5, anchor="middle")
    s.legend(x0, y0 + 44 + rh * len(rows) + 10, [("warn", "costs more than it needs to"),
                                                  ("public", "cheaper than today")])
    s.write("per-frame.svg")


# -----------------------------------------------------------------------------
# 3. One frame's decisions, as RenderOffscreen takes them
# -----------------------------------------------------------------------------
def decisions():
    s = Svg(1200, 1100, "One frame's decisions about the ring atlas and the shared buffer")
    s.text(30, 34, "One frame's decisions - RenderOffscreen, after it knows whether anything moved", size=16,
           weight="bold")
    s.rect(20, 52, 1160, 868, "phase1")
    s.text(38, 78, "Phase 1 - Prepare", size=14, weight="bold")

    # The three reasons to need the atlas.
    s.box(40, 96, 340, 74, "nothing moved", ["~reuseOffscreen: passes 1a and 1b", "~are reused this frame"])
    s.box(430, 96, 340, 74, "the Ring is read", ["`IsBufferWanted(RING_KEY)", "~a layer's read, or AddHostRead"])
    s.box(820, 96, 340, 74, "the Composite is wanted", ["~compositeScale > 0, read, and", "~its two programs built"])

    cx, cw = 300, 600
    s.box(cx, 214, cw, 76, "the ring atlas is needed", [
        "~possible: the glow drawn, a ring on screen, half float, and not a size",
        "~whose allocation just failed - AND one of the three above"], center=True)
    for x in (210, 600, 990):
        s.arrow([(x, 170), (x, 192), (cx + cw / 2 if x == 600 else (cx + 60 if x < 600 else cx + cw - 60), 192),
                 (cx + cw / 2 if x == 600 else (cx + 60 if x < 600 else cx + cw - 60), 214)])

    s.box(40, 336, 230, 92, "no atlas this frame", ["~2c shades the ring on the", "~screen (or composites its", "~field), as before"],
          "plain")
    s.arrow([(cx, 252), (155, 252), (155, 336)], "no", label_at=0.35, label_dy=-8)

    s.box(cx, 336, cw, 76, "reuse it?", [
        "~nothing moved, it holds last frame's ring, it is EXACT (not a field scaled",
        "~to a moving intensity), and the viewport and packing are the same"], center=True)
    s.arrow([(cx + cw / 2, 290), (cx + cw / 2, 336)], "yes", label_dy=8)

    s.box(cx, 458, 280, 76, "reuse the atlas", ["~no pass; its version holds"], "public", center=True)
    s.box(cx + 320, 458, 280, 76, "pass 1t: draw it", ["~shaded, or from the ring's field;", "~sets exact"], "public",
          center=True)
    s.arrow([(cx + 140, 412), (cx + 140, 458)], "yes", label_dy=8)
    s.arrow([(cx + 460, 412), (cx + 460, 458)], "no", label_dy=8)

    s.box(cx, 580, cw, 76, "the shared light (SharedLight)", [
        "~valid while the glow and the atlas hold this frame's light - or the glow",
        "~alone, with no ring on screen; what pass 1s and GetSharedLight hand on"], center=True)
    s.arrow([(cx + 140, 534), (cx + 140, 580)])
    s.arrow([(cx + 460, 534), (cx + 460, 580)])

    s.box(cx, 702, cw, 76, "reuse the shared buffer?", [
        "~wanted, nothing moved, the atlas NOT drawn again this frame (it is",
        "~assembled from it), and the same scale, viewport and region"], center=True)
    s.arrow([(cx + cw / 2, 656), (cx + cw / 2, 702)])
    s.arrow([(990, 170), (990, 192), (1150, 192), (1150, 740), (cx + cw, 740)], "wanted", label_at=0.62,
            label_dx=-58, label_dy=-8)

    s.box(cx, 824, 280, 76, "reuse it", ["~no pass; its version holds"], "public", center=True)
    s.box(cx + 320, 824, 280, 76, "pass 1s: assemble it", ["~fill at s, then the light averaged", "~per texel (needs the light valid)"],
          "public", center=True)
    s.arrow([(cx + 140, 778), (cx + 140, 824)], "yes", label_dy=8)
    s.arrow([(cx + 460, 778), (cx + 460, 824)], "no", label_dy=8)

    s.note(40, 716, "not wanted:\nno pass 1s, and the\nComposite record is\nnot current", size=12)
    s.note(940, 860, "then: publishBuffers marks\nRing and Composite current\nwhen they hold this frame's", size=12)

    # Phase 2.
    s.rect(20, 940, 1160, 140, "phase2")
    s.text(38, 966, "Phase 2 - Render: the screen", size=14, weight="bold")
    s.box(40, 984, 240, 76, "2a fill, 2b blit", ["~the blit reads the Glow"], "plain")
    s.box(cx, 984, cw, 76, "pass 2c: the edge ring", [
        "~mFramePlan.ringFromAtlas: COPY the atlas, dithered - else shade the",
        "~ring on the screen, or composite its field, as before the atlas"], "public", center=True)
    s.arrow([(280, 1022), (cx, 1022)])
    s.write("decisions.svg")


# -----------------------------------------------------------------------------
# 4. The shared buffer among the neon's other components
# -----------------------------------------------------------------------------
def components():
    s = Svg(1240, 920, "The shared buffer among the neon renderer's other components")
    s.text(30, 34, "The shared buffer among the neon's other components", size=16, weight="bold")

    # Left: what the screen needs anyway.
    s.rect(20, 52, 420, 614, "phase1")
    s.text(38, 78, "The neon's own pipeline (phase 1)", size=14, weight="bold")
    s.note(38, 96, "drawn for the screen whether or not anyone shares", size=12)
    lx, lw = 40, 380
    s.box(lx, 112, lw, 56, "Tables - pass 0, 0b", ["~emission per sample; coverage per piece"])
    s.box(lx, 184, lw, 56, "Gather - pass 1a", ["~the colour arriving at each point"])
    s.box(lx, 256, lw, 92, "Glow - pass 1b, or 1c", ["~halo + bloom at resolutionScale,", "~or the glow field's composite",
                                                       "`NeonRenderer.Glow"], "public")
    s.box(lx, 364, lw, 74, "Ring field - pass 1r", ["~a cache of the ring on moving frames;", "~reads the gather"])
    s.box(lx, 454, lw, 92, "Ring atlas - pass 1t", ["~the edge ring: final colours, packed,", "~undithered; reads the gather",
                                                      "`NeonRenderer.Ring"], "public")
    s.box(lx, 562, lw, 88, "Geometry (OnConfigChanged)", ["~mRingBounds: the ring's annulus", "~mBlitBounds: the light's whole box",
                                                            "~mFillOuter: the fill's band"])
    s.arrow([(lx + 190, 168), (lx + 190, 184)])
    s.arrow([(lx + 190, 240), (lx + 190, 256)])
    s.arrow([(lx + 190, 438), (lx + 190, 454)])

    # Middle: the shared buffer.
    mx, mw = 500, 280
    s.box(mx, 300, mw, 170, "Shared buffer - pass 1s", [
        "1. the fill, at scale s",
        "~   (drawFill, its own program)",
        "2. the light: neonLight at every",
        "~   pixel, averaged per texel",
        "~   (neon-assemble.frag)",
        "`NeonRenderer.Composite"], "public")
    s.arrow([(lx + lw, 302), (470, 302), (470, 340), (mx, 340)], "Glow", label_at=0.12, label_dy=-8)
    s.arrow([(lx + lw, 500), (470, 500), (470, 410), (mx, 410)], "Ring", label_at=0.1, label_dy=-8)
    s.arrow([(lx + lw, 606), (485, 606), (485, 450), (mx, 450)], "boxes", label_at=0.1, label_dy=-8)
    # The one way it changes the rest: the ring goes through its atlas.
    s.arrow([(mx + 40, 470), (mx + 40, 526), (lx + lw, 526)], dashed=True, color=RED)
    s.note(mx + 50, 500, "while it is wanted, the ring\natlas is drawn on moving\nframes too", size=12, color=RED,
           italic=False)

    # Right: what steers it.
    s.rect(820, 52, 400, 614, "context")
    s.text(838, 78, "What steers it", size=14, weight="bold")
    rx, rw = 840, 360
    s.box(rx, 96, rw, 110, "Who reads it -> IsBufferWanted", [
        "a layer's CollectReads", "a host's EdgeLightingEffect::AddHostRead", "the debug layer's buffer view",
        "~nobody: pass 1s never runs"], "effect")
    s.box(rx, 222, rw, 92, "NeonConfig::compositeScale", [
        "~0: off, and the buffer released", "~else clamped to [max(res, 0.4), 1]",
        "~a change alone: setupCompositeGeometry"])
    s.box(rx, 330, rw, 92, "Reuse", [
        "~nothing moved (reuseOffscreen), the ring", "~atlas not drawn again, same scale,",
        "~viewport and region: pass 1s skipped"])
    s.box(rx, 438, rw, 74, "Releases", ["~Update: 5 s unread", "~OnConfigChanged: scale 0, layer off"])
    s.box(rx, 528, rw, 74, "Its programs", ["~mCompositeFillShader, mAssembleShader:", "~built the first frame it is wanted"])
    s.arrow([(rx, 150), (800, 150), (800, 330), (mx + mw, 330)])
    s.arrow([(rx, 268), (800, 268)])
    s.arrow([(rx, 376), (mx + mw, 376)])
    s.arrow([(rx, 475), (800, 475), (800, 430), (mx + mw, 430)])

    # Phase 2.
    s.rect(20, 690, 1200, 210, "phase2")
    s.text(38, 716, "Phase 2 - the screen", size=14, weight="bold")
    s.box(40, 734, 260, 66, "2a fill", ["~full resolution, its own program"])
    s.box(320, 734, 260, 66, "2b blit", ["~the Glow, scaled up, edge mask"])
    s.box(600, 734, 300, 66, "2c edge ring", ["~a COPY of the ring atlas when it", "~holds this frame's ring"], "public")
    s.box(940, 734, 260, 66, "a reader", ["~samples Composite in its Render,", "~or after the frame"], "host")
    s.arrow([(lx, 302), (30, 302), (30, 676), (400, 676), (400, 734)])
    s.arrow([(lx + lw, 530), (432, 530), (432, 682), (750, 682), (750, 734)])
    s.arrow([(760, 470), (760, 668), (1070, 668), (1070, 734)])
    s.note(40, 834, "The screen is never drawn from the shared buffer. It is drawn from the same two textures the shared\n"
                    "buffer is assembled from, so the two agree - the screen's own dither aside.", size=12.5)
    s.write("components.svg")


# -----------------------------------------------------------------------------
# 5. The plan: the two parts as the product, no flat buffer in the neon
# -----------------------------------------------------------------------------
def parts_core():
    s = Svg(1240, 660, "The neon's light as two parts, read by any reader in its own pass")
    s.text(30, 34, "Planned - the two parts are the product; a flat texture is a reader's own pass", size=16,
           weight="bold")
    rows = [
        ("Pass 0 / 0b / 1a", ["~tables and gather"], "Emission, Gather", "DEBUG", "plain", False),
        ("Pass 1b / 1c - the glow", ["~halo + bloom at resolutionScale"], "Glow", "PUBLIC", "public", False),
        ("Pass 1t - the ring atlas", ["~on a still frame's first, or while", "~the Ring is read"], "Ring", "PUBLIC",
         "public", False),
    ]
    centres, bottom = phase1_frame(s, rows)
    s.box(PASS_X, bottom + 16, 570, 92, "the neon's light: Glow + Ring, described (NeonLight)", [
        "~the two textures, the glow's box, the ring's boxes and packing, the",
        "~edge mask, the viewport and a version - found through the effect or",
        "~the context, not a pointer to the neon"], "public")
    s.arrow([(BUF_X + 120, centres[2] + 16), (BUF_X + 120, bottom + 16)])
    s.arrow([(BUF_X + 60, centres[1] + 16), (BUF_X + 60, centres[2] - 16)])
    passes = [
        ("Pass 2a - fill", ["~unchanged"], "plain"),
        ("Pass 2b - blit", ["~Glow scaled up, edge mask"], "plain"),
        ("Pass 2c - edge ring", ["~copy of Ring, or shaded"], "public"),
    ]
    ys, px, pw, _ = phase2_frame(s, passes)
    s.arrow([(BUF_X + BUF_W, centres[1]), (655, centres[1]), (655, ys[1] + 28), (px, ys[1] + 28)])
    s.arrow([(BUF_X + BUF_W, centres[2] - 6), (668, centres[2] - 6), (668, ys[2] + 28), (px, ys[2] + 28)])

    rx, rw = 990, 230
    s.text(rx, 78, "Readers (their own passes)", size=14, weight="bold")
    s.box(rx, 96, rw, 110, "a host shader", ["~NEON_LIGHT_GLSL:", "~neonLight(p) per pixel", "~declares a read of Ring"],
          "host")
    s.box(rx, 222, rw, 110, "a library layer", ["~the same chunk; its", "~CollectReads orders it", "~after the neon"], "renderer")
    s.box(rx, 348, rw, 128, "a flat texture, if", ["~a reader wants one: its", "~own pass at its own scale,", "~neonLightOverSquare()",
                                                      "~(today's pass 1s, moved)"], "host")
    for y in (150, 276, 410):
        s.arrow([(972, y), (rx, y)])
    s.arrow([(PASS_X + 570, bottom + 62), (972, bottom + 62), (972, 150)])
    s.note(rx, 510, "no pass in the neon exists\nfor readers but the ring\natlas on moving frames,\nand only while it is read",
           size=12)
    s.write("parts-core.svg")


if __name__ == "__main__":
    current()
    per_frame()
    decisions()
    components()
    parts_core()
    print("wrote 5 diagrams to", OUT)
