"""Draws the diagrams in docs/neon-shared-buffer.md and
docs/neon-shared-layer-plan.md as SVG files beside this script.

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
# 1. Today: the shared buffer is an extra drawing
# -----------------------------------------------------------------------------
def today():
    s = Svg(1240, 760, "The neon's shared buffer today: an extra drawing, made only while read")
    s.text(30, 34, "Today - the shared buffer is drawn a second time, only while something reads it",
           size=16, weight="bold")
    rows = [
        ("Pass 0 - emission table", ["~each perimeter sample's colour"], "Emission", "DEBUG", "plain", False),
        ("Pass 0b - glow coverage table", ["~how lit each edge piece is"], "GlowCover", "DEBUG", "plain", False),
        ("Pass 1a - gather", ["~colour arriving at each point"], "Gather", "DEBUG", "plain", False),
        ("Pass 1b / 1c - shading", ["~halo + bloom at resolutionScale"], "Scaled = Glow", "PUBLIC", "plain", False),
        ("Pass 1r - ring field bake", ["~a cache for the ring (some configs)"], "RingField", "DEBUG", "plain", False),
        ("Pass 1s - the shared buffer", ["reads Glow + Gather; only while read", "fill again + glow copy + RING AGAIN"],
         "Composite", "PUBLIC", "warn", False),
    ]
    centres, bottom = phase1_frame(s, rows)
    s.box(PASS_X, bottom + 18, 570, 60, "", ["~not drawn: 0 cost but useless  |  drawn: 1.3-2.2x on frames",
                                             "~where something moves (M2, 1080p, scale 1)"], "plain", center=True)

    passes = [
        ("Pass 2a - fill", ["~the opaque fill, full res"], "plain"),
        ("Pass 2b - blit", ["~Glow scaled up, edge mask"], "plain"),
        ("Pass 2c / 2r - edge ring", ["~SHADED every frame, full res"], "warn"),
    ]
    ys, px, pw, _ = phase2_frame(s, passes)

    # The screen's inputs.
    s.arrow([(BUF_X + BUF_W, centres[3]), (655, centres[3]), (655, ys[1] + 28), (px, ys[1] + 28)])
    s.arrow([(BUF_X + BUF_W, centres[2] - 6), (662, centres[2] - 6), (662, ys[2] + 22), (px, ys[2] + 22)])
    s.arrow([(BUF_X + BUF_W, centres[4]), (669, centres[4]), (669, ys[2] + 36), (px, ys[2] + 36)], dashed=True)
    # Readers.
    rx = 1000
    s.text(rx, 470, "Readers of Composite", size=14, weight="bold")
    readers = [("debug buffer view", ["~Debug > Buffer View"], "renderer"),
               ("another layer", ["~declares it in CollectReads"], "renderer"),
               ("a C++ host", ["~AddHostRead(COMPOSITE_KEY)"], "host")]
    for i, (name, lines, style) in enumerate(readers):
        y = 486 + i * 74
        s.box(rx, y, 220, 60, name, lines, style)
        s.arrow([(BUF_X + BUF_W, centres[5]), (960, centres[5]), (960, y + 30), (rx, y + 30)])
    s.note(rx, 720, "a read declaration is what\nmakes pass 1s run at all", size=11.5)
    s.legend(690, 520, [("warn", "the edge ring: shaded twice when read"),
                        ("public", "PUBLIC product: a written contract"),
                        ("debug", "DEBUG: internal, may change any time")])
    s.write("today.svg")


# -----------------------------------------------------------------------------
# 2. Proposed: the screen is drawn from the textures readers get
# -----------------------------------------------------------------------------
def proposed():
    s = Svg(1240, 730, "Proposed: the neon draws the screen from textures it shares")
    s.text(30, 34, "Proposed - the screen is drawn FROM the shared textures; sharing costs nothing",
           size=16, weight="bold")
    rows = [
        ("Pass 0 - emission table", ["~unchanged"], "Emission", "DEBUG", "plain", False),
        ("Pass 0b - glow coverage table", ["~unchanged"], "GlowCover", "DEBUG", "plain", False),
        ("Pass 1a - gather", ["~unchanged"], "Gather", "DEBUG", "plain", False),
        ("Pass 1b / 1c - shading", ["~unchanged: halo + bloom"], "Scaled = Glow", "PUBLIC", "public", False),
        ("Pass 1r - ring field bake", ["~unchanged cache"], "RingField", "DEBUG", "plain", False),
        ("Pass 1t - the edge ring", ["from Gather (+ RingField): final colours", "redrawn only when something moved"],
         "Ring", "PUBLIC", "public", True),
        ("Pass 1s - flat texture", ["only if one plain texture is asked for", "COPIES fill + Glow + Ring; no shading"],
         "Composite", "PUBLIC", "plain", False),
    ]
    centres, bottom = phase1_frame(s, rows)

    passes = [
        ("Pass 2a - fill", ["~the opaque fill, full res"], "plain"),
        ("Pass 2b - blit", ["~Glow scaled up, edge mask"], "plain"),
        ("Pass 2c - ring copy", ["~Ring atlas -> screen, a copy"], "public"),
    ]
    ys, px, pw, _ = phase2_frame(s, passes)

    s.arrow([(BUF_X + BUF_W, centres[3]), (655, centres[3]), (655, ys[1] + 28), (px, ys[1] + 28)])
    s.arrow([(BUF_X + BUF_W, centres[5] - 6), (668, centres[5] - 6), (668, ys[2] + 28), (px, ys[2] + 28)])
    # The readers sit under the screen, fed by the same two lines that feed
    # it: whatever the screen is drawn from, a reader gets too.
    rx, rw = 690, 250
    s.text(rx, 418, "Readers", size=14, weight="bold")
    parts_y, flat_y, view_y = 430, 544, 640
    s.box(rx, parts_y, rw, 76, "a parts reader", ["~neon-layer.glsl: Glow + Ring", "~free: nothing extra drawn"],
          "renderer")
    s.box(rx, flat_y, rw, 76, "a flat-texture reader", ["~a host, a compositor", "~one cheap copy pass (1s)"], "host")
    s.box(rx, view_y, rw, 60, "debug buffer view", ["~any record"], "renderer")
    s.arrow([(655, centres[3]), (655, parts_y + 24), (rx, parts_y + 24)])
    s.arrow([(668, centres[5] - 6), (668, parts_y + 44), (rx, parts_y + 44)])
    s.arrow([(BUF_X + BUF_W, centres[6]), (rx, centres[6])])

    s.note(980, 440, "the screen and every reader\nsee the SAME two textures:\nGlow + Ring", size=12.5, italic=False)
    s.legend(980, 510, [("public", "PUBLIC: shared, screen uses it"), ("debug", "DEBUG: internal caches")])
    s.box(PASS_X, bottom + 18, 570, 60, "", ["~still frame: the ring is only copied (cheaper than today)",
                                             "~moving frame: the ring is shaded once, then copied"], "plain",
          center=True)
    s.write("proposed.svg")


# -----------------------------------------------------------------------------
# 3. Now: what was built (steps 1-3 of the plan)
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
# 4. What runs, per kind of frame
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


if __name__ == "__main__":
    today()
    proposed()
    current()
    per_frame()
    print("wrote 4 diagrams to", OUT)
