#include "figures.h"

#include "canvas.h"
#include "pass-recorder.h"

#include "renderer/neon-tuning.h"
#include "util/capture-util.h"
#include "util/geometry-utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace NeonGuideFigures
{
    using namespace NeonTools;

    namespace
    {
        // ---------------------------------------------------------------
        // Scenes
        // ---------------------------------------------------------------

        /// The clear every render sits on: rgb(5, 5, 8), as in neon-scale-check.
        const glm::vec3 CLEAR(5.0f / 255.0f, 5.0f / 255.0f, 8.0f / 255.0f);

        /// The wide frame: an 800 x 450 view of a 560 x 280 rect.
        const int WIDE_W = 800;
        const int WIDE_H = 450;
        /// The panel frame, for figures shown three to a row.
        const int PANEL_W = 480;
        const int PANEL_H = 270;
        /// The pass figures' frame.
        const int PASS_W = 640;
        const int PASS_H = 360;

        const glm::vec3 CYAN(0.15f, 0.8f, 1.0f);
        const glm::vec3 ORANGE(1.0f, 0.55f, 0.1f);

        ColorStop Stop(float position, const glm::vec3 &rgb, float alpha = 1.0f)
        {
            return ColorStop{position, glm::vec4(rgb, alpha)};
        }

        /// Three stops far enough apart in hue that a seam between them shows.
        std::vector<ColorStop> ShowcaseStops()
        {
            return {Stop(0.0f, glm::vec3(0.1f, 0.85f, 1.0f)), Stop(0.33f, glm::vec3(0.7f, 0.35f, 1.0f)),
                    Stop(0.66f, glm::vec3(1.0f, 0.3f, 0.55f))};
        }

        /// A rect of @p rectW x @p rectH centred in a @p frameW x @p frameH
        /// frame, the neon on, everything time-dependent frozen and the debug
        /// layer quiet - so a render depends on the config alone.
        Config Scene(int frameW, int frameH, float rectW, float rectH, float radius)
        {
            Config c;
            c.geometry.width = rectW;
            c.geometry.height = rectH;
            c.geometry.cornerRadius = radius;
            c.geometry.position = glm::vec2((frameW - rectW) * 0.5f, (frameH - rectH) * 0.5f);
            c.neon.enable = true;
            c.neon.hueRotationRate = 0.0f;
            c.neon.colorTransitionDuration = 0.0f;
            c.debug.showWireframe = false;
            return c;
        }

        Config WideScene() { return Scene(WIDE_W, WIDE_H, 560.0f, 280.0f, 48.0f); }
        Config PanelScene() { return Scene(PANEL_W, PANEL_H, 336.0f, 168.0f, 32.0f); }
        Config PassScene() { return Scene(PASS_W, PASS_H, 440.0f, 220.0f, 40.0f); }

        /// Clear to the figure background, or lay a grey checkerboard - which
        /// is what shows where the opaque fill paints.
        void ClearBackground(int w, int h, bool checker)
        {
            glClearColor(CLEAR.r, CLEAR.g, CLEAR.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!checker)
            {
                return;
            }
            const int cell = 18;
            glEnable(GL_SCISSOR_TEST);
            for (int y = 0; y < h; y += cell)
            {
                for (int x = 0; x < w; x += cell)
                {
                    const bool light = ((x / cell) + (y / cell)) % 2 == 0;
                    const float v = light ? 0.42f : 0.30f;
                    glScissor(x, y, cell, cell);
                    glClearColor(v, v, v, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
            }
            glDisable(GL_SCISSOR_TEST);
        }

        /// One frame of @p config at @p w x @p h, read back. With @p record,
        /// PassRecorder sees every draw of it.
        Canvas Render(EdgeLightingEffect &effect, const Config &config, int w, int h, bool checker = false,
                      bool record = false)
        {
            effect.SetConfig(config);
            effect.Update(0.0f);
            OffscreenCapture capture;
            capture.Begin(w, h);
            ClearBackground(w, h, checker);
            if (record)
            {
                PassRecorder::Begin();
            }
            effect.Render(w, h);
            if (record)
            {
                PassRecorder::End();
            }
            CaptureUtil::Image image;
            capture.Read(image);
            capture.End();
            return FromImage(image);
        }

        /// A neon-only effect of its own, so its first frame runs every pass -
        /// the emission pre-pass is skipped on a frame whose config did not
        /// change, and a recorded frame has to contain it.
        std::unique_ptr<EdgeLightingEffect> FreshEffect()
        {
            std::unique_ptr<EdgeLightingEffect> effect(new EdgeLightingEffect());
            effect->Initialize();
            effect->AddRenderer(RendererLayer::NEON);
            return effect;
        }

        std::string Path(const std::string &dir, const char *name)
        {
            return dir + "/" + name;
        }

        // ---------------------------------------------------------------
        // The shaders' own geometry, on the CPU
        // ---------------------------------------------------------------

        /// sdRoundBox, as every neon shader has it.
        float SdRoundBox(const glm::vec2 &p, const glm::vec2 &b, float r)
        {
            const glm::vec2 q = glm::abs(p) - b + glm::vec2(r);
            return std::min(std::max(q.x, q.y), 0.0f) + glm::length(glm::max(q, glm::vec2(0.0f))) - r;
        }

        /// neon.frag's perimeterPosition, line for line: the perimeter
        /// fraction of @p p's nearest outline point. @p p is rect-local, +y up.
        float PerimeterPosition(const glm::vec2 &p, float w, float h, float radius, bool clockwise)
        {
            const float pi = 3.14159265358979f;
            const float halfPi = 0.5f * pi;
            const float halfW = w * 0.5f;
            const float halfH = h * 0.5f;
            const float r = std::clamp(radius, 0.0f, std::min(halfW, halfH));
            const float halfWs = halfW - r;
            const float halfHs = halfH - r;
            const float ws = w - 2.0f * r;
            const float hs = h - 2.0f * r;
            const float arcLen = pi * r * 0.5f;
            const float peri = 2.0f * ws + 2.0f * hs + 4.0f * arcLen;

            const glm::vec2 b(halfWs, halfHs);
            const glm::vec2 c = glm::clamp(p, -b, b);
            const glm::vec2 d = p - c;
            const float dl = glm::length(d);
            glm::vec2 cp;
            if (dl > 1e-6f)
            {
                cp = c + d * (r / dl);
            }
            else
            {
                const glm::vec2 e = b - glm::abs(p);
                const float sx = (p.x >= 0.0f) ? 1.0f : -1.0f;
                const float sy = (p.y >= 0.0f) ? 1.0f : -1.0f;
                cp = (e.x < e.y) ? glm::vec2(sx * halfW, p.y) : glm::vec2(p.x, sy * halfH);
            }

            const float ax = std::fabs(cp.x);
            const float ay = std::fabs(cp.y);
            int seg = 0;
            float u = 0.0f;
            if (ax > halfWs && ay > halfHs)
            {
                const float sx = (cp.x >= 0.0f) ? 1.0f : -1.0f;
                const float sy = (cp.y >= 0.0f) ? 1.0f : -1.0f;
                float th = std::atan2(cp.y - sy * halfHs, cp.x - sx * halfWs);
                if (sx > 0.0f && sy > 0.0f)
                {
                    seg = 1;
                    u = (halfPi - th) / halfPi;
                }
                else if (sx > 0.0f)
                {
                    seg = 3;
                    u = -th / halfPi;
                }
                else if (sy < 0.0f)
                {
                    seg = 5;
                    if (th > 0.0f)
                    {
                        th -= 2.0f * pi;
                    }
                    u = (-halfPi - th) / halfPi;
                }
                else
                {
                    seg = 7;
                    u = (pi - th) / halfPi;
                }
            }
            else if (ay >= halfHs)
            {
                seg = cp.y > 0.0f ? 0 : 4;
                u = cp.y > 0.0f ? (cp.x + halfWs) / ws : (halfWs - cp.x) / ws;
            }
            else if (ax >= halfWs)
            {
                seg = cp.x > 0.0f ? 2 : 6;
                u = cp.x > 0.0f ? (halfHs - cp.y) / hs : (cp.y + halfHs) / hs;
            }

            float base = 0.0f;
            float len = arcLen;
            if (clockwise)
            {
                const float bases[8] = {0.0f, ws, ws + arcLen, ws + arcLen + hs, ws + 2.0f * arcLen + hs,
                                        2.0f * ws + 2.0f * arcLen + hs, 2.0f * ws + 3.0f * arcLen + hs,
                                        peri - arcLen};
                const float lens[8] = {ws, arcLen, hs, arcLen, ws, arcLen, hs, arcLen};
                base = bases[seg];
                len = lens[seg];
                return (base + len * u) / peri;
            }
            const float bases[8] = {2.0f * hs + 3.0f * arcLen + ws, 2.0f * hs + 2.0f * arcLen + ws,
                                    hs + 2.0f * arcLen + ws, hs + arcLen + ws, hs + arcLen, hs, 0.0f,
                                    2.0f * hs + 3.0f * arcLen + 2.0f * ws};
            const float lens[8] = {ws, arcLen, hs, arcLen, ws, arcLen, hs, arcLen};
            base = bases[seg];
            len = lens[seg];
            return (base + len * (1.0f - u)) / peri;
        }

        /// neon-common.glsl's rectPerimeter.
        float RectPerimeter(float w, float h, float radius)
        {
            const float r = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
            return 2.0f * (w + h - 4.0f * r) + 6.28318530717959f * r;
        }

        /// The gradient ring at @p t, blended in RGB the way GradientRingLUT
        /// bakes it for BlendSpace::RGB: sorted stops, wrapping past 1.
        glm::vec3 GradientAt(std::vector<ColorStop> stops, float t)
        {
            std::sort(stops.begin(), stops.end(),
                      [](const ColorStop &a, const ColorStop &b) { return a.position < b.position; });
            t -= std::floor(t);
            if (stops.size() == 1)
            {
                return glm::vec3(stops[0].color);
            }
            for (size_t i = 0; i + 1 < stops.size(); ++i)
            {
                if (t >= stops[i].position && t < stops[i + 1].position)
                {
                    const float f = (t - stops[i].position) / (stops[i + 1].position - stops[i].position);
                    return glm::mix(glm::vec3(stops[i].color), glm::vec3(stops[i + 1].color), f);
                }
            }
            const ColorStop &last = stops.back();
            const ColorStop &first = stops.front();
            const float span = first.position + 1.0f - last.position;
            const float f = (t >= last.position ? t - last.position : t + 1.0f - last.position) / span;
            return glm::mix(glm::vec3(last.color), glm::vec3(first.color), f);
        }

        glm::vec3 Hsv(float h, float s, float v)
        {
            const glm::vec3 k = glm::abs(glm::fract(glm::vec3(h) + glm::vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f -
                                         glm::vec3(3.0f));
            return v * glm::mix(glm::vec3(1.0f), glm::clamp(k - glm::vec3(1.0f), 0.0f, 1.0f), s);
        }

        /// Rect-local (+y up, origin at the rect centre) to image px of a
        /// canvas whose rect is centred at (@p cx, @p cy).
        glm::vec2 ToImage(const glm::vec2 &local, float cx, float cy)
        {
            return glm::vec2(cx + local.x, cy - local.y);
        }

        void DrawOutline(Canvas &canvas, const RectGeometry &geom, float cx, float cy, const glm::vec3 &colour,
                         float width, float alpha)
        {
            const int steps = 600;
            glm::vec2 prev = ToImage(GeometryUtils::GetPointOnRectangle(0.0f, geom), cx, cy);
            for (int i = 1; i <= steps; ++i)
            {
                const glm::vec2 next = ToImage(GeometryUtils::GetPointOnRectangle(float(i) / steps, geom), cx, cy);
                DrawLine(canvas, prev, next, width, colour, alpha);
                prev = next;
            }
        }

        /// An arrow along the outline from perimeter fraction @p t0 to @p t1.
        void DrawPerimeterArrow(Canvas &canvas, const RectGeometry &geom, float cx, float cy, float t0, float t1,
                                const glm::vec3 &colour)
        {
            const int steps = 40;
            glm::vec2 prev = ToImage(GeometryUtils::GetPointOnRectangle(t0, geom), cx, cy);
            for (int i = 1; i <= steps; ++i)
            {
                const float t = t0 + (t1 - t0) * float(i) / steps;
                const glm::vec2 next = ToImage(GeometryUtils::GetPointOnRectangle(t, geom), cx, cy);
                DrawLine(canvas, prev, next, 3.0f, colour, 1.0f);
                prev = next;
            }
            const glm::vec2 tip = prev;
            const glm::vec2 back = ToImage(GeometryUtils::GetPointOnRectangle(t1 - 0.01f, geom), cx, cy);
            const glm::vec2 dir = glm::normalize(tip - back);
            const glm::vec2 side(-dir.y, dir.x);
            DrawLine(canvas, tip, tip - dir * 12.0f + side * 7.0f, 3.0f, colour, 1.0f);
            DrawLine(canvas, tip, tip - dir * 12.0f - side * 7.0f, 3.0f, colour, 1.0f);
            DrawDisc(canvas, ToImage(GeometryUtils::GetPointOnRectangle(t0, geom), cx, cy), 6.0f, colour, 1.0f);
        }

        // ---------------------------------------------------------------
        // Pass snapshots -> images
        // ---------------------------------------------------------------

        /// An attachment as an image, top row first, through @p map.
        template <typename Map>
        Canvas AttachmentImage(const Attachment &a, Map map)
        {
            Canvas out(a.width, a.height);
            for (int y = 0; y < a.height; ++y)
            {
                for (int x = 0; x < a.width; ++x)
                {
                    out.At(x, a.height - 1 - y) = map(a.At(x, y));
                }
            }
            return out;
        }

        /// Premultiplied colour composited over the figure background.
        glm::vec3 OverClear(const glm::vec4 &premultiplied)
        {
            return glm::vec3(premultiplied) + CLEAR * (1.0f - premultiplied.a);
        }

        /// The gather buffer stores coverage as e = c / (1 + c); this is the
        /// shader's decode.
        float DecodeCoverage(float e)
        {
            return e / std::max(1.0f - e, 1.0f / 255.0f);
        }

        glm::vec4 Bilinear(const Attachment &a, float u, float v)
        {
            const float fx = u * a.width - 0.5f;
            const float fy = v * a.height - 0.5f;
            const int x0 = int(std::floor(fx));
            const int y0 = int(std::floor(fy));
            const float tx = fx - x0;
            const float ty = fy - y0;
            auto texel = [&](int x, int y) {
                return a.At(std::clamp(x, 0, a.width - 1), std::clamp(y, 0, a.height - 1));
            };
            return glm::mix(glm::mix(texel(x0, y0), texel(x0 + 1, y0), tx),
                            glm::mix(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
        }

        /// What the blit alone would draw over the WHOLE frame: the reduced
        /// buffer, read through the blit's own uv map at every pixel - the
        /// line included, which in the real frame the ring redraws.
        Canvas NaiveUpscale(const Attachment &reduced, const DrawRecord &blit, const Config &config, int w, int h)
        {
            const float cx = config.geometry.position.x + config.geometry.width * 0.5f;
            const float cy = float(h) - config.geometry.position.y - config.geometry.height * 0.5f;
            Canvas out(w, h);
            for (int py = 0; py < h; ++py)
            {
                for (int px = 0; px < w; ++px)
                {
                    const glm::vec2 local(px + 0.5f - cx, float(h) - (py + 0.5f) - cy);
                    const glm::vec2 uv = local * blit.uvScale + blit.uvOffset;
                    out.At(px, py) = OverClear(Bilinear(reduced, uv.x, uv.y));
                }
            }
            return out;
        }

        /// Composite a premultiplied RGBA8 overlay over @p base.
        Canvas Composite(const Canvas &base, const CaptureUtil::Image &overlay)
        {
            Canvas out = base;
            for (int y = 0; y < base.height; ++y)
            {
                for (int x = 0; x < base.width; ++x)
                {
                    const unsigned char *p = &overlay.pixels[(size_t(y) * size_t(overlay.width) + size_t(x)) * 4];
                    const glm::vec3 rgb = glm::vec3(p[0], p[1], p[2]) / 255.0f;
                    const float a = p[3] / 255.0f;
                    out.At(x, y) = rgb + out.At(x, y) * (1.0f - a);
                }
            }
            return out;
        }

        typedef struct Layer
        {
            PassKind kind;
            glm::vec3 colour;
        } Layer;

        /// The triangles @p layers' passes drew, filled translucently and
        /// outlined, over a dimmed copy of the frame.
        Canvas GeometryOverlay(const Canvas &frame, const std::vector<Layer> &layers)
        {
            OffscreenCapture overlay;
            overlay.Begin(frame.width, frame.height);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            for (const Layer &layer : layers)
            {
                if (const DrawRecord *record = PassRecorder::Find(layer.kind))
                {
                    PassRecorder::Redraw(*record, glm::vec4(layer.colour * 0.35f, 0.35f), false);
                }
            }
            for (const Layer &layer : layers)
            {
                if (const DrawRecord *record = PassRecorder::Find(layer.kind))
                {
                    const glm::vec3 edge = glm::mix(layer.colour, glm::vec3(1.0f), 0.5f);
                    PassRecorder::Redraw(*record, glm::vec4(edge, 1.0f), true);
                }
            }
            CaptureUtil::Image image;
            overlay.Read(image);
            overlay.End();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            return Composite(Dim(frame, 0.45f), image);
        }

        /// @p frame where @p record's triangles cover a pixel, the figure
        /// background everywhere else: one pass's own output, cut out of a
        /// frame that other passes also drew into. Exact when the passes tile
        /// without overlap, as the blit and the ring do - the redraw goes
        /// through neon.vert, so it rasterises to the same pixels.
        Canvas OnlyWhereDrawn(const Canvas &frame, const DrawRecord &record)
        {
            OffscreenCapture mask;
            mask.Begin(frame.width, frame.height);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_BLEND);
            PassRecorder::Redraw(record, glm::vec4(1.0f), false);
            CaptureUtil::Image image;
            mask.Read(image);
            mask.End();
            Canvas out(frame.width, frame.height, CLEAR);
            for (int y = 0; y < frame.height; ++y)
            {
                for (int x = 0; x < frame.width; ++x)
                {
                    if (image.pixels[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + 3] > 0)
                    {
                        out.At(x, y) = frame.At(x, y);
                    }
                }
            }
            return out;
        }

        /// The magnification that takes @p width to about @p target px.
        int FitFactor(int width, int target)
        {
            return std::max(1, target / std::max(width, 1));
        }

        // ---------------------------------------------------------------
        // Plots
        // ---------------------------------------------------------------

        /// Rec. 709 luma down column @p x from row 0 to @p rows, plotted
        /// against the signed distance to an edge at row @p edgeY (outside
        /// positive - the rows above it).
        Series ColumnProfile(const Canvas &image, int x, int rows, float edgeY, const char *label,
                             const char *colour, bool dashed = false)
        {
            Series s;
            s.label = label;
            s.colour = colour;
            s.dashed = dashed;
            for (int y = 0; y < rows; ++y)
            {
                s.points.push_back(glm::vec2(edgeY - (y + 0.5f), Luma255(image.At(x, y))));
            }
            return s;
        }

        /// Luma along row @p y, columns @p x0 to @p x1, the row's brightest of
        /// it and the row below (the line can straddle two).
        Series RowProfile(const Canvas &image, int y, int x0, int x1, const char *label, const char *colour,
                          bool dashed = false)
        {
            Series s;
            s.label = label;
            s.colour = colour;
            s.dashed = dashed;
            for (int x = x0; x <= x1; ++x)
            {
                s.points.push_back(glm::vec2(float(x), std::max(Luma255(image.At(x, y)), Luma255(image.At(x, y + 1)))));
            }
            return s;
        }
    }

    // ===================================================================
    // Part 1
    // ===================================================================

    void WriteConceptFigures(const std::string &dir)
    {
        std::printf("concepts\n");

        // 1.8 - the signed distance to a rounded rect, banded every 20 px.
        {
            const int w = 640;
            const int h = 360;
            const glm::vec2 half(200.0f, 110.0f);
            const float r = 40.0f;
            Canvas c(w, h);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const glm::vec2 p(x + 0.5f - w * 0.5f, h * 0.5f - (y + 0.5f));
                    const float d = SdRoundBox(p, half, r);
                    glm::vec3 col = d > 0.0f ? glm::vec3(0.95f, 0.6f, 0.3f) : glm::vec3(0.35f, 0.65f, 1.0f);
                    col *= 0.25f + 0.75f * (1.0f - std::exp(-std::fabs(d) / 60.0f));
                    col *= 0.8f + 0.2f * std::cos(6.28318530718f * d / 20.0f);
                    const float line = 1.0f - std::clamp((std::fabs(d) - 1.0f) / 1.0f, 0.0f, 1.0f);
                    c.At(x, y) = glm::mix(col, glm::vec3(1.0f), line);
                }
            }
            SavePNG(c, Path(dir, "sdf-field.png"));
        }

        // 1.9 - the grade, against a plain clamp.
        {
            Plot plot;
            plot.title = "Tone map: linear light in, display value out";
            plot.xLabel = "linear peak channel (before the grade)";
            plot.yLabel = "display value";
            plot.xMin = 0.0f;
            plot.xMax = 6.0f;
            plot.yMin = 0.0f;
            plot.yMax = 1.05f;
            plot.xTicks = {0, 1, 2, 3, 4, 5, 6};
            plot.yTicks = {0, 0.25f, 0.5f, 0.75f, 1.0f};
            plot.markers = {1.0f};
            Series clamp{"clamp: min(x, 1)", "#999999", {}, true};
            Series reinhard{"x / (x + 0.6)", "#1f77b4", {}, true};
            Series neon{"neon: (x / (x + 0.6))^0.85", "#d62728", {}, false};
            for (int i = 0; i <= 240; ++i)
            {
                const float x = 6.0f * float(i) / 240.0f;
                clamp.points.push_back(glm::vec2(x, std::min(x, 1.0f)));
                reinhard.points.push_back(glm::vec2(x, x / (x + 0.6f)));
                neon.points.push_back(glm::vec2(x, std::pow(x / (x + 0.6f), 0.85f)));
            }
            plot.series = {clamp, reinhard, neon};
            SaveSVG(plot, Path(dir, "tone-map.svg"));
        }
    }

    // ===================================================================
    // Part 3
    // ===================================================================

    void WriteModelFigures(EdgeLightingEffect &effect, const std::string &dir)
    {
        std::printf("model\n");

        // 3.1 - the library's look: its defaults, on a centred rect.
        {
            Config c = WideScene();
            SavePNG(Render(effect, c, WIDE_W, WIDE_H), Path(dir, "hero-defaults.png"));
        }

        // 3.1 / 3.5 - the three layers, one at a time, and their cross-section.
        {
            Config c = WideScene();
            c.neon.colorStops = {Stop(0.0f, CYAN)};
            c.neon.lineWidth = 4.0f;
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.35f;

            Config filament = c;
            filament.neon.glowRadius = 0.0f;
            Config halo = c;
            halo.neon.lineWidth = 0.0f;
            halo.neon.bloomStrength = 0.0f;
            Config glow = c;
            glow.neon.lineWidth = 0.0f;

            const Canvas imgFilament = Render(effect, filament, WIDE_W, WIDE_H);
            const Canvas imgHalo = Render(effect, halo, WIDE_W, WIDE_H);
            const Canvas imgGlow = Render(effect, glow, WIDE_W, WIDE_H);
            const Canvas imgAll = Render(effect, c, WIDE_W, WIDE_H);

            const int cx = 40;
            const int cy = 20;
            const int cw = 360;
            const int ch = 220;
            SavePNG(Crop(imgFilament, cx, cy, cw, ch), Path(dir, "layer-filament.png"));
            SavePNG(Crop(imgHalo, cx, cy, cw, ch), Path(dir, "layer-halo.png"));
            SavePNG(Crop(imgGlow, cx, cy, cw, ch), Path(dir, "layer-halo-bloom.png"));
            SavePNG(Crop(imgAll, cx, cy, cw, ch), Path(dir, "layer-all.png"));

            // Down the frame's centre column, which crosses the top edge at
            // row 85 and reaches the rect's centre at row 225.
            Plot plot;
            plot.title = "Across the top edge: brightness of each layer";
            plot.xLabel = "signed distance d from the outline, px (negative = inside)";
            plot.yLabel = "luma, 0-255";
            plot.xMin = -140.0f;
            plot.xMax = 85.0f;
            plot.yMin = 0.0f;
            plot.yMax = 260.0f;
            plot.xTicks = {-140, -120, -100, -80, -60, -40, -20, 0, 20, 40, 60, 80};
            plot.yTicks = {0, 50, 100, 150, 200, 250};
            plot.markers = {0.0f};
            plot.series = {ColumnProfile(imgFilament, 400, 225, 85.0f, "filament only", "#2ca02c"),
                           ColumnProfile(imgHalo, 400, 225, 85.0f, "halo only", "#1f77b4"),
                           ColumnProfile(imgGlow, 400, 225, 85.0f, "halo + bloom", "#9467bd"),
                           ColumnProfile(imgAll, 400, 225, 85.0f, "all three", "#d62728", true)};
            SaveSVG(plot, Path(dir, "layer-cross-section.svg"));
        }

        // 3.2 - the perimeter position t of every pixel's nearest outline point.
        {
            const int w = 640;
            const int h = 360;
            RectGeometry geom;
            geom.width = 400.0f;
            geom.height = 220.0f;
            geom.cornerRadius = 40.0f;
            geom.winding = Winding::COUNTER_CLOCKWISE;
            const float cx = w * 0.5f;
            const float cy = h * 0.5f;
            Canvas c(w, h);
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const glm::vec2 p(x + 0.5f - cx, cy - (y + 0.5f));
                    const float t = PerimeterPosition(p, geom.width, geom.height, geom.cornerRadius, false);
                    const float d = SdRoundBox(p, glm::vec2(geom.width, geom.height) * 0.5f, geom.cornerRadius);
                    c.At(x, y) = Hsv(t, 0.7f, d > 0.0f ? 0.95f : 0.75f);
                }
            }
            DrawOutline(c, geom, cx, cy, glm::vec3(1.0f), 2.0f, 0.9f);
            DrawPerimeterArrow(c, geom, cx, cy, 0.0f, 0.07f, glm::vec3(1.0f));
            SavePNG(c, Path(dir, "perimeter-t-field.png"));
        }

        // 3.2 - the two windings: an arc from t = 0 to 0.25, red head to yellow tail.
        {
            Config c = PanelScene();
            Arc arc;
            arc.start = 0.0f;
            arc.length = 0.25f;
            arc.colorStops = {Stop(0.0f, glm::vec3(1.0f, 0.12f, 0.08f)), Stop(1.0f, glm::vec3(1.0f, 0.9f, 0.2f))};
            c.neon.arcs = {arc};
            c.neon.glowRadius = 6.0f;
            c.neon.bloomStrength = 0.4f;
            c.geometry.winding = Winding::COUNTER_CLOCKWISE;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "winding-ccw.png"));
            c.geometry.winding = Winding::CLOCKWISE;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "winding-cw.png"));
        }

        // 3.3 - why colour is gathered: the real render beside the same
        // brightness coloured by each pixel's OWN t.
        {
            Config c = PassScene();
            c.neon.colorStops = ShowcaseStops();
            c.neon.lineWidth = 4.0f;
            c.neon.glowRadius = 24.0f;
            c.neon.bloomStrength = 1.0f;
            const Canvas real = Render(effect, c, PASS_W, PASS_H);
            const float cx = c.geometry.position.x + c.geometry.width * 0.5f;
            const float cy = c.geometry.position.y + c.geometry.height * 0.5f;
            Canvas naive(PASS_W, PASS_H);
            for (int y = 0; y < PASS_H; ++y)
            {
                for (int x = 0; x < PASS_W; ++x)
                {
                    const glm::vec3 &px = real.At(x, y);
                    const float peak = std::max(px.r, std::max(px.g, px.b));
                    const glm::vec2 p(x + 0.5f - cx, cy - (y + 0.5f));
                    const float t = PerimeterPosition(p, c.geometry.width, c.geometry.height,
                                                      c.geometry.cornerRadius, false);
                    const glm::vec3 hue = GradientAt(c.neon.colorStops, t);
                    naive.At(x, y) = hue / std::max(std::max(hue.r, std::max(hue.g, hue.b)), 1e-4f) * peak;
                }
            }
            SavePNG(naive, Path(dir, "colour-own-t.png"));
            SavePNG(real, Path(dir, "colour-gathered.png"));
        }

        // 3.3 - the gather's weights for one pixel near the edge and one at the centre.
        {
            const int w = 640;
            const int h = 360;
            RectGeometry geom;
            geom.width = 400.0f;
            geom.height = 220.0f;
            geom.cornerRadius = 40.0f;
            const float cx = w * 0.5f;
            const float cy = h * 0.5f;
            const std::vector<ColorStop> stops = ShowcaseStops();
            const float kc = std::max(RectPerimeter(geom.width, geom.height, geom.cornerRadius) *
                                          float(COLOR_BLEND_PERIM_FRAC),
                                      float(EMISSION_MIN_WIDTH));
            const int n = NEON_MAX_LOOP_SAMPLES;

            const glm::vec2 probes[2] = {glm::vec2(70.0f, 128.0f), glm::vec2(0.0f, 0.0f)};
            const char *names[2] = {"gather-weights-edge.png", "gather-weights-centre.png"};
            for (int k = 0; k < 2; ++k)
            {
                Canvas c(w, h, CLEAR);
                DrawOutline(c, geom, cx, cy, glm::vec3(0.35f), 1.5f, 1.0f);
                std::vector<float> g(n);
                float gMax = 0.0f;
                float gSum = 0.0f;
                glm::vec3 mean(0.0f);
                for (int i = 0; i < n; ++i)
                {
                    const glm::vec2 s = GeometryUtils::GetPointOnRectangle(float(i) / n, geom);
                    const glm::vec2 dv = probes[k] - s;
                    g[i] = 1.0f / (glm::dot(dv, dv) + kc * kc);
                    gMax = std::max(gMax, g[i]);
                    gSum += g[i];
                    mean += GradientAt(stops, float(i) / n) * g[i];
                }
                mean /= gSum;
                for (int i = 0; i < n; ++i)
                {
                    const float wgt = g[i] / gMax;
                    const glm::vec2 s = ToImage(GeometryUtils::GetPointOnRectangle(float(i) / n, geom), cx, cy);
                    DrawDisc(c, s, 1.5f + 4.0f * std::sqrt(wgt), GradientAt(stops, float(i) / n),
                             0.25f + 0.75f * wgt);
                }
                const glm::vec2 probe = ToImage(probes[k], cx, cy);
                DrawDisc(c, probe, 11.0f, glm::vec3(1.0f), 1.0f);
                DrawDisc(c, probe, 8.5f, mean, 1.0f);
                SavePNG(c, Path(dir, names[k]));
            }
        }

        // 3.4 - one arc with free ends; two arcs that tile, each with its own stops.
        {
            Config c = PanelScene();
            c.neon.colorStops = ShowcaseStops();
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.5f;

            Arc single;
            single.start = 0.1f;
            single.length = 0.35f;
            c.neon.arcs = {single};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "arc-single.png"));

            Arc a;
            a.start = 0.0f;
            a.length = 0.5f;
            a.colorStops = {Stop(0.0f, glm::vec3(0.1f, 0.9f, 1.0f)), Stop(1.0f, glm::vec3(0.2f, 0.3f, 1.0f))};
            Arc b;
            b.start = 0.5f;
            b.length = 0.5f;
            b.intensity = 0.6f;
            b.colorStops = {Stop(0.0f, glm::vec3(1.0f, 0.75f, 0.1f)), Stop(1.0f, glm::vec3(1.0f, 0.15f, 0.2f))};
            c.neon.arcs = {a, b};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "arc-tiled.png"));
        }

        // 3.4 - a segment on a dark stretch, below and above the 0.5 gate.
        {
            Config c = PanelScene();
            c.neon.colorStops = {Stop(0.0f, CYAN)};
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.5f;
            Arc half;
            half.start = 0.0f;
            half.length = 0.5f;
            c.neon.arcs = {half};
            SegmentBoost seg;
            seg.position = 0.75f;
            seg.length = 0.12f;
            seg.colorStops = {Stop(0.0f, ORANGE)};
            seg.boost = 0.4f;
            c.neon.segmentBoosts = {seg};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "segment-boost-0.4.png"));
            c.neon.segmentBoosts[0].boost = 2.0f;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "segment-boost-2.png"));
        }

        // 3.6 - pointwise against gathered coverage, at the end of an arc
        // that stops in the middle of the bottom edge.
        {
            Config c = WideScene();
            c.neon.colorStops = {Stop(0.0f, CYAN)};
            c.neon.glowRadius = 10.0f;
            c.neon.bloomStrength = 0.6f;
            const float tMid = PerimeterPosition(glm::vec2(0.0f, -c.geometry.height * 0.5f), c.geometry.width,
                                                 c.geometry.height, c.geometry.cornerRadius, false);
            Arc half;
            half.start = tMid + 0.5f - std::floor(tMid + 0.5f);
            half.length = 0.5f;
            c.neon.arcs = {half};
            const Canvas img = Render(effect, c, WIDE_W, WIDE_H);
            SavePNG(Magnify(Crop(img, 300, 300, 200, 110), 2), Path(dir, "arc-end-closeup.png"));

            // The bottom edge is row 365; the arc ends at x = 400 and is lit
            // to the left of it.
            Plot plot;
            plot.title = "Along the bottom edge, past the end of an arc";
            plot.xLabel = "x, px (the arc ends at 400; lit to the left)";
            plot.yLabel = "luma, 0-255";
            plot.xMin = 250.0f;
            plot.xMax = 550.0f;
            plot.yMin = 0.0f;
            plot.yMax = 260.0f;
            plot.xTicks = {250, 300, 350, 400, 450, 500, 550};
            plot.yTicks = {0, 50, 100, 150, 200, 250};
            plot.markers = {386.0f, 400.0f};
            plot.series = {RowProfile(img, 364, 250, 550, "on the line (filament)", "#d62728"),
                           RowProfile(img, 376, 250, 550, "12 px outside (glow)", "#1f77b4"),
                           RowProfile(img, 394, 250, 550, "30 px outside (glow)", "#9467bd", true)};
            SaveSVG(plot, Path(dir, "arc-end-profile.svg"));
        }
    }

    // ===================================================================
    // Part 4
    // ===================================================================

    void WriteConfigFigures(EdgeLightingEffect &effect, const std::string &dir)
    {
        std::printf("config\n");

        // 4.3 - the line and the glow, one field at a time.
        {
            Config base = WideScene();
            base.neon.colorStops = {Stop(0.0f, CYAN)};

            // Around the top-left corner, magnified 3x: the corner arc spans
            // x 120-168, y 85-133.
            auto lineCrop = [&](const Config &c, const char *name) {
                SavePNG(Magnify(Crop(Render(effect, c, WIDE_W, WIDE_H), 96, 56, 104, 72), 3), Path(dir, name));
            };
            auto cornerCrop = [&](const Config &c, const char *name) {
                SavePNG(Crop(Render(effect, c, WIDE_W, WIDE_H), 40, 20, 320, 200), Path(dir, name));
            };

            Config c = base;
            c.neon.glowRadius = 4.0f;
            c.neon.bloomStrength = 0.2f;
            const float widths[3] = {1.0f, 4.0f, 12.0f};
            const char *widthNames[3] = {"line-width-1.png", "line-width-4.png", "line-width-12.png"};
            for (int i = 0; i < 3; ++i)
            {
                c.neon.lineWidth = widths[i];
                lineCrop(c, widthNames[i]);
            }

            c = base;
            c.neon.lineWidth = 12.0f;
            c.neon.glowRadius = 0.0f;
            c.neon.bloomStrength = 0.0f;
            const float falloffs[3] = {0.5f, 1.0f, 4.0f};
            const char *falloffNames[3] = {"falloff-0.5.png", "falloff-1.png", "falloff-4.png"};
            for (int i = 0; i < 3; ++i)
            {
                c.neon.filamentFalloff = falloffs[i];
                lineCrop(c, falloffNames[i]);
            }

            c = base;
            c.neon.bloomStrength = 0.4f;
            const float radii[3] = {3.0f, 10.0f, 30.0f};
            const char *radiusNames[3] = {"glow-radius-3.png", "glow-radius-10.png", "glow-radius-30.png"};
            for (int i = 0; i < 3; ++i)
            {
                c.neon.glowRadius = radii[i];
                cornerCrop(c, radiusNames[i]);
            }

            c = base;
            c.neon.glowRadius = 10.0f;
            const float blooms[3] = {0.0f, 0.6f, 1.5f};
            const char *bloomNames[3] = {"bloom-0.png", "bloom-0.6.png", "bloom-1.5.png"};
            for (int i = 0; i < 3; ++i)
            {
                c.neon.bloomStrength = blooms[i];
                cornerCrop(c, bloomNames[i]);
            }
        }

        // 4.4 - which side glows, and the cutoffs.
        {
            Config c = PanelScene();
            c.neon.colorStops = ShowcaseStops();
            c.neon.glowRadius = 10.0f;
            c.neon.bloomStrength = 0.6f;
            c.neon.glowSide = GlowSide::BOTH;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "glow-side-both.png"));
            c.neon.glowSide = GlowSide::INSIDE;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "glow-side-inside.png"));
            c.neon.glowSide = GlowSide::OUTSIDE;
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "glow-side-outside.png"));

            c.neon.glowSide = GlowSide::BOTH;
            c.neon.glowRadius = 16.0f;
            c.neon.bloomStrength = 1.0f;
            c.neon.insideCutoff = Cutoff{true, 20.0f, 12.0f};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "cutoff-inside.png"));
            c.neon.insideCutoff = Cutoff{false, 0.0f, 0.0f};
            c.neon.outsideCutoff = Cutoff{true, 24.0f, 16.0f};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "cutoff-outside.png"));
            c.neon.insideCutoff = Cutoff{true, 20.0f, 12.0f};
            SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, "cutoff-band.png"));
        }

        // 4.5 - the opaque fill, over a checkerboard so its extent shows.
        {
            Config c = PanelScene();
            c.neon.colorStops = ShowcaseStops();
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.5f;
            const OpaqueMode modes[4] = {OpaqueMode::NONE, OpaqueMode::OUTSIDE, OpaqueMode::INSIDE,
                                         OpaqueMode::BOTH};
            const char *names[4] = {"opaque-none.png", "opaque-outside.png", "opaque-inside.png",
                                    "opaque-both.png"};
            for (int i = 0; i < 4; ++i)
            {
                c.neon.opaqueMode = modes[i];
                const bool band = modes[i] == OpaqueMode::BOTH;
                c.neon.opaqueInsideCutoff = Cutoff{band, 24.0f, 0.0f};
                c.neon.opaqueOutsideCutoff = Cutoff{band, 24.0f, 0.0f};
                SavePNG(Render(effect, c, PANEL_W, PANEL_H, true), Path(dir, names[i]));
            }
        }

        // 4.6 - blend spaces, with the debug layer's ring strip and stop markers.
        {
            Config c = PanelScene();
            c.neon.colorStops = {Stop(0.0f, glm::vec3(1.0f, 0.1f, 0.1f)), Stop(0.5f, glm::vec3(0.1f, 1.0f, 0.1f))};
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.5f;
            c.debug.showGradientLUT = true;
            c.debug.showColorStops = true;
            const BlendSpace spaces[3] = {BlendSpace::RGB, BlendSpace::HSV, BlendSpace::HSL};
            const char *names[3] = {"blend-rgb.png", "blend-hsv.png", "blend-hsl.png"};
            for (int i = 0; i < 3; ++i)
            {
                c.neon.blendSpace = spaces[i];
                SavePNG(Render(effect, c, PANEL_W, PANEL_H), Path(dir, names[i]));
            }
        }

        // 3.6 / 4.6 - colour-stop alpha reaches the filament and not the glow.
        {
            Config c = PanelScene();
            c.neon.colorStops = {Stop(0.0f, CYAN, 1.0f), Stop(0.3f, CYAN, 1.0f), Stop(0.4f, CYAN, 0.0f),
                                 Stop(0.6f, CYAN, 0.0f), Stop(0.7f, CYAN, 1.0f)};
            c.neon.glowRadius = 6.0f;
            c.neon.bloomStrength = 0.3f;
            const Canvas img = Render(effect, c, PANEL_W, PANEL_H);
            SavePNG(img, Path(dir, "stop-alpha.png"));
            // Where the bottom edge passes from alpha 1 to alpha 0 (t 0.3 to 0.4).
            SavePNG(Magnify(Crop(img, 250, 196, 120, 44), 3), Path(dir, "stop-alpha-closeup.png"));
        }

        // 9.2 - the debug layer's three overlays.
        {
            Config c = WideScene();
            c.neon.colorStops = ShowcaseStops();
            c.neon.glowRadius = 8.0f;
            c.neon.bloomStrength = 0.5f;
            c.debug.showGradientLUT = true;
            c.debug.showColorStops = true;
            c.debug.showWireframe = true;
            SavePNG(Render(effect, c, WIDE_W, WIDE_H), Path(dir, "debug-overlays.png"));
        }
    }

    // ===================================================================
    // Parts 5, 6 and 8
    // ===================================================================

    void WritePassFigures(const std::string &dir)
    {
        std::printf("passes\n");

        // One scene with something in every row of every buffer: a lit arc,
        // a dimmer arc with its own stops, a gap between them, and a segment.
        Config c = PassScene();
        c.neon.colorStops = ShowcaseStops();
        c.neon.lineWidth = 3.0f;
        c.neon.glowRadius = 10.0f;
        c.neon.bloomStrength = 0.6f;
        Arc a;
        a.start = 0.0f;
        a.length = 0.6f;
        Arc b;
        b.start = 0.65f;
        b.length = 0.3f;
        b.intensity = 0.6f;
        b.colorStops = {Stop(0.0f, glm::vec3(1.0f, 0.75f, 0.1f)), Stop(1.0f, glm::vec3(1.0f, 0.15f, 0.2f))};
        c.neon.arcs = {a, b};
        SegmentBoost seg;
        seg.position = 0.3f;
        seg.length = 0.08f;
        seg.boost = 1.5f;
        c.neon.segmentBoosts = {seg};
        c.neon.resolutionScale = 0.5f;

        {
            std::unique_ptr<EdgeLightingEffect> effect = FreshEffect();
            Render(*effect, c, PASS_W, PASS_H, false, true);

            // P0 - the emission table, one 5 px column per sample, four bands:
            // row 0 colour, row 0 weight, row 1 colour, row 1 weight.
            if (const DrawRecord *p0 = PassRecorder::Find(PassKind::P0))
            {
                const Attachment &t = p0->written[0];
                const int cell = 5;
                const int band = 30;
                const int gap = 6;
                Canvas out(t.width * cell, 4 * band + 3 * gap, glm::vec3(0.18f));
                float bellMax = 1e-6f;
                for (int i = 0; i < t.width; ++i)
                {
                    bellMax = std::max(bellMax, t.At(i, 1).a);
                }
                for (int i = 0; i < t.width; ++i)
                {
                    const glm::vec4 r0 = t.At(i, 0);
                    const glm::vec4 r1 = t.At(i, 1);
                    const glm::vec3 values[4] = {glm::clamp(glm::vec3(r0), 0.0f, 1.0f),
                                                 glm::vec3(std::clamp(r0.a, 0.0f, 1.0f)),
                                                 glm::clamp(glm::vec3(r1), 0.0f, 1.0f),
                                                 glm::vec3(r1.a / bellMax)};
                    for (int k = 0; k < 4; ++k)
                    {
                        FillRect(out, i * cell, k * (band + gap), cell, band, values[k]);
                    }
                }
                SavePNG(out, Path(dir, "pass-p0-emission.png"));
            }

            // P0b - the glow coverage table: the arcs' coverage as the halo
            // (top band) and the bloom (bottom band) see it, perimeter position
            // across, distance from the line down, every fourth column.
            if (const DrawRecord *p0b = PassRecorder::Find(PassKind::P0B))
            {
                const Attachment &t = p0b->written[0];
                const int step = 4;
                const int rowPx = 2;
                const int gap = 6;
                const int columns = (t.width - 2) / step;
                Canvas out(columns, 2 * t.height * rowPx + gap, glm::vec3(0.18f));
                for (int i = 0; i < columns; ++i)
                {
                    for (int j = 0; j < t.height; ++j)
                    {
                        const glm::vec4 v = t.At(1 + i * step, j);
                        FillRect(out, i, j * rowPx, 1, rowPx, glm::vec3(std::min(DecodeCoverage(v.r), 1.0f)));
                        FillRect(out, i, t.height * rowPx + gap + j * rowPx, 1, rowPx,
                                 glm::vec3(std::min(DecodeCoverage(v.g), 1.0f)));
                    }
                }
                SavePNG(out, Path(dir, "pass-p0b-glow-cover.png"));
            }

            // P1a - the gather buffer: hue, arc coverage, segment coverage.
            if (const DrawRecord *p1a = PassRecorder::Find(PassKind::P1A))
            {
                const Attachment &g0 = p1a->written[0];
                const int k = FitFactor(g0.width, 600);
                SavePNG(Magnify(AttachmentImage(g0, [](const glm::vec4 &v) { return glm::vec3(v); }), k),
                        Path(dir, "pass-p1a-gather-hue.png"));
                SavePNG(Magnify(AttachmentImage(g0,
                                                [](const glm::vec4 &v) {
                                                    return glm::vec3(std::min(DecodeCoverage(v.a), 1.0f));
                                                }),
                                k),
                        Path(dir, "pass-p1a-gather-cover.png"));
                if (p1a->written.size() > 1)
                {
                    const Attachment &g1 = p1a->written[1];
                    float most = 1e-6f;
                    for (const glm::vec4 &v : g1.texels)
                    {
                        most = std::max(most, DecodeCoverage(v.a));
                    }
                    SavePNG(Magnify(AttachmentImage(g1,
                                                    [most](const glm::vec4 &v) {
                                                        return glm::vec3(v) * (DecodeCoverage(v.a) / most);
                                                    }),
                                    k),
                            Path(dir, "pass-p1a-gather-segment.png"));
                }
                std::printf("    gather buffer %d x %d, %zu attachment(s), internal format 0x%x\n", g0.width,
                            g0.height, p1a->written.size(), unsigned(g0.internalFormat));
            }

            // P1b - the reduced buffer, magnified so its texels show.
            if (const DrawRecord *p1b = PassRecorder::Find(PassKind::P1B))
            {
                const Attachment &s = p1b->written[0];
                SavePNG(Magnify(AttachmentImage(s, OverClear), 2), Path(dir, "pass-p1b-reduced.png"));
                std::printf("    reduced buffer %d x %d\n", s.width, s.height);
            }

            // P2b, P2c - the caller's framebuffer after the blit, then after the ring.
            const DrawRecord *p2b = PassRecorder::Find(PassKind::P2B);
            const DrawRecord *p2c = PassRecorder::Find(PassKind::P2C);
            if (p2b && p2c)
            {
                const Canvas afterBlit = AttachmentImage(p2b->written[0], [](const glm::vec4 &v) {
                    return glm::vec3(v);
                });
                const Canvas afterRing = AttachmentImage(p2c->written[0], [](const glm::vec4 &v) {
                    return glm::vec3(v);
                });
                SavePNG(afterBlit, Path(dir, "pass-after-blit.png"));
                SavePNG(afterRing, Path(dir, "pass-after-ring.png"));
                SavePNG(Magnify(Crop(afterBlit, 76, 244, 104, 70), 3), Path(dir, "pass-after-blit-crop.png"));
                SavePNG(Magnify(Crop(afterRing, 76, 244, 104, 70), 3), Path(dir, "pass-after-ring-crop.png"));
                // The ring's own output: what it drew, with the blit's pixels
                // left out.
                SavePNG(OnlyWhereDrawn(afterRing, *p2c), Path(dir, "pass-p2c-ring-only.png"));
            }
        }

        // P1 - the direct path's one pass, for the same scene at scale 1.0.
        {
            Config direct = c;
            direct.neon.resolutionScale = 1.0f;
            std::unique_ptr<EdgeLightingEffect> effect = FreshEffect();
            Render(*effect, direct, PASS_W, PASS_H, false, true);
            if (const DrawRecord *p1 = PassRecorder::Find(PassKind::P1))
            {
                SavePNG(AttachmentImage(p1->written[0], [](const glm::vec4 &v) { return glm::vec3(v); }),
                        Path(dir, "pass-p1-direct.png"));
            }
        }

        // 1.12 / 8 - what the ring is for: a thin line at scale 0.25, as the
        // blit alone would draw it, as the frame draws it, and at 1.0.
        {
            Config thin = c;
            thin.neon.lineWidth = 2.0f;
            thin.neon.resolutionScale = 0.25f;
            std::unique_ptr<EdgeLightingEffect> effect = FreshEffect();
            const Canvas scaled = Render(*effect, thin, PASS_W, PASS_H, false, true);
            const DrawRecord *p1b = PassRecorder::Find(PassKind::P1B);
            const DrawRecord *p2b = PassRecorder::Find(PassKind::P2B);
            if (p1b && p2b)
            {
                const Canvas naive = NaiveUpscale(p1b->written[0], *p2b, thin, PASS_W, PASS_H);
                SavePNG(Magnify(Crop(naive, 76, 244, 104, 70), 3), Path(dir, "scale-0.25-blit-only.png"));
            }
            SavePNG(Magnify(Crop(scaled, 76, 244, 104, 70), 3), Path(dir, "scale-0.25-final.png"));
            thin.neon.resolutionScale = 1.0f;
            const Canvas full = Render(*effect, thin, PASS_W, PASS_H);
            SavePNG(Magnify(Crop(full, 76, 244, 104, 70), 3), Path(dir, "scale-1-final.png"));
        }

        // 5.6 / 8.3 - the geometry each path draws, with both cutoffs on so
        // every boundary lands inside the frame.
        {
            Config g = PassScene();
            g.neon.colorStops = ShowcaseStops();
            g.neon.lineWidth = 3.0f;
            g.neon.glowRadius = 10.0f;
            g.neon.bloomStrength = 0.6f;
            g.neon.insideCutoff = Cutoff{true, 40.0f, 16.0f};
            g.neon.outsideCutoff = Cutoff{true, 36.0f, 16.0f};

            {
                std::unique_ptr<EdgeLightingEffect> effect = FreshEffect();
                const Canvas frame = Render(*effect, g, PASS_W, PASS_H, false, true);
                SavePNG(GeometryOverlay(frame, {{PassKind::P1, glm::vec3(0.3f, 0.6f, 1.0f)}}),
                        Path(dir, "geometry-direct.png"));
            }
            {
                g.neon.resolutionScale = 0.5f;
                std::unique_ptr<EdgeLightingEffect> effect = FreshEffect();
                const Canvas frame = Render(*effect, g, PASS_W, PASS_H, false, true);
                SavePNG(GeometryOverlay(frame, {{PassKind::P2B, glm::vec3(0.3f, 0.6f, 1.0f)},
                                                {PassKind::P2C, glm::vec3(0.3f, 1.0f, 0.45f)}}),
                        Path(dir, "geometry-scaled.png"));
            }
        }
    }
}
