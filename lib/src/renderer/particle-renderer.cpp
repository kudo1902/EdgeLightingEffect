#include "renderer/particle-renderer.h"
#include "util/color-utils.h"
#include "util/constants.h"
#include "util/geometry-utils.h"
#include "util/log-util.h"
#include "shaders.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace EdgeLighting
{
    namespace
    {
        // Deterministic xorshift32 RNG so the spark pattern is reproducible
        // across runs without pulling in a heavier PRNG or std::random.
        struct Rng
        {
            uint32_t s = 0x13375eedU;
            uint32_t Next()
            {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                return s;
            }
            /// Uniform in [0, 1).
            float Uniform() { return static_cast<float>(Next() & 0x00FFFFFFu) / 16777216.0f; }
            /// Uniform in [-1, 1).
            float Signed() { return Uniform() * 2.0f - 1.0f; }
        };
        Rng gRng;
    }

    bool ParticleRenderer::Initialize()
    {
        if (!setupShaders())
        {
            LOG_E("Failed to compile/link ParticleRenderer shaders.");
            return false;
        }
        // Reserve a bit of scratch so the first frame doesn't allocate.
        mParticles.reserve(1024);
        mVboScratch.reserve(1024 * 7);
        return true;
    }

    void ParticleRenderer::Update(float, float, const Config &)
    {
        // Age + emit is done in Render() where we also have `time` for dt.
    }

    void ParticleRenderer::Render(int viewportWidth, int viewportHeight, float time, const Config &config)
    {
        if (!config.particle.enable)
        {
            // Freeze the pool; if the user disables mid-run we keep the
            // particles idling. Re-enable and they resume ageing normally.
            mLastTime = time;
            return;
        }

        // Compute dt from the effect clock so pausing the clock also pauses
        // the sim (the demo pauses by not advancing `time`).
        float dt = 0.0f;
        if (mLastTime >= 0.0f)
        {
            dt = std::max(0.0f, time - mLastTime);
        }
        mLastTime = time;
        // Guard against jumbo dt spikes (window drag / paused frame that then
        // resumes) so particles don't teleport off-screen.
        dt = std::min(dt, 0.1f);

        agingStep(dt, config.particle.drag);
        emitFromArcs(config, dt, time);
        if (mParticles.empty())
        {
            return;
        }

        // --- MVP: same convention as NeonRenderer (rect-centered local coords). ---
        float halfRectW = config.geometry.width * 0.5f;
        float halfRectH = config.geometry.height * 0.5f;
        glm::mat4 proj = glm::ortho(0.0f, static_cast<float>(viewportWidth),
                                    0.0f, static_cast<float>(viewportHeight),
                                    -1.0f, 1.0f);
        glm::vec2 center(config.geometry.position.x + halfRectW,
                         static_cast<float>(viewportHeight) - config.geometry.position.y - halfRectH);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
        glm::mat4 mvp = proj * model;

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

#if !defined(PLATFORM_LINUX) && !defined(PLATFORM_WINDOWS)
        // Desktop GL needs this to let the vertex shader write gl_PointSize.
        // GLES 3.0 has it enabled implicitly.
        glEnable(GL_PROGRAM_POINT_SIZE);
#endif

        mShaderProgram.Use();
        mShaderProgram.SetUniform("uMVP", mvp);
        uploadAndDraw();
        mShaderProgram.Unuse();
    }

    void ParticleRenderer::OnConfigChanged(const Config & /*config*/)
    {
        // Nothing that needs a full rebuild — every knob is consumed inline.
    }

    bool ParticleRenderer::setupShaders()
    {
        mShaderProgram = ShaderProgram(ShaderSource::PARTICLE_VERT_SRC,
                                       ShaderSource::PARTICLE_FRAG_SRC,
                                       "ParticleRenderer");

        constexpr GLsizei stride = 7 * sizeof(float);
        // aPos (loc 0): 2 floats.
        mVertexArray.SetAttribPointer(0, 2, GL_FLOAT, stride, 0);
        // aColor (loc 1): 3 floats after pos.
        mVertexArray.SetAttribPointer(1, 3, GL_FLOAT, stride, 2 * sizeof(float));
        // aLife (loc 2): 1 float after colour.
        mVertexArray.SetAttribPointer(2, 1, GL_FLOAT, stride, 5 * sizeof(float));
        // aSize (loc 3): 1 float last.
        mVertexArray.SetAttribPointer(3, 1, GL_FLOAT, stride, 6 * sizeof(float));

        return mShaderProgram.IsValid();
    }

    void ParticleRenderer::agingStep(float dt, float drag)
    {
        if (dt <= 0.0f)
        {
            return;
        }
        float velScale = std::exp(-drag * dt);
        for (auto &p : mParticles)
        {
            p.pos += p.vel * dt;
            p.vel *= velScale;
            p.life -= dt;
        }
        mParticles.erase(
            std::remove_if(mParticles.begin(), mParticles.end(),
                           [](const Particle &p) { return p.life <= 0.0f; }),
            mParticles.end());
    }

    void ParticleRenderer::emitFromArcs(const Config &config, float dt, float time)
    {
        const auto &p = config.particle;
        if (p.emissionRate <= 0.0f || dt <= 0.0f)
        {
            return;
        }

        // Collect emit sites from NeonConfig's arc pair (arcStart/arcLength and
        // the optional arcStart2/arcLength2). Each arc contributes 0/1/2 sites
        // (tail, head).
        auto pushArcSites = [&](float start, float length,
                                std::vector<float> &sites)
        {
            if (length <= 0.001f) { return; }
            if (p.emitFromHead)
            {
                float head = start + length;
                head -= std::floor(head);
                sites.push_back(head);
            }
            if (p.emitFromTail)
            {
                float tail = start - std::floor(start);
                sites.push_back(tail);
            }
        };

        std::vector<float> sites;
        sites.reserve(2);
        pushArcSites(config.neon.arcStart, config.neon.arcLength, sites);
        if (sites.empty())
        {
            return;
        }
        if (mSpawnAcc.size() != sites.size())
        {
            mSpawnAcc.assign(sites.size(), 0.0f);
        }

        for (size_t i = 0; i < sites.size(); ++i)
        {
            mSpawnAcc[i] += p.emissionRate * dt;
            int toSpawn = static_cast<int>(mSpawnAcc[i]);
            mSpawnAcc[i] -= static_cast<float>(toSpawn);
            for (int n = 0; n < toSpawn; ++n)
            {
                if (static_cast<int>(mParticles.size()) >= p.maxParticles)
                {
                    return;
                }
                spawnAt(config, sites[i], time);
            }
        }
    }

    void ParticleRenderer::spawnAt(const Config &config, float perimeterPos, float time)
    {
        const auto &p = config.particle;

        // Spawn position in local (rect-centered) coords.
        glm::vec2 pos = GeometryUtils::GetPointOnRectangle(perimeterPos, config.geometry);

        // Outward direction from the rect centre — the perimeter point IS its
        // own offset from centre in local coords, so normalising gives us the
        // outward normal at that point.
        glm::vec2 outward = glm::length(pos) > 1e-3f ? glm::normalize(pos) : glm::vec2(0.0f, 1.0f);
        // Tangent = 90° rotation of outward.
        glm::vec2 tangent(-outward.y, outward.x);
        // A random sign so tangential drift goes both ways.
        if (gRng.Uniform() < 0.5f) { tangent = -tangent; }

        glm::vec2 emitDir = glm::normalize(
            glm::mix(tangent, outward, p.outwardBias));

        // Rotate emitDir by a random angle within the spread cone.
        float spreadRad = glm::radians(p.spreadDeg) * 0.5f;
        float ang = gRng.Signed() * spreadRad;
        float cs  = std::cos(ang);
        float sn  = std::sin(ang);
        glm::vec2 vel(cs * emitDir.x - sn * emitDir.y,
                      sn * emitDir.x + cs * emitDir.y);
        vel *= p.speed * (0.5f + gRng.Uniform() * 0.5f);

        // Colour from the neon gradient at the emit position, matching the
        // same time-rotated colour the neon shader is showing at that point.
        // Without this rotation the sparks colour would stay fixed to the
        // static gradient position, diverging from the filament when
        // hueRotationRate != 0 (e.g. blue sparks on a red ring).
        glm::vec3 col(1.0f, 0.7f, 0.3f);
        if (!config.neon.colorStops.empty())
        {
            float rotatedPos = perimeterPos - time * config.neon.hueRotationRate;
            rotatedPos -= std::floor(rotatedPos);
            col = ColorUtils::SampleStops(rotatedPos, config.neon.colorStops,
                                           config.neon.blendSpace);
        }

        Particle np;
        np.pos       = pos;
        np.vel       = vel;
        np.life      = p.lifetime;
        np.lifeMax   = p.lifetime;
        np.color     = col;
        np.sizeStart = p.sizeStart;
        mParticles.push_back(np);
    }

    void ParticleRenderer::uploadAndDraw()
    {
        // Pack into interleaved floats: pos.xy, rgb, life, size.
        mVboScratch.clear();
        mVboScratch.reserve(mParticles.size() * 7);
        for (const auto &p : mParticles)
        {
            float lifeFrac = p.lifeMax > 0.0f ? std::max(0.0f, p.life / p.lifeMax) : 0.0f;
            mVboScratch.push_back(p.pos.x);
            mVboScratch.push_back(p.pos.y);
            mVboScratch.push_back(p.color.r);
            mVboScratch.push_back(p.color.g);
            mVboScratch.push_back(p.color.b);
            mVboScratch.push_back(lifeFrac);
            mVboScratch.push_back(p.sizeStart);
        }

        mVertexArray.SetVertexData(mVboScratch.data(),
                                   mVboScratch.size() * sizeof(float),
                                   GL_DYNAMIC_DRAW);
        mVertexArray.DrawArrays(GL_POINTS, static_cast<GLint>(mParticles.size()));
    }

} // namespace EdgeLighting
