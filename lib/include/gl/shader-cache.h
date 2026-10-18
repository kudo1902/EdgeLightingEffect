#ifndef _EDGE_LIGHTING_SHADER_CACHE_H_
#define _EDGE_LIGHTING_SHADER_CACHE_H_

#include "gl/shader-program.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace EdgeLighting
{
    /// Compiled shader stages, each compiled once and linked by every program
    /// that needs it - across renderers, which is what it is for: neon.vert
    /// went into up to 17 programs, each compiling its own copy, and neon.frag
    /// into two (docs/plans/resource-sharing-plan.md, section 1.2).
    ///
    /// Shares STAGES, never programs. Every pass keeps a program object of its
    /// own, since one program drawing an offscreen buffer unblended and the
    /// caller's framebuffer blended in the same frame measured 93-624x slower
    /// on the AMD macOS driver (docs/history/neon-resolution-scale-plan.md section 7,
    /// build AB). The program object is what such a driver keys its
    /// per-target state on; the stage is only the compile.
    ///
    /// Owned by a @c ResourceContext, one per effect. It holds GL objects, so
    /// it is destroyed, like the renderers, with the GL context current.
    class ShaderCache
    {
    public:
        ShaderCache() = default;
        ShaderCache(const ShaderCache &) = delete;
        ShaderCache &operator=(const ShaderCache &) = delete;

        /// The @p type stage compiled from @p source with @p defines spliced
        /// in (@ref WithDefines; nullptr or "" for none), compiled by the first
        /// request for it. A compile that failed is kept too, as an invalid
        /// object: its error was logged once, by that compile, and no later
        /// request - from any renderer - compiles it again. @p programName
        /// only attributes that log.
        ///
        /// A stage is matched by its TEXT, not by @p source's address: every
        /// translation unit that includes shaders.h has a pointer of its own,
        /// and a host renderer may build from a string that is gone by the
        /// next request. The cache keeps neither the pointer nor a copy - the
        /// sources add up to ~700 KB - but the text's length and a 64-bit hash
        /// of it, so @p source need only live through this call. Two different
        /// texts of one length share a hash with odds of about 2^-64.
        const ShaderObject &Get(GLenum type, const char *source, const char *defines, const char *programName)
        {
            const char *names = defines ? defines : "";
            const size_t length = std::strlen(source);
            const uint64_t hash = HashText(source, length);
            for (const std::unique_ptr<Entry> &entry : mEntries)
            {
                if (entry->type == type && entry->length == length && entry->hash == hash && entry->defines == names)
                {
                    return entry->object;
                }
            }

            std::unique_ptr<Entry> entry(new Entry());
            entry->type = type;
            entry->hash = hash;
            entry->length = length;
            entry->defines = names;
            const char *stage = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
            if (*names == '\0')
            {
                entry->object = ShaderObject(type, source, stage, programName);
            }
            else
            {
                const std::string text = WithDefines(source, names);
                entry->object = ShaderObject(type, text.c_str(), stage, programName);
            }
            ++mCompileCount;
            mEntries.push_back(std::move(entry));
            return mEntries.back()->object;
        }

        /// How many stages the cache has compiled: every request it could not
        /// serve from a stage it already held, failed compiles included.
        size_t GetCompileCount() const { return mCompileCount; }

        /// @p src with `#define NAME` spliced in after its first line for each
        /// space-separated NAME in @p defines.
        ///
        /// How one shader file yields several programs. Every embedded source
        /// starts with the @GLSL_VERSION@ line, which must stay first, so the
        /// defines go immediately after it - ahead of everything else, the
        /// injected tuning header and neon-common.glsl included. Today that is
        /// neon.frag's field bake (NEON_FIELD_BAKE) and neon-field.frag's
        /// ring composite (NEON_FIELD_RING); see
        /// @c NeonRenderer::ensureFieldPrograms.
        static std::string WithDefines(const char *src, const char *defines)
        {
            std::string lines;
            const std::string names(defines);
            size_t start = 0;
            while (start < names.size())
            {
                const size_t end = names.find(' ', start);
                const std::string name = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!name.empty())
                {
                    lines += "#define " + name + "\n";
                }
                start = (end == std::string::npos) ? names.size() : end + 1;
            }
            std::string out(src);
            const size_t eol = out.find('\n');
            out.insert(eol == std::string::npos ? out.size() : eol + 1, lines);
            return out;
        }

    private:
        /// 64-bit FNV-1a of @p length chars of @p text.
        static uint64_t HashText(const char *text, size_t length)
        {
            uint64_t hash = 14695981039346656037ull;
            for (size_t i = 0; i < length; ++i)
            {
                hash = (hash ^ static_cast<unsigned char>(text[i])) * 1099511628211ull;
            }
            return hash;
        }

        /// One stage. Held by pointer, so the reference @ref Get returns stays
        /// valid while the vector grows.
        typedef struct Entry
        {
            GLenum type = 0;
            uint64_t hash = 0;   ///< HashText of the source as requested, before the defines.
            size_t length = 0;   ///< strlen of that source, compared before the hash.
            std::string defines; ///< As requested; "" for none.
            ShaderObject object; ///< Invalid if the compile failed.
        } Entry;

        std::vector<std::unique_ptr<Entry>> mEntries;
        size_t mCompileCount = 0;
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_SHADER_CACHE_H_
