# Neon resolution scale: performance before and after

What the resolution-scale work
([`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md), steps 1-5,
plus the `invariant gl_Position` follow-up) did to the neon layer's frame time,
startup and memory. "Before" is `542dad4`, the last commit before step 1;
"after" is `db2c250`. The quality it bought is in
[`neon-resolution-scale-comparison.html`](neon-resolution-scale-comparison.html):
this is the price.

Sections 1-7 are from one machine - an i7-9750H MacBook Pro whose GL
renderer is an **AMD Radeon Pro 5300M** (x86_64 build, macOS GL 4.1) - and the
plan's own step timings were taken on an Apple M2 Pro, where the ring measured
far cheaper. Section 8 is on Mesa llvmpipe and section 9 on an Apple M2 Pro.
Read ratios, not milliseconds, and expect the target device to differ again.

**Result.** Scale 1.0 barely moves - its output is byte-identical and its cost
rose by +1 to +58 us (about 1% of the frame), almost all of it from step 2.
Every reduced scale costs more: **+0.10 to +0.25 ms at 1280 x 720 and +0.12 to
+0.35 ms at 1920 x 1080**, three quarters of it the edge ring (step 5) and most
of the rest the gather target (step 4). On the default scene that takes 0.25
from 7.4x faster than 1.0 to 5.2x at 720p, and from 8.3x to 6.0x at 1080p; 0.5
goes from 3.1x to 2.7x at both. Two scenes that were already marginal lose more:
the cutoff band is now about 2x SLOWER below 1.0 than at it, and a one-sided
glow keeps only 1.2x to 1.8x at 720p (and is slower than 1.0 at 0.75). Startup
doubles, 23 ms to 50 ms per effect, and the scaled buffer's memory doubles
(triples with segments). The plan's section 12 has since recovered part of
this: programs are built per path on first use, so a host compiles only the
path it draws; and the composite and the ring cover only where the glow can be
lit, which mostly helps the cutoff band and the one-sided glows.

**Since then** (section 10): the glow-coverage fixes V19, V20 and V21 in
[`review-findings.md`](review-findings.md) added a coverage table and a
per-piece read of it, which every number above predates. On an Apple M2 Pro
V19 and V20 cost about 1.04x at scale 1.0 and 1.08x at 0.5 on a fully lit
ring, and 1.1-1.2x on a partly lit one; V21, on an AMD Radeon Pro 5300M, is
1.00-1.01x of that at 1.0 and 4-7% faster below it; plus a per-frame bake
under animation. Section 9 was not re-run against them.

**Against `main`** (section 9, Apple M2 Pro): at scale 0.5 the branch now
renders the neon **2.3x faster than `main` at 1280 x 720 and 2.8x at 1920 x
1080**, and passes `neon-scale-check check` where `main`'s 0.5 is up to 77/255
off its own 1.0. Scale 1.0 is within run-to-run noise of `main`, with one
exception (`segments` at 720p, about 11% faster).

## 1. Method

| build | commit | what it changed |
| ----- | ------ | --------------- |
| before | `542dad4` | the scaled path as it was: gather into a reduced buffer, bilinear blit |
| step 1 | `546d2b7` | the spotlight and flare get their own `blit.frag`; the neon's blit gains the tuning header (no code) |
| step 2 | `45bd1f0` | inside / outside cutoffs move into the neon's blit, at destination resolution |
| step 3 | `c155a37` | `Framebuffer` gains extra colour attachments (unused yet) |
| step 4 | `8a50121` | pass 1 below 1.0 writes the gather target: 1-2 extra RGBA8 attachments, blending off |
| step 5 | `ea62200` | the edge ring: pass 2c re-shades a ring around the edge at full resolution |
| after | `db2c250` | `invariant gl_Position` in `neon.vert` |

Each build's library was compiled from its own commit and timed by
[`tools/neon-scale-check`](../tools/neon-scale-check/README.md)'s `time`
command, built standalone against it: the comparison page's twelve scenes, at
six resolution scales, at 1280 x 720 and at 1920 x 1080 (the scenes' layout
scaled to the frame, the neon's px parameters not). Each figure is one freshly
initialised effect, five warm-up frames, then the minimum over 5 runs of the
mean of 40 `Render()` calls between `glFinish`.

**Rounds and the median.** All seven builds ran in three rounds with the build
order rotated each round, and before / after got two more rounds of their own
in ABBA order. Each figure is the **median over its rounds**, not the minimum,
because the minimum was measurably biased: the first builds of round 1 ran on a
cooler GPU and came out faster (`542dad4` at 0.5, 720p: 0.770 ms in round 1
against 0.806 and 0.802), so a minimum hands that luck to whichever build runs
first - it made step 1, which changes no neon code, look +25 us dearer. The
before / after tables use all five rounds of both builds; the per-step table
uses rounds 1-3 only, where every build ran interleaved with every other.

**Noise.** Steps 1 and 3 and the `invariant` commit add no per-frame neon work,
and they measure within +/-10 us at every scale and size - that is the noise
band of a median here. Single scenes swing +/-30-50 us at the small scales,
where a frame is only 0.2-0.4 ms.

**One anomaly, at 0.75.** The three builds before step 3 are bimodal there: a
process lands at about 1.8 ms or about 2.2 ms on some scenes and stays there
(`segments`, `arcs`, `default` at 720p). From step 3 on, which rewrote how the
scaled buffer's textures are allocated, every run lands in the fast mode. The
cause is not established. It makes the 0.75 "before" figures unreliable on
those scenes - `segments` at 0.75 below reads its slow mode - and the
conclusions here rest on 0.5 and below.

## 2. Scale 1.0

| | 1280 x 720 | 1920 x 1080 |
| - | ---------- | ----------- |
| after - before, range over 12 scenes | +1 to +33 us | +5 to +58 us |
| as a share of the frame | at most 1.3% | at most 1.9% (the 0.28 ms band scene; about 1% otherwise) |
| step 2's share (median) | +15 us | +24 us |

The direct path's OUTPUT is byte-identical through every step (re-verified in
the plan's section 7), but its program is not: step 2 added the
`blitOwnsCut`-gated cutoff code to `neon.frag`, which the direct path compiles
too, and it costs every scene a little - all twelve positive at both sizes.
Nothing else in the work touches scale 1.0.

## 3. Reduced scales, before and after

ms per frame, with the speed-up against the same build's own 1.0 render.

**1280 x 720**

| scene | 1.0 before / after | 0.5 before | **0.5 after** | 0.25 before | **0.25 after** | 0.125 before | **0.125 after** |
| ----- | ------------------ | ---------- | ------------- | ----------- | -------------- | ------------ | --------------- |
| `default` | 2.64 / 2.67 | 0.86 (3.1x) | **0.99 (2.7x)** | 0.36 (7.4x) | **0.51 (5.2x)** | 0.22 (11.8x) | **0.47 (5.7x)** |
| `hairline` | 1.95 / 1.98 | 0.69 (2.8x) | **0.89 (2.2x)** | 0.32 (6.1x) | **0.51 (3.9x)** | 0.21 (9.3x) | **0.42 (4.7x)** |
| `crisp_tube` | 2.31 / 2.34 | 0.81 (2.9x) | **0.99 (2.4x)** | 0.35 (6.5x) | **0.55 (4.3x)** | 0.24 (9.6x) | **0.45 (5.2x)** |
| `soft_wash` | 2.68 / 2.70 | 0.89 (3.0x) | **1.12 (2.4x)** | 0.38 (7.0x) | **0.60 (4.5x)** | 0.26 (10.3x) | **0.47 (5.8x)** |
| `sharp_corners` | 2.20 / 2.21 | 0.74 (3.0x) | **0.88 (2.5x)** | 0.33 (6.6x) | **0.45 (4.9x)** | 0.24 (9.3x) | **0.35 (6.4x)** |
| `small_rect` | 1.65 / 1.67 | 0.64 (2.6x) | **0.76 (2.2x)** | 0.30 (5.4x) | **0.41 (4.1x)** | 0.21 (7.9x) | **0.32 (5.2x)** |
| `glow_inside` | 0.70 / 0.70 | 0.38 (1.8x) | **0.57 (1.2x)** | 0.27 (2.6x) | **0.44 (1.6x)** | 0.23 (3.0x) | **0.40 (1.8x)** |
| `card_outside` | 2.15 / 2.16 | 0.79 (2.7x) | **0.95 (2.3x)** | 0.38 (5.6x) | **0.54 (4.0x)** | 0.26 (8.2x) | **0.45 (4.8x)** |
| `bounded_band` | 0.19 / 0.19 | 0.28 (0.7x) | **0.45 (0.4x)** | 0.26 (0.7x) | **0.43 (0.4x)** | 0.24 (0.8x) | **0.38 (0.5x)** |
| `arcs` | 2.77 / 2.79 | 0.90 (3.1x) | **1.10 (2.5x)** | 0.37 (7.5x) | **0.57 (4.9x)** | 0.25 (11.1x) | **0.49 (5.7x)** |
| `segments` | 2.77 / 2.78 | 0.91 (3.0x) | **1.13 (2.5x)** | 0.37 (7.5x) | **0.55 (5.0x)** | 0.25 (11.1x) | **0.46 (6.1x)** |
| `overdrive` | 2.67 / 2.71 | 0.90 (3.0x) | **1.10 (2.5x)** | 0.37 (7.3x) | **0.54 (5.0x)** | 0.25 (10.8x) | **0.44 (6.1x)** |

**1920 x 1080**

| scene | 1.0 before / after | 0.5 before | **0.5 after** | 0.25 before | **0.25 after** | 0.125 before | **0.125 after** |
| ----- | ------------------ | ---------- | ------------- | ----------- | -------------- | ------------ | --------------- |
| `default` | 4.94 / 4.98 | 1.57 (3.1x) | **1.83 (2.7x)** | 0.59 (8.3x) | **0.83 (6.0x)** | 0.35 (14.1x) | **0.63 (7.9x)** |
| `hairline` | 3.25 / 3.28 | 1.10 (3.0x) | **1.30 (2.5x)** | 0.48 (6.7x) | **0.70 (4.7x)** | 0.34 (9.7x) | **0.61 (5.4x)** |
| `crisp_tube` | 4.08 / 4.12 | 1.34 (3.0x) | **1.64 (2.5x)** | 0.53 (7.7x) | **0.77 (5.3x)** | 0.32 (12.7x) | **0.62 (6.6x)** |
| `soft_wash` | 5.97 / 6.03 | 1.84 (3.2x) | **2.14 (2.8x)** | 0.66 (9.0x) | **0.96 (6.3x)** | 0.34 (17.4x) | **0.67 (9.0x)** |
| `sharp_corners` | 4.09 / 4.10 | 1.34 (3.0x) | **1.50 (2.7x)** | 0.49 (8.3x) | **0.64 (6.4x)** | 0.32 (12.9x) | **0.49 (8.4x)** |
| `small_rect` | 1.93 / 1.95 | 0.76 (2.5x) | **0.93 (2.1x)** | 0.41 (4.7x) | **0.53 (3.7x)** | 0.29 (6.6x) | **0.42 (4.6x)** |
| `glow_inside` | 1.55 / 1.56 | 0.66 (2.3x) | **0.90 (1.7x)** | 0.37 (4.2x) | **0.58 (2.7x)** | 0.28 (5.6x) | **0.50 (3.1x)** |
| `card_outside` | 4.69 / 4.73 | 1.57 (3.0x) | **1.76 (2.7x)** | 0.61 (7.6x) | **0.81 (5.8x)** | 0.39 (12.1x) | **0.60 (7.8x)** |
| `bounded_band` | 0.28 / 0.29 | 0.39 (0.7x) | **0.62 (0.5x)** | 0.29 (1.0x) | **0.58 (0.5x)** | 0.29 (1.0x) | **0.52 (0.6x)** |
| `arcs` | 5.15 / 5.18 | 1.64 (3.1x) | **1.90 (2.7x)** | 0.61 (8.5x) | **0.87 (6.0x)** | 0.33 (15.7x) | **0.67 (7.7x)** |
| `segments` | 5.18 / 5.18 | 1.64 (3.2x) | **1.94 (2.7x)** | 0.60 (8.7x) | **0.88 (5.9x)** | 0.34 (15.3x) | **0.68 (7.6x)** |
| `overdrive` | 5.98 / 6.03 | 1.83 (3.3x) | **2.12 (2.9x)** | 0.64 (9.3x) | **0.93 (6.5x)** | 0.35 (17.2x) | **0.65 (9.2x)** |

Added time, after - before, us (the `segments` 0.75 figure is the bimodality
above, not a saving):

**1280 x 720**

| scene | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ----- | --- | --- | --- | --- | --- | --- |
| `default` | +26 | +201 | +135 | +154 | +153 | +246 |
| `hairline` | +22 | +172 | +200 | +174 | +188 | +210 |
| `crisp_tube` | +30 | +212 | +187 | +158 | +196 | +207 |
| `soft_wash` | +27 | +240 | +234 | +182 | +223 | +211 |
| `sharp_corners` | +10 | +141 | +144 | +127 | +116 | +110 |
| `small_rect` | +18 | +133 | +116 | +103 | +102 | +113 |
| `glow_inside` | +9 | +197 | +193 | +161 | +169 | +163 |
| `card_outside` | +18 | +108 | +160 | +147 | +156 | +192 |
| `bounded_band` | +2 | +177 | +172 | +188 | +167 | +137 |
| `arcs` | +20 | +157 | +200 | +200 | +201 | +237 |
| `segments` | +1 | -187 | +218 | +181 | +185 | +206 |
| `overdrive` | +33 | +207 | +199 | +145 | +176 | +195 |

**1920 x 1080**

| scene | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ----- | --- | --- | --- | --- | --- | --- |
| `default` | +44 | +257 | +260 | +205 | +240 | +277 |
| `hairline` | +32 | +229 | +202 | +236 | +221 | +275 |
| `crisp_tube` | +41 | +290 | +294 | +259 | +243 | +303 |
| `soft_wash` | +56 | +343 | +299 | +302 | +303 | +331 |
| `sharp_corners` | +10 | +173 | +159 | +134 | +148 | +173 |
| `small_rect` | +20 | +183 | +175 | +135 | +117 | +129 |
| `glow_inside` | +15 | +239 | +240 | +230 | +205 | +223 |
| `card_outside` | +40 | +221 | +192 | +185 | +197 | +215 |
| `bounded_band` | +5 | +269 | +231 | +206 | +289 | +230 |
| `arcs` | +33 | +293 | +268 | +264 | +260 | +346 |
| `segments` | +5 | +339 | +298 | +265 | +284 | +345 |
| `overdrive` | +58 | +296 | +284 | +280 | +285 | +306 |

The added time is close to a flat charge per scale: +0.10 to +0.25 ms at 720p
and +0.12 to +0.35 ms at 1080p, whatever the scene costs at 1.0. That is why
the cheap scenes suffer most in ratio. A full ring around a big glow
(`soft_wash`, `overdrive`) pays the most in absolute terms and still keeps
4.5-6.5x at 0.25; the cutoff band, which costs 0.19 ms at 1.0, cannot.

## 4. Where it comes from

Median change against the previous build over the twelve scenes, us, with the
range across scenes. Rounds 1-3. The extreme ranges in the 1280 x 720 0.75
column (-384 to +376) are the bimodality in section 1, which flips steps 1 to 4
against each other on a few scenes; the medians are unaffected.

**1280 x 720**

| build | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ----- | --- | --- | --- | --- | --- | --- |
| step 1 `546d2b7` | +1 (-4 to +9) | +3 (-11 to +193) | +10 (-6 to +32) | +2 (-35 to +30) | +3 (-14 to +17) | +4 (-25 to +48) |
| step 2 `45bd1f0` | +15 (+2 to +26) | +8 (-163 to +376) | -3 (-30 to +19) | +4 (-33 to +34) | +4 (-13 to +17) | +4 (-34 to +29) |
| step 3 `c155a37` | -3 (-10 to +9) | -9 (-384 to +38) | -6 (-18 to +30) | -2 (-31 to +32) | -4 (-19 to +15) | -5 (-25 to +5) |
| step 4 `8a50121` | +2 (-10 to +8) | +56 (-349 to +98) | +38 (+13 to +60) | +27 (-9 to +40) | +21 (-4 to +37) | +21 (-2 to +70) |
| step 5 `ea62200` | +2 (-3 to +14) | +135 (+78 to +193) | +140 (+73 to +186) | +156 (+64 to +210) | +153 (+98 to +210) | +181 (+113 to +248) |
| after `db2c250` | +1 (-5 to +6) | +2 (-10 to +67) | -0 (-47 to +13) | -1 (-11 to +23) | -4 (-28 to +10) | +3 (-13 to +21) |

**1920 x 1080**

| build | 1.0 | 0.75 | 0.5 | 0.35 | 0.25 | 0.125 |
| ----- | --- | --- | --- | --- | --- | --- |
| step 1 `546d2b7` | +2 (-5 to +12) | +7 (-12 to +45) | +6 (-20 to +50) | +10 (-8 to +59) | +6 (-9 to +44) | +6 (-11 to +29) |
| step 2 `45bd1f0` | +24 (+2 to +56) | +3 (-20 to +42) | +5 (-9 to +79) | -3 (-23 to +23) | +6 (-16 to +22) | -0 (-11 to +26) |
| step 3 `c155a37` | -1 (-9 to +6) | +2 (-10 to +10) | +0 (-15 to +13) | +4 (-10 to +13) | -2 (-22 to +25) | -3 (-29 to +23) |
| step 4 `8a50121` | -0 (-5 to +15) | +64 (+41 to +145) | +42 (+27 to +100) | +29 (-4 to +47) | +20 (-10 to +58) | +16 (-13 to +55) |
| step 5 `ea62200` | +1 (-15 to +6) | +184 (+91 to +278) | +184 (+91 to +250) | +197 (+97 to +252) | +208 (+110 to +263) | +255 (+124 to +293) |
| after `db2c250` | +0 (-3 to +9) | +0 (-16 to +8) | +2 (-13 to +23) | +2 (-21 to +16) | +1 (-24 to +55) | -5 (-16 to +32) |

- **Step 5, the edge ring, is about three quarters of the cost.** +135 to
  +255 us median, at every reduced scale and on every scene. It is one more
  full-resolution pass - program switch, uniforms, the blit drawn as a frame of
  quads instead of one - plus the ring's fragments. A perimeter sweep on this
  GPU (plan, section 7) put it at about 75 us that does not move with the rect
  plus about 30 us per 1,000 px of perimeter at 0.5, 720p. It grows as the scale
  falls because the ring widens: its half-width carries `RING_GUARD_TEXELS /
  scale` and the reduced pass's filament reach, so the default scene's ring is
  9 px at 0.5, 11 px at 0.25 and 22 px at 0.125. A soft filament
  (`filamentFalloff` below about 0.3) makes it far wider; the plan measured the
  scaled path's gain falling to 1.5x / 2.2x at 0.5 / 0.25 there.
- **Step 4, the gather target, is most of the rest.** +16 to +64 us, largest at
  0.75 and smallest at 0.125: writing and clearing one or two more RGBA8
  attachments at the buffer's size. On the M2 Pro this step measured no change,
  and step 5 measured +35 to +48 us; this GPU prices both higher.
- **Step 2 costs scale 1.0 alone**, section 2. Below 1.0 it is within noise:
  the blit's cutoff code runs only when a cutoff is live, and only the band
  scene has one.
- **Steps 1 and 3 and the `invariant` commit cost nothing measurable.**

## 5. Startup and memory

Construct + `Initialize()` + adding the neon renderer, per effect - median and
minimum over every effect a build's runs created (216 per build, 360 for before
and after):

| build | 1280 x 720 median / min | 1920 x 1080 median / min |
| ----- | ----------------------- | ------------------------ |
| before `542dad4` | 23.4 / 16.5 ms | 23.5 / 16.7 ms |
| step 1 `546d2b7` | 23.5 / 20.9 ms | 23.6 / 17.9 ms |
| step 2 `45bd1f0` | 24.3 / 20.8 ms | 24.3 / 18.9 ms |
| step 3 `c155a37` | 24.1 / 17.7 ms | 24.1 / 18.4 ms |
| step 4 `8a50121` | 38.2 / 33.1 ms | 38.4 / 32.4 ms |
| step 5 `ea62200` | 49.9 / 38.4 ms | 50.4 / 40.9 ms |
| after `db2c250` | 50.5 / 40.5 ms | 50.3 / 40.5 ms |

Steps 4 and 5 each add one compile of `neon.frag` (the `NEON_WRITES_GATHER`
and `NEON_RING_PASS` variants): +14 ms and +12 ms, **23 ms to 50 ms per effect**
on this machine. They were built eagerly at `Initialize` so that the first
switch to a reduced scale would not stall. They are now built on first use and
per path, which was decision 6 in the plan (taken, plan section 12.2). A host
at 1.0 compiles one `neon.frag` and no blit; a host below 1.0 compiles the two
variants and the blit, and not the plain program. The table above is before
that change. Building fewer was measured too (the plan's section 7):
folding the ring into the plain program takes this to 39.3 ms with output and
frame time unchanged, and a single program for all three draws is not viable -
93-624x slower below 1.0 on this GPU.

The scaled buffer gains one RGBA8 gather attachment, or two when the config has
segments, at the reduced size. Nothing is allocated at scale 1.0, before or
after.

| frame | scale | before | after, no segments | after, segments |
| ----- | ----- | ------ | ------------------ | --------------- |
| 1280 x 720 | 0.5 | 0.92 MB | 1.84 MB | 2.76 MB |
| 1920 x 1080 | 0.5 | 2.07 MB | 4.15 MB | 6.22 MB |
| 1920 x 1080 | 0.25 | 0.52 MB | 1.04 MB | 1.56 MB |
| 3840 x 2160 | 0.5 | 8.29 MB | 16.59 MB | 24.88 MB |

The ring and the blit's partition add two vertex buffers of under 250 bytes
each.

Since the plan's section 13 the gather attachments are gone. The reduced buffer
is one RGBA8 attachment again, now covering only what the blit reads (never more
than the whole reduced viewport), and the gather has an RGBA16F buffer of its
own at its own coarse scale, over the glow. Measured at 1920 x 1080 with
section 8's harness:

| rect | scale | before section 13 | after, no segments | after, segments |
| ---- | ----- | ----------------- | ------------------ | --------------- |
| full screen | 0.5 | 4.15 MB | 2.15 MB | 2.23 MB |
| full screen | 0.25 | 1.04 MB | 0.60 MB | 0.68 MB |
| 900 x 540 | 0.5 | 4.15 MB | 1.80 MB | 1.91 MB |
| 300 x 200 | 0.5 | 4.15 MB | 1.23 MB | 1.63 MB |
| 120 x 80 | 0.5 | 4.15 MB | 1.70 MB | 2.83 MB |

## 6. What to take from it

- **Scale 1.0 is unaffected.** Hosts that never lower the scale used to pay
  the startup; since the programs are built per path on first use, they no
  longer do.
- **0.5 and 0.25 still pay off on any scene with a real glow** - 2.1x to 2.9x
  and 3.7x to 6.5x here - and they now look like 1.0 instead of a blurred copy
  of it.
- **Below 0.25 the gain flattens.** On the glow scenes 0.125 now saves 8-23%
  over 0.25 at 720p and 13-30% at 1080p, against 29-37% and 29-48% before - the
  ring is widest there and its fixed part dominates.
- **A cheap scene can lose.** The cutoff band was already slower below 1.0 on
  this GPU before the ring (0.7x at 0.5); it is now 0.4x to 0.6x. A one-sided
  glow is close to break-even. (Both measured before the plan's section 12,
  which cut the cutoff band's composite by 97% of its fragments and the
  one-sided glow's by 23-77%, and took 44% off the band's frame time and
  11-24% off the one-sided glows' at 0.25 on llvmpipe. Not re-measured on this
  GPU.) `NeonConfig::resolutionScale`'s documentation
  says so (review finding I25).
- **Measure on the device.** The two GPUs measured so far price the ring 4-5x
  apart on the default scene (+35 to +48 us on the M2 Pro, +132 to +228 us
  here). Whether "always on below 1.0" (the plan's decision 1) is right for the
  target depends on which it resembles.

## 7. Reproducing

Build the tool in-tree and once standalone per older commit (see its README),
then run `time` for every build in rounds with the order rotated:

```bash
./build/tools/neon-scale-check/neon-scale-check time head_1280x720_r1.json --label head --size 1280x720
```

Take the median per figure over rounds, and compare builds only over rounds
they ran in together.

## 8. After the split gather

[`neon-resolution-scale-plan.md`](neon-resolution-scale-plan.md) section 13
took the gather loop out of the reduced pass: it now runs alone, at its own
coarse scale, and the reduced pass and the ring shade from its result. Measured
on Mesa llvmpipe at 1280 x 720 (not the GPU above, so read ratios), median of
three interleaved rounds against the commit before it (`da24f9c`), each scene a
fresh effect, best of 8 runs of 4 frames:

| scene | 1.0 | 0.5 | 0.25 |
| ----- | --- | --- | ---- |
| `default` | 1.01x | **3.85x** | **1.79x** |
| `hairline` | 1.00x | 3.43x | 1.88x |
| `crisp_tube` | 1.01x | 3.56x | 1.59x |
| `soft_wash` | 0.99x | 3.41x | 1.66x |
| `sharp_corners` | 0.99x | 4.23x | 1.73x |
| `small_rect` | 1.00x | 1.03x | 0.98x |
| `glow_inside` | 1.00x | 2.38x | 1.10x |
| `card_outside` | 1.00x | 3.61x | 1.64x |
| `bounded_band` | 0.99x | 1.68x | 0.87x |
| `arcs` | 0.98x | 3.51x | 1.77x |
| `segments` | 1.00x | 4.31x | 1.93x |
| `overdrive` | 0.98x | 3.67x | 1.81x |

(Speed-up: before / after. 1.0 is untouched and reads within the noise.) The
default scene now takes 9.9 ms at 0.5 and 7.0 ms at 0.25 against 134 ms at 1.0;
`main` on the same machine takes 36.5 ms and 11.5 ms against 133. Init plus the
first frame, median over the twelve scenes: 80.5 ms before, 73.0 ms after.

Where it does not pay: `small_rect` gathers at the reduced scale itself (its
colour kernel is too narrow for a coarser grid), so it gains nothing; and
`bounded_band`, already cheap, pays the extra pass's fixed cost - about even
with the commit before at 0.25, within a 10% spread run to run. Per pass at 0.5
on `default` (skipping one pass at a time, so approximate): the shading pass at
the reduced scale is now the largest, ~40-50%, then the gather ~25-35%, the
blit ~20% and the ring ~15%.


## 9. Against `main` at scale 0.5

Sections 1-8 measure the branch's steps against each other. This one measures
where the branch ended up against `main` (`1b5cf94`, its merge base): the
library at `d3b671b` against `main`'s, both Release builds, at resolution scale
0.5, on an **Apple M2 Pro** (arm64, macOS GL 4.1).

What each does at 0.5:

- **`main`** shades the whole glow in one pass at half resolution - the
  128-sample loop at every reduced pixel, the line included - and blits all of
  it back with one bilinear upsample.
- **The branch** runs the loop alone on a coarse grid (section 8), shades the
  rest at 0.5, and re-shades the line and every hard edge at full resolution in
  a thin ring (sections 1-5). Each buffer covers only the part of the frame
  that can be lit.

**Method.** As in section 1: `tools/neon-scale-check`'s `time`, built in-tree
against the branch's library and standalone against `main`'s (`EL_ROOT`), five
rounds per build at each size with the build order alternating, median per
figure. Milliseconds per frame, neon layer only.

| scene | 720p `main` | 720p current | speed-up | 1080p `main` | 1080p current | speed-up |
| ----- | ----------- | ------------ | -------- | ------------ | ------------- | -------- |
| `default` | 0.618 | 0.242 | **2.5x** | 1.171 | 0.394 | **3.0x** |
| `hairline` | 0.523 | 0.206 | 2.5x | 0.855 | 0.294 | 2.9x |
| `crisp_tube` | 0.599 | 0.240 | 2.5x | 1.041 | 0.350 | 3.0x |
| `soft_wash` | 0.690 | 0.275 | 2.5x | 1.392 | 0.471 | 3.0x |
| `sharp_corners` | 0.617 | 0.182 | 3.4x | 1.145 | 0.262 | 4.4x |
| `small_rect` | 0.455 | 0.358 | 1.3x | 0.522 | 0.265 | 2.0x |
| `glow_inside` | 0.275 | 0.156 | 1.8x | 0.485 | 0.204 | 2.4x |
| `card_outside` | 0.569 | 0.219 | 2.6x | 1.187 | 0.379 | 3.1x |
| `bounded_band` | 0.125 | 0.112 | 1.1x | 0.172 | 0.121 | 1.4x |
| `arcs` | 0.696 | 0.267 | 2.6x | 1.253 | 0.439 | 2.9x |
| `segments` | 0.872 | 0.300 | 2.9x | 1.622 | 0.427 | 3.8x |
| `overdrive` | 0.695 | 0.263 | 2.6x | 1.440 | 0.457 | 3.2x |
| geometric mean | | | **2.3x** | | | **2.8x** |

(Speed-up: `main` / current.) Two checks on the numbers:

- **Scale 1.0 is the control, and it holds.** The branch runs at 0.98x to 1.13x
  of `main`'s speed at 1.0 across the twelve scenes and both sizes. For all but
  one figure the two builds' five-round ranges overlap, so the difference is
  noise; the exception is `segments` at 720p, about 11% faster with no overlap.
  Either way it is far smaller than the gains at 0.5, which come from the
  reduced-scale path, not from the machine.
- **The noise is small.** A figure at 0.5 typically varied 3-10% across its
  five rounds, 15% at worst. Even the smallest gain, `bounded_band` at 720p
  (1.1x), is real: the two builds' ranges there do not overlap.

Against each build's own 1.0, `main`'s 0.5 is 1.4x to 4.1x faster; the
branch's is 1.6x to 12.0x at 720p and 1.9x to 15.9x at 1080p. `bounded_band`
is the low end of every range.

**Quality at 0.5.** `neon-scale-check check`, each build against its own 1.0
render, max / p99 error out of 255:

| scene | `main` at 0.5 | current at 0.5 |
| ----- | ------------- | -------------- |
| `default` | 7 / 4 | 1 / 1 |
| `hairline` | 77 / 59 | 2 / 1 |
| `crisp_tube` | 41 / 25 | 1 / 1 |
| `soft_wash` | 1 / 1 | 1 / 1 |
| `sharp_corners` | 13 / 4 | 1 / 1 |
| `small_rect` | 11 / 8 | 1 / 1 |
| `glow_inside` | 4 / 3 | 1 / 1 |
| `card_outside` | 5 / 2 | 1 / 1 |
| `bounded_band` | 42 / 21 | 1 / 1 |
| `arcs` | 7 / 4 | 2 / 1 |
| `segments` | 8 / 6 | 1 / 1 |
| `overdrive` | 1 / 1 | 1 / 1 |

The branch passes. `main` fails 55 values across the scales, and its moving
hairline drifts 0.15 px at 0.5 (the bound is 0.1) and 1.75 px at 0.125,
against 0.05 and 0.02 now. `main`'s worst scenes are the ones the edge ring was
built for: a 1 px line (`hairline`, 77/255), a flat-topped tube (`crisp_tube`,
41) and a hard cutoff band (`bounded_band`, 42).

**Reading it.**

- **The gain grows with the frame,** 2.3x at 720p and 2.8x at 1080p, where
  `time` draws the scenes' rects larger. The branch's loop grid is sized by the
  colour kernel, which grows with the rect, so its texel count stays about the
  same as the rect grows; `main` runs the loop at every reduced pixel, so its
  cost grows with the area. The ring grows only with the perimeter.
- **The smallest gains are the cheap scenes.** `bounded_band` (1.1x, 1.4x)
  lights only a thin band, so the passes' fixed cost dominates. `small_rect`
  (1.3x, 2.0x) has a colour kernel of about 4 px at 720p, so its gather grid
  comes out at scale 0.48, almost the reduced scale itself (section 8); at
  1080p the rect is 1.5x larger, the grid drops to 0.32, and the gain to 2.0x.
  `glow_inside` (1.8x, 2.4x) glows inside the rect only, so it is cheap even at
  1.0 (0.6 ms against 2.3 ms for `default` at 720p) and has less to gain.
- **`bounded_band` is not quite like for like.** `Cutoff::softness` is centred
  on `size` on `main` and runs outward from it on the branch
  ([`upgrade-notes.md`](upgrade-notes.md)), so the lit band itself differs a
  little; `main`'s 1.0 render of it is 131/255 off the committed image. Read
  that row as indicative.
- **Startup is not compared.** `time` also records effect construction, 12-15
  ms on `main` against 3.5 ms on the branch, but the branch builds the neon
  programs on the first frame instead (section 6). That number shows where the
  compile happens, not what it costs.
- **One GPU.** The AMD GPU (sections 1-7) and llvmpipe (section 8) priced the
  ring and the gather split differently again. Measure on the target device.

To reproduce, with `main` exported and built in its own tree:

```bash
mkdir -p /tmp/el-main
git archive main | tar -x -C /tmp/el-main
cmake -S /tmp/el-main -B /tmp/el-main/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/el-main/build --target edge-lighting glad
cmake -S tools/neon-scale-check -B build/neon-scale-check-main -G Ninja -DEL_ROOT=/tmp/el-main
cmake --build build/neon-scale-check-main
```

then `time` both builds in alternating rounds at each size (section 7), and
`check` each - `main`'s with `--images-dir docs/images/neon-resolution-scale`.

## 10. After V19, V20 and V21: the glow coverage table

V19 and V20 in [`review-findings.md`](review-findings.md) changed what scales
the halo and bloom on a partly lit ring: each of the outline's eight pieces
now reads its own coverage from a table (`neon-glow-cover.frag`, pass 0b)
baked once per config change, instead of every piece sharing the coverage
gathered around the pixel. Every timing in sections 1-9 predates it. Measured
on an Apple M2 Pro with `neon-scale-check time`, the build before V19 against
the build after V20, interleaved rounds (five at 1280 x 720, three at 1920 x
1080), median per figure, as cost after / before:

| scenes | 720p, 1.0 | 720p, 0.5 | 1080p, 1.0 | 1080p, 0.5 |
| ------ | --------- | --------- | ---------- | ---------- |
| fully lit (10 scenes) | 1.01x-1.05x, ~1.04x | 1.00x-1.10x, ~1.08x | 1.01x-1.05x, ~1.03x | 1.04x-1.10x, ~1.08x |
| `arcs` | 1.11x | 1.20x | 1.09x | 1.15x |
| `segments` | 1.09x | 1.18x | 1.08x | 1.18x |

A fully lit ring skips the per-piece read entirely and renders bit-identically,
so what it pays is the code being there. The scaled path pays a little more in
proportion because its shading passes are cheap - the gather, which dominates
at 1.0, is untouched.

`time` holds the config still, so the table bakes once per effect. **Under an
animation that changes the config every frame it re-bakes every frame**: on a
640 x 360 rect at 1280 x 720 with three arcs and two segments, the extra a
frame costs over the same animation before V19 is about 0.1 ms when an arc
animates and 0.2-0.25 ms when a segment does at scale 0.5 (a whole neon frame
there is 0.36 ms), and 0.3-0.6 ms when a segment does at 1.0 (2.2 ms). The
table is 2048 x 128 texels; segments are integrated numerically per texel and
arcs in closed form, which is the difference. This is the figure to measure
on a target GPU before shipping an animated segment.

Memory: the table is 2 MB in RGBA16F (1 MB in the RGBA8 fallback), allocated
once at `Initialize` whatever the scale.

**V21** replaced that table with one per piece of the outline, so no piece's
coverage runs on past its own ends: 1024 x 192, each band shared by a straight
and a corner in proportion to their lengths, 1.5 MB in RGBA16F (0.75 MB in
RGBA8) - less than the table it replaced. Measured on an AMD Radeon Pro 5300M -
not the M2 above, so read the two tables as ratios, never against each other -
the build after V21 against the build before it, four interleaved rounds with
the order alternating, median per figure, as cost after / before:

| scenes | 720p, 1.0 | 720p, 0.5 | 1080p, 1.0 | 1080p, 0.5 |
| ------ | --------- | --------- | ---------- | ---------- |
| fully lit (10 scenes) | 0.99x-1.01x, ~1.01x | 0.90x-0.99x, ~0.94x | 1.00x-1.01x, ~1.00x | 0.86x-1.01x, ~0.95x |
| `arcs` | 1.01x | 0.94x | 1.01x | 0.96x |
| `segments` | 1.00x | 0.94x | 1.00x | 0.98x |

The table's read is laid out so it costs a fully lit ring nothing; the first
build, whose read took an atan and logs and developed all four corners before
reading any, was 1.11x-1.15x on fully lit scenes at 1.0 (V21 has the
attribution). Under animation, on the AMD at 1280 x 720, a frame whose config
changes costs 0.16 ms in all with one arc animating (0.17 before V21) and
0.34 ms with three arcs and two segments (0.57) at 1.0, 0.32 ms (0.53) at 0.5;
on a ring lit uniformly the pass no longer runs at all, so an intensity
animation there costs 0.04 ms against 0.10. `Initialize` no longer compiles
the bake - it is built on the first frame that needs one - and drops from
~8.7 ms to ~4.5 ms.
