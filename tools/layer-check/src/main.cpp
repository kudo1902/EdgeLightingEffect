// layer-check - the five-layer baseline of docs/resource-sharing-plan.md (S0).
//
// neon-scale-check judges the neon alone, against images. This judges every
// layer at once, against ITSELF: it renders a fixed set of scenes through all
// five renderers and writes a report - one hash per frame, the GL memory and
// heap allocations each scene holds, and what the effect published - so a
// change that must not move a pixel is checked by diffing two reports from
// the same machine and build type. `startup` measures what the plan's shared
// shader cache is for: Initialize and first-frame time, and every shader
// compile, by source.
//
//   layer-check report [--out FILE] [--only SUBSTRING]
//                                        every scene, or those whose id has SUBSTRING;
//                                        FILE defaults to stdout
//   layer-check diff A B                 exit 1 if any frame hash differs
//   layer-check startup [--fail-stage NAME]
//
// See README.md.

#include "core/edge-lighting.h"
#include "core/buffer-registry.h"
#include "util/capture-util.h"
#include "shaders.h"
#include "scenes.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace EdgeLighting;

// -----------------------------------------------------------------------------
// Heap allocations, counted only inside the window a measurement opens and
// never inside a GL hook below, whose own bookkeeping would otherwise count.
// -----------------------------------------------------------------------------
namespace
{
    long gNewCount = 0;
    bool gCountingNews = false;
    bool gInHook = false;

    void *CountedAlloc(std::size_t size)
    {
        if (gCountingNews && !gInHook)
        {
            ++gNewCount;
        }
        void *p = std::malloc(size ? size : 1);
        if (!p)
        {
            throw std::bad_alloc();
        }
        return p;
    }
} // namespace

void *operator new(std::size_t size) { return CountedAlloc(size); }
void *operator new[](std::size_t size) { return CountedAlloc(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace
{
    const int WIDTH = 1280;
    const int HEIGHT = 720;
    const int HASHED_FRAMES = 4;
    const int COUNTED_FRAMES = 8;
    const float DT = 1.0f / 60.0f;

    // -------------------------------------------------------------------------
    // GL hooks: the library calls GL through glad's pointers, so replacing a
    // pointer after gladLoadGL sees every call it makes (a static link only).
    // -------------------------------------------------------------------------
    typedef struct KnownSource
    {
        const char *name;
        const char *source;
    } KnownSource;

    const KnownSource KNOWN_SOURCES[] = {
        {"BLACK_RECT_FRAG_SRC", ShaderSource::BLACK_RECT_FRAG_SRC},
        {"BLIT_FRAG_SRC", ShaderSource::BLIT_FRAG_SRC},
        {"DEBUG_BUFFER_VIEW_FRAG_SRC", ShaderSource::DEBUG_BUFFER_VIEW_FRAG_SRC},
        {"DROPLETS_FRAG_SRC", ShaderSource::DROPLETS_FRAG_SRC},
        {"LENS_FLARE_FRAG_SRC", ShaderSource::LENS_FLARE_FRAG_SRC},
        {"LENS_FLARE_VERT_SRC", ShaderSource::LENS_FLARE_VERT_SRC},
        {"NEON_BLIT_FRAG_SRC", ShaderSource::NEON_BLIT_FRAG_SRC},
        {"NEON_EMISSION_FRAG_SRC", ShaderSource::NEON_EMISSION_FRAG_SRC},
        {"NEON_FIELD_FRAG_SRC", ShaderSource::NEON_FIELD_FRAG_SRC},
        {"NEON_FRAG_SRC", ShaderSource::NEON_FRAG_SRC},
        {"NEON_GATHER_FRAG_SRC", ShaderSource::NEON_GATHER_FRAG_SRC},
        {"NEON_GLOW_COVER_FILL_FRAG_SRC", ShaderSource::NEON_GLOW_COVER_FILL_FRAG_SRC},
        {"NEON_GLOW_COVER_FRAG_SRC", ShaderSource::NEON_GLOW_COVER_FRAG_SRC},
        {"NEON_LUT_DEBUG_FRAG_SRC", ShaderSource::NEON_LUT_DEBUG_FRAG_SRC},
        {"NEON_STOP_MARKER_FRAG_SRC", ShaderSource::NEON_STOP_MARKER_FRAG_SRC},
        {"NEON_VERT_SRC", ShaderSource::NEON_VERT_SRC},
        {"SPOTLIGHT_BLIT_FRAG_SRC", ShaderSource::SPOTLIGHT_BLIT_FRAG_SRC},
        {"SPOTLIGHT_FRAG_SRC", ShaderSource::SPOTLIGHT_FRAG_SRC},
        {"SPOTLIGHT_VERT_SRC", ShaderSource::SPOTLIGHT_VERT_SRC},
        {"WIREFRAME_FRAG_SRC", ShaderSource::WIREFRAME_FRAG_SRC},
        {"WIREFRAME_VERT_SRC", ShaderSource::WIREFRAME_VERT_SRC},
    };

    /// The ShaderSource constant @p source was made from. A renderer may put a
    /// #define in after the #version line (NeonRenderer's WithDefine), which
    /// leaves the source's tail as it was, so the tail names it: "+def" marks
    /// such a variant.
    std::string LabelFor(const std::string &source)
    {
        for (const KnownSource &known : KNOWN_SOURCES)
        {
            const size_t n = std::strlen(known.source);
            if (source.size() == n && source == known.source)
            {
                return known.name;
            }
            const size_t tail = std::min<size_t>(n, 256);
            if (source.size() > n &&
                source.compare(source.size() - tail, tail, known.source + (n - tail)) == 0)
            {
                return std::string(known.name) + "+def";
            }
        }
        return "unknown";
    }

    typedef struct GLStats
    {
        long compiles = 0;
        long links = 0;
        double compileMs = 0.0;
        double linkMs = 0.0;
        std::map<std::string, int> compilesByLabel;
    } GLStats;

    GLStats gStats;
    std::map<GLuint, std::string> gShaderLabel;
    std::string gFailLabel;
    std::set<GLuint> gForcedFailures;
    std::map<std::pair<GLuint, GLint>, size_t> gTextureBytes;
    std::map<GLuint, size_t> gBufferBytes;

    PFNGLSHADERSOURCEPROC gRealShaderSource = nullptr;
    PFNGLCOMPILESHADERPROC gRealCompileShader = nullptr;
    PFNGLGETSHADERIVPROC gRealGetShaderiv = nullptr;
    PFNGLLINKPROGRAMPROC gRealLinkProgram = nullptr;
    PFNGLTEXIMAGE2DPROC gRealTexImage2D = nullptr;
    PFNGLDELETETEXTURESPROC gRealDeleteTextures = nullptr;
    PFNGLBUFFERDATAPROC gRealBufferData = nullptr;
    PFNGLDELETEBUFFERSPROC gRealDeleteBuffers = nullptr;

    double MsSince(std::chrono::high_resolution_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    void APIENTRY HookShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
    {
        gInHook = true;
        std::string source;
        for (GLsizei i = 0; i < count; ++i)
        {
            if (lengths && lengths[i] >= 0)
            {
                source.append(strings[i], size_t(lengths[i]));
            }
            else
            {
                source.append(strings[i]);
            }
        }
        gShaderLabel[shader] = LabelFor(source);
        gInHook = false;
        gRealShaderSource(shader, count, strings, lengths);
    }

    void APIENTRY HookCompileShader(GLuint shader)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        gRealCompileShader(shader);
        gInHook = true;
        gStats.compileMs += MsSince(t0);
        ++gStats.compiles;
        const std::string &label = gShaderLabel[shader];
        ++gStats.compilesByLabel[label];
        // By id, and cleared for any other source: GL hands a deleted shader's
        // id to the next glCreateShader, so a stale entry would fail an
        // unrelated stage.
        if (!gFailLabel.empty() && (label == gFailLabel || label == gFailLabel + "+def"))
        {
            gForcedFailures.insert(shader);
        }
        else
        {
            gForcedFailures.erase(shader);
        }
        gInHook = false;
    }

    void APIENTRY HookGetShaderiv(GLuint shader, GLenum pname, GLint *params)
    {
        gRealGetShaderiv(shader, pname, params);
        if (pname == GL_COMPILE_STATUS && gForcedFailures.count(shader) != 0)
        {
            *params = GL_FALSE;
        }
    }

    void APIENTRY HookLinkProgram(GLuint program)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        gRealLinkProgram(program);
        gStats.linkMs += MsSince(t0);
        ++gStats.links;
    }

    size_t BytesPerTexel(GLint internalFormat, GLenum type)
    {
        switch (internalFormat)
        {
        case GL_R8:
        {
            return 1;
        }
        case GL_RG8:
        case GL_R16F:
        {
            return 2;
        }
        case GL_RGB8:
        {
            return 3;
        }
        case GL_RGBA8:
        case GL_RG16F:
        case GL_R32F:
        case GL_SRGB8_ALPHA8:
        case GL_RGB10_A2:
        case GL_R11F_G11F_B10F:
        {
            return 4;
        }
        case GL_RGB16F:
        {
            return 6;
        }
        case GL_RGBA16F:
        case GL_RG32F:
        {
            return 8;
        }
        case GL_RGB32F:
        {
            return 12;
        }
        case GL_RGBA32F:
        {
            return 16;
        }
        default:
        {
            // Unsized: four channels of the upload type.
            return type == GL_FLOAT ? 16 : type == GL_HALF_FLOAT ? 8 : 4;
        }
        }
    }

    void APIENTRY HookTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                                 GLint border, GLenum format, GLenum type, const void *pixels)
    {
        gRealTexImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
        if (target != GL_TEXTURE_2D)
        {
            return;
        }
        gInHook = true;
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        gTextureBytes[{GLuint(bound), level}] = size_t(width) * size_t(height) * BytesPerTexel(internalFormat, type);
        gInHook = false;
    }

    void APIENTRY HookDeleteTextures(GLsizei n, const GLuint *textures)
    {
        gInHook = true;
        for (GLsizei i = 0; i < n; ++i)
        {
            for (auto it = gTextureBytes.begin(); it != gTextureBytes.end();)
            {
                it = it->first.first == textures[i] ? gTextureBytes.erase(it) : std::next(it);
            }
        }
        gInHook = false;
        gRealDeleteTextures(n, textures);
    }

    void APIENTRY HookBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
    {
        gRealBufferData(target, size, data, usage);
        GLenum binding = 0;
        switch (target)
        {
        case GL_ARRAY_BUFFER:
        {
            binding = GL_ARRAY_BUFFER_BINDING;
            break;
        }
        case GL_ELEMENT_ARRAY_BUFFER:
        {
            binding = GL_ELEMENT_ARRAY_BUFFER_BINDING;
            break;
        }
        case GL_UNIFORM_BUFFER:
        {
            binding = GL_UNIFORM_BUFFER_BINDING;
            break;
        }
        default:
        {
            return;
        }
        }
        gInHook = true;
        GLint bound = 0;
        glGetIntegerv(binding, &bound);
        gBufferBytes[GLuint(bound)] = size_t(size);
        gInHook = false;
    }

    void APIENTRY HookDeleteBuffers(GLsizei n, const GLuint *buffers)
    {
        gInHook = true;
        for (GLsizei i = 0; i < n; ++i)
        {
            gBufferBytes.erase(buffers[i]);
        }
        gInHook = false;
        gRealDeleteBuffers(n, buffers);
    }

    void InstallHooks()
    {
        gRealShaderSource = glad_glShaderSource;
        gRealCompileShader = glad_glCompileShader;
        gRealGetShaderiv = glad_glGetShaderiv;
        gRealLinkProgram = glad_glLinkProgram;
        gRealTexImage2D = glad_glTexImage2D;
        gRealDeleteTextures = glad_glDeleteTextures;
        gRealBufferData = glad_glBufferData;
        gRealDeleteBuffers = glad_glDeleteBuffers;
        glad_glShaderSource = HookShaderSource;
        glad_glCompileShader = HookCompileShader;
        glad_glGetShaderiv = HookGetShaderiv;
        glad_glLinkProgram = HookLinkProgram;
        glad_glTexImage2D = HookTexImage2D;
        glad_glDeleteTextures = HookDeleteTextures;
        glad_glBufferData = HookBufferData;
        glad_glDeleteBuffers = HookDeleteBuffers;
    }

    size_t LiveTextureBytes()
    {
        size_t total = 0;
        for (const auto &entry : gTextureBytes)
        {
            total += entry.second;
        }
        return total;
    }

    size_t LiveBufferBytes()
    {
        size_t total = 0;
        for (const auto &entry : gBufferBytes)
        {
            total += entry.second;
        }
        return total;
    }

    // -------------------------------------------------------------------------
    // GL context.
    // -------------------------------------------------------------------------
    GLFWwindow *gWindow = nullptr;

    void InitGL()
    {
        if (!glfwInit())
        {
            std::fprintf(stderr, "layer-check: glfwInit failed\n");
            std::exit(2);
        }
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        gWindow = glfwCreateWindow(64, 64, "layer-check", nullptr, nullptr);
        if (!gWindow)
        {
            std::fprintf(stderr, "layer-check: could not create a GL 3.3 core context\n");
            std::exit(2);
        }
        glfwMakeContextCurrent(gWindow);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        {
            std::fprintf(stderr, "layer-check: GLAD could not load GL\n");
            std::exit(2);
        }
        InstallHooks();
    }

    std::string RendererName()
    {
        const GLubyte *name = glGetString(GL_RENDERER);
        return name ? std::string(reinterpret_cast<const char *>(name)) : std::string("unknown");
    }

    // -------------------------------------------------------------------------
    // Scenes.
    // -------------------------------------------------------------------------
    typedef enum class Motion
    {
        STILL,          ///< The config holds; the clock advances.
        HUE,            ///< hueRotationRate 0.5, the clock advancing.
        INTENSITY,      ///< intensity +/-10%, one SetConfig per frame.
        ARC_WIPE,       ///< arcs[0].length sweeping, one SetConfig per frame.
        SEGMENT_TRAVEL, ///< segmentBoosts[0] moving round the ring, one SetConfig per frame.
        STOP_CHANGE,    ///< the colour stops replaced on the second frame, with a 0.5 s cross-fade.
    } Motion;

    typedef struct LayerScene
    {
        std::string id;
        Config config;
        Motion motion = Motion::STILL;
    } LayerScene;

    std::string ScaleTag(float scale)
    {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "%g", double(scale));
        return tag;
    }

    Config NeonScene(const char *id)
    {
        Config c = NeonScaleCheck::BaseConfig();
        for (const NeonScaleCheck::Scene &scene : NeonScaleCheck::SCENES)
        {
            if (std::strcmp(scene.id, id) == 0)
            {
                scene.apply(c);
                return c;
            }
        }
        std::fprintf(stderr, "layer-check: no neon scene '%s'\n", id);
        std::exit(2);
    }

    void AddFlare(Config &c, float scale)
    {
        c.lensFlare.enable = true;
        c.lensFlare.perimeterPosition = 0.3f;
        c.lensFlare.resolutionScale = scale;
    }

    void AddSpotlightFan(Config &c, float scale)
    {
        c.spotlight.enable = true;
        c.spotlight.resolutionScale = scale;
        c.spotlight.lamps.clear();
        for (int i = 0; i < 5; ++i)
        {
            SpotLight lamp;
            lamp.position = glm::vec2(240.0f + 200.0f * float(i), 40.0f);
            lamp.angle = 60.0f + 15.0f * float(i);
            lamp.throwLength = 420.0f;
            lamp.colorTemp = 3200.0f + 800.0f * float(i);
            c.spotlight.lamps.push_back(lamp);
        }
    }

    void AddDebugOverlays(Config &c)
    {
        c.debug.showGradientLUT = true;
        c.debug.showColorStops = true;
        c.debug.showWireframe = true;
    }

    /// Everything on at once below 1.0: the frame that interleaves offscreen
    /// and on-screen work across layers today.
    Config AllLayers()
    {
        Config c = NeonScene("segments");
        c.neon.resolutionScale = 0.5f;
        c.neon.hueRotationRate = 0.5f;
        c.droplets.enable = true;
        AddFlare(c, 0.5f);
        AddSpotlightFan(c, 0.5f);
        AddDebugOverlays(c);
        return c;
    }

    std::vector<LayerScene> BuildScenes()
    {
        std::vector<LayerScene> scenes;
        auto add = [&](const std::string &id, const Config &c, Motion motion) {
            LayerScene s;
            s.id = id;
            s.config = c;
            s.motion = motion;
            scenes.push_back(s);
        };

        static const float NEON_SCALES[] = {1.0f, 0.5f, 0.25f};
        for (const NeonScaleCheck::Scene &scene : NeonScaleCheck::SCENES)
        {
            for (float scale : NEON_SCALES)
            {
                Config c = NeonScene(scene.id);
                c.neon.resolutionScale = scale;
                add(std::string("neon/") + scene.id + "@" + ScaleTag(scale), c, Motion::STILL);
            }
        }

        typedef struct Moving
        {
            const char *scene;
            float scale;
            Motion motion;
            const char *tag;
        } Moving;
        static const Moving MOVING[] = {
            {"default", 1.0f, Motion::HUE, "hue"},
            {"default", 0.5f, Motion::HUE, "hue"},
            {"default", 0.25f, Motion::HUE, "hue"},
            {"segments", 0.5f, Motion::HUE, "hue"},
            {"default", 0.5f, Motion::INTENSITY, "intensity"},
            {"arcs", 0.5f, Motion::ARC_WIPE, "arc-wipe"},
            {"segments", 0.5f, Motion::SEGMENT_TRAVEL, "segment-travel"},
        };
        for (const Moving &m : MOVING)
        {
            Config c = NeonScene(m.scene);
            c.neon.resolutionScale = m.scale;
            if (m.motion == Motion::HUE)
            {
                c.neon.hueRotationRate = 0.5f;
            }
            add(std::string("neon/") + m.scene + "@" + ScaleTag(m.scale) + "+" + m.tag, c, m.motion);
        }

        for (float scale : {1.0f, 0.5f})
        {
            Config c = NeonScene("default");
            AddFlare(c, scale);
            add("flare@" + ScaleTag(scale), c, Motion::STILL);
        }
        for (float scale : {1.0f, 0.5f})
        {
            Config c = NeonScene("default");
            AddSpotlightFan(c, scale);
            add("spotlight-fan@" + ScaleTag(scale), c, Motion::STILL);
        }
        {
            // A clipped lamp pins the scale to 1.0 (HasClippedLamp).
            Config c = NeonScene("default");
            AddSpotlightFan(c, 0.5f);
            c.spotlight.lamps[2].clipped = true;
            c.spotlight.clipArea.position = c.geometry.position;
            c.spotlight.clipArea.width = c.geometry.width;
            c.spotlight.clipArea.height = c.geometry.height;
            c.spotlight.clipArea.cornerRadius = c.geometry.cornerRadius;
            add("spotlight-clipped@0.5", c, Motion::STILL);
        }
        {
            Config c = NeonScene("default");
            c.droplets.enable = true;
            add("droplets", c, Motion::STILL);
        }
        {
            Config c = NeonScene("default");
            AddDebugOverlays(c);
            add("debug-overlays", c, Motion::STILL);
            c.neon.colorTransitionDuration = 0.5f;
            add("debug-overlays+stop-fade", c, Motion::STOP_CHANGE);
        }
        add("all@0.5+hue", AllLayers(), Motion::HUE);
        return scenes;
    }

    Config FrameConfig(const Config &start, Motion motion, int frame)
    {
        Config c = start;
        const float f = float(frame);
        switch (motion)
        {
        case Motion::INTENSITY:
        {
            c.neon.intensity = start.neon.intensity * (1.0f + 0.1f * std::sin(0.1f * f));
            break;
        }
        case Motion::ARC_WIPE:
        {
            if (!c.neon.arcs.empty())
            {
                c.neon.arcs[0].length = 0.3f + 0.6f * (0.5f + 0.5f * std::sin(0.05f * f));
            }
            break;
        }
        case Motion::SEGMENT_TRAVEL:
        {
            if (!c.neon.segmentBoosts.empty())
            {
                c.neon.segmentBoosts[0].position = std::fmod(start.neon.segmentBoosts[0].position + 0.003f * f, 1.0f);
            }
            break;
        }
        case Motion::STOP_CHANGE:
        {
            if (frame >= 1)
            {
                c.neon.colorStops = {NeonScaleCheck::Stop(0.0f, 1.0f, 0.4f, 0.1f),
                                     NeonScaleCheck::Stop(0.5f, 0.2f, 1.0f, 0.4f)};
            }
            break;
        }
        default:
        {
            break;
        }
        }
        return c;
    }

    bool ChangesConfigPerFrame(Motion motion)
    {
        return motion != Motion::STILL && motion != Motion::HUE;
    }

    // -------------------------------------------------------------------------
    // Running a scene.
    // -------------------------------------------------------------------------
    void CreateEffect(EdgeLightingEffect &effect)
    {
        for (RendererLayer layer : EdgeLightingEffect::GetDefaultLayerOrder())
        {
            effect.AddRenderer(layer);
        }
        effect.Initialize();
    }

    uint64_t Fnv1a(const std::vector<unsigned char> &bytes)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char b : bytes)
        {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }

    CaptureUtil::Image RenderFrame(EdgeLightingEffect &effect)
    {
        OffscreenCapture capture;
        capture.Begin(WIDTH, HEIGHT);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        effect.Render(WIDTH, HEIGHT);
        CaptureUtil::Image image;
        capture.Read(image);
        capture.End();
        return image;
    }

    typedef struct SceneResult
    {
        std::vector<uint64_t> hashes;
        size_t textureBytes = 0;
        size_t bufferBytes = 0;
        std::vector<long> allocations; ///< Per counted frame.
        std::vector<std::string> published;
    } SceneResult;

    /// A record as the reports name it: its Framebuffer's name, "#", its
    /// attachment - "NeonRenderer.Gather#1" for the key
    /// "NeonRenderer.Gather[1]". The form S0's reports used, kept so reports
    /// from before and after the registry's string keys stay comparable.
    std::string RecordId(const BufferRecord &record)
    {
        std::string id(record.key);
        if (record.attachment > 0)
        {
            id.erase(id.rfind('['));
        }
        return id + "#" + std::to_string(record.attachment);
    }

    SceneResult RunScene(const LayerScene &scene)
    {
        SceneResult result;
        EdgeLightingEffect effect;
        CreateEffect(effect);
        effect.SetConfig(scene.config);
        const bool perFrame = ChangesConfigPerFrame(scene.motion);
        for (int f = 0; f < HASHED_FRAMES; ++f)
        {
            if (perFrame)
            {
                effect.SetConfig(FrameConfig(scene.config, scene.motion, f));
            }
            effect.Update(DT);
            result.hashes.push_back(Fnv1a(RenderFrame(effect).pixels));
        }

        result.textureBytes = LiveTextureBytes();
        result.bufferBytes = LiveBufferBytes();
        const BufferRegistry &registry = effect.GetBufferRegistry();
        for (const BufferRecord &record : registry.GetRecords())
        {
            if (!registry.IsCurrent(record))
            {
                continue;
            }
            const glm::ivec2 texels = record.GetTexels();
            char line[160];
            std::snprintf(line, sizeof(line), "%s %dx%d ch%d", RecordId(record).c_str(), texels.x, texels.y,
                          record.GetChannels());
            result.published.push_back(line);
        }

        // Steady frames, counted without the capture's own read-back: the
        // configs are built before the window opens, so only the library's
        // allocations count.
        std::vector<Config> configs;
        for (int f = 0; f < COUNTED_FRAMES; ++f)
        {
            configs.push_back(FrameConfig(scene.config, scene.motion, HASHED_FRAMES + f));
        }
        OffscreenCapture capture;
        capture.Begin(WIDTH, HEIGHT);
        result.allocations.reserve(COUNTED_FRAMES);
        for (int f = 0; f < COUNTED_FRAMES; ++f)
        {
            gNewCount = 0;
            gCountingNews = true;
            if (perFrame)
            {
                effect.SetConfig(configs[size_t(f)]);
            }
            effect.Update(DT);
            effect.Render(WIDTH, HEIGHT);
            gCountingNews = false;
            result.allocations.push_back(gNewCount);
        }
        capture.End();
        return result;
    }

    // -------------------------------------------------------------------------
    // Modes.
    // -------------------------------------------------------------------------
    int Report(const char *outPath, const char *only)
    {
        std::ostringstream out;
        out << "# layer-check report\n";
        out << "# GL renderer: " << RendererName() << "\n";
        out << "# frame " << WIDTH << "x" << HEIGHT << ", " << HASHED_FRAMES << " hashed frames per scene at dt 1/60, "
            << COUNTED_FRAMES << " counted\n";

        std::vector<LayerScene> scenes = BuildScenes();

        // One scene per buffer the all-layers frame publishes: the debug view
        // on it, drawn over that same frame.
        {
            EdgeLightingEffect effect;
            CreateEffect(effect);
            const Config all = AllLayers();
            effect.SetConfig(all);
            for (int f = 0; f < 3; ++f)
            {
                effect.Update(DT);
                RenderFrame(effect);
            }
            const BufferRegistry &registry = effect.GetBufferRegistry();
            for (const BufferRecord &record : registry.GetRecords())
            {
                if (!registry.IsCurrent(record))
                {
                    continue;
                }
                LayerScene s;
                s.id = "view/" + RecordId(record);
                s.config = all;
                s.config.debug.bufferView = record.key;
                s.motion = Motion::HUE;
                scenes.push_back(s);
            }
        }

        for (const LayerScene &scene : scenes)
        {
            if (only && scene.id.find(only) == std::string::npos)
            {
                continue;
            }
            const SceneResult r = RunScene(scene);
            for (size_t f = 0; f < r.hashes.size(); ++f)
            {
                char hex[24];
                std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(r.hashes[f]));
                out << "hash " << scene.id << " " << f << " " << hex << "\n";
            }
            out << "mem " << scene.id << " " << r.textureBytes << " " << r.bufferBytes << "\n";
            long allocTotal = 0;
            std::string perFrame;
            for (long n : r.allocations)
            {
                allocTotal += n;
                perFrame += " " + std::to_string(n);
            }
            out << "alloc " << scene.id << " " << allocTotal << perFrame << "\n";
            if (scene.id == "all@0.5+hue")
            {
                for (const std::string &p : r.published)
                {
                    out << "published " << p << "\n";
                }
            }
            std::fprintf(stderr, "layer-check: %s\n", scene.id.c_str());
        }

        if (outPath)
        {
            std::ofstream file(outPath);
            file << out.str();
            if (!file)
            {
                std::fprintf(stderr, "layer-check: could not write %s\n", outPath);
                return 2;
            }
        }
        else
        {
            std::fputs(out.str().c_str(), stdout);
        }
        return 0;
    }

    typedef struct ParsedReport
    {
        std::map<std::string, std::string> hashes; ///< "scene frame" -> hex
        std::map<std::string, std::pair<long long, long long>> memory;
        std::map<std::string, long> allocations;
        std::string renderer;
    } ParsedReport;

    bool Parse(const char *path, ParsedReport &report)
    {
        std::ifstream file(path);
        if (!file)
        {
            std::fprintf(stderr, "layer-check: cannot read %s\n", path);
            return false;
        }
        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream in(line);
            std::string kind;
            in >> kind;
            if (kind == "hash")
            {
                std::string scene, frame, hex;
                in >> scene >> frame >> hex;
                report.hashes[scene + " " + frame] = hex;
            }
            else if (kind == "mem")
            {
                std::string scene;
                long long tex = 0, buf = 0;
                in >> scene >> tex >> buf;
                report.memory[scene] = {tex, buf};
            }
            else if (kind == "alloc")
            {
                std::string scene;
                long n = 0;
                in >> scene >> n;
                report.allocations[scene] = n;
            }
            else if (line.rfind("# GL renderer: ", 0) == 0)
            {
                report.renderer = line.substr(15);
            }
        }
        return true;
    }

    int Diff(const char *pathA, const char *pathB)
    {
        ParsedReport a, b;
        if (!Parse(pathA, a) || !Parse(pathB, b))
        {
            return 2;
        }
        if (a.renderer != b.renderer)
        {
            std::printf("warning: different GPUs (%s / %s) - hashes are not comparable\n", a.renderer.c_str(),
                        b.renderer.c_str());
        }
        int differing = 0;
        int missing = 0;
        for (const auto &entry : a.hashes)
        {
            auto it = b.hashes.find(entry.first);
            if (it == b.hashes.end())
            {
                std::printf("missing in B: %s\n", entry.first.c_str());
                ++missing;
            }
            else if (it->second != entry.second)
            {
                std::printf("differs: %s\n", entry.first.c_str());
                ++differing;
            }
        }
        for (const auto &entry : b.hashes)
        {
            if (a.hashes.count(entry.first) == 0)
            {
                std::printf("missing in A: %s\n", entry.first.c_str());
                ++missing;
            }
        }
        for (const auto &entry : a.memory)
        {
            auto it = b.memory.find(entry.first);
            if (it != b.memory.end() && it->second != entry.second)
            {
                std::printf("memory: %s textures %lld -> %lld, buffers %lld -> %lld\n", entry.first.c_str(),
                            entry.second.first, it->second.first, entry.second.second, it->second.second);
            }
        }
        for (const auto &entry : a.allocations)
        {
            auto it = b.allocations.find(entry.first);
            if (it != b.allocations.end() && it->second != entry.second)
            {
                std::printf("allocations: %s %ld -> %ld over %d frames\n", entry.first.c_str(), entry.second,
                            it->second, COUNTED_FRAMES);
            }
        }
        std::printf("%zu frames compared: %d differ, %d missing\n", a.hashes.size(), differing, missing);
        std::printf("%s\n", differing == 0 && missing == 0 ? "PASS" : "FAIL");
        return differing == 0 && missing == 0 ? 0 : 1;
    }

    int Startup(const char *failStage)
    {
        if (failStage)
        {
            gFailLabel = failStage;
        }
        const auto t0 = std::chrono::high_resolution_clock::now();
        EdgeLightingEffect effect;
        CreateEffect(effect);
        glFinish();
        const double initMs = MsSince(t0);
        const GLStats atInit = gStats;

        // Every layer on, with segments and the hue rotating (the field's
        // programs), the debug view on a buffer (its program): the frame that
        // builds every program a host can need, but the opaque fill's.
        Config c = AllLayers();
        c.debug.bufferView = "NeonRenderer.Scaled";
        effect.SetConfig(c);
        const auto t1 = std::chrono::high_resolution_clock::now();
        effect.Update(DT);
        CaptureUtil::Image first = RenderFrame(effect);
        glFinish();
        const double firstMs = MsSince(t1);
        const GLStats atFirst = gStats;
        CaptureUtil::Image last;
        for (int f = 0; f < 5; ++f)
        {
            effect.Update(DT);
            last = RenderFrame(effect);
        }
        glFinish();

        long lit = 0;
        for (size_t i = 0; i + 3 < last.pixels.size(); i += 4)
        {
            if (last.pixels[i] | last.pixels[i + 1] | last.pixels[i + 2] | last.pixels[i + 3])
            {
                ++lit;
            }
        }

        std::printf("GL renderer: %s\n", RendererName().c_str());
        if (failStage)
        {
            std::printf("forced to fail: %s\n", failStage);
        }
        std::printf("layers after Initialize: %zu of %zu\n", effect.GetLayerOrder().size(),
                    EdgeLightingEffect::GetDefaultLayerOrder().size());
        std::printf("initialize_ms %.2f  (compiles %ld, %.2f ms; links %ld, %.2f ms)\n", initMs, atInit.compiles,
                    atInit.compileMs, atInit.links, atInit.linkMs);
        std::printf("first_frame_ms %.2f  (compiles %ld, %.2f ms; links %ld, %.2f ms)\n", firstMs,
                    atFirst.compiles - atInit.compiles, atFirst.compileMs - atInit.compileMs,
                    atFirst.links - atInit.links, atFirst.linkMs - atInit.linkMs);
        std::printf("total: compiles %ld, links %ld\n", gStats.compiles, gStats.links);
        std::printf("compiles by source:\n");
        for (const auto &entry : gStats.compilesByLabel)
        {
            std::printf("  %-34s %d\n", entry.first.c_str(), entry.second);
        }
        std::printf("last frame: %ld of %d px drawn\n", lit, WIDTH * HEIGHT);
        (void)first;
        return 0;
    }

    int Usage()
    {
        std::fprintf(stderr,
                     "usage: layer-check report [--out FILE] [--only SUBSTRING]\n"
                     "       layer-check diff A B\n"
                     "       layer-check startup [--fail-stage NAME]   (NAME: a ShaderSource constant, e.g. NEON_VERT_SRC)\n");
        return 2;
    }
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        return Usage();
    }
    const std::string mode = argv[1];
    if (mode == "diff")
    {
        return argc == 4 ? Diff(argv[2], argv[3]) : Usage();
    }

    const char *outPath = nullptr;
    const char *failStage = nullptr;
    const char *only = nullptr;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
        {
            outPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--fail-stage") == 0 && i + 1 < argc)
        {
            failStage = argv[++i];
        }
        else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
        {
            only = argv[++i];
        }
        else
        {
            return Usage();
        }
    }

    InitGL();
    int rc = 2;
    if (mode == "report")
    {
        rc = Report(outPath, only);
    }
    else if (mode == "startup")
    {
        rc = Startup(failStage);
    }
    else
    {
        rc = Usage();
    }
    glfwDestroyWindow(gWindow);
    glfwTerminate();
    return rc;
}
