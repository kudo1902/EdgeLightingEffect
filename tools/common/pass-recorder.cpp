#include "pass-recorder.h"

#include "gl/shader-program.h"
#include "shaders.h"

#include <glm/gtc/type_ptr.hpp>
#include <memory>

namespace NeonTools
{
    namespace
    {
        PFNGLDRAWARRAYSPROC gRealDrawArrays = nullptr;
        bool gRecording = false;
        bool gReadback = true;
        GLint gCallerFramebuffer = 0;
        std::vector<DrawRecord> gDraws;

        /// The flat-colour program Redraw uses. neon.vert itself, so a redrawn
        /// triangle lands on exactly the pixels the pass's own did.
        std::unique_ptr<EdgeLighting::ShaderProgram> gFlatProgram;

        const char *const FLAT_FRAG_SRC = R"(#version 330 core
precision highp float;
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

        bool HasUniform(GLuint program, const char *name)
        {
            return glGetUniformLocation(program, name) >= 0;
        }

        /// Name the pass from what its program declares. Every neon fragment
        /// shader has a uniform no other one has, except the three built from
        /// neon.frag's source - those differ by target, and offscreen by the
        /// scale they were uploaded at.
        PassKind Classify(GLuint program, bool ontoCaller)
        {
            if (HasUniform(program, "uSource"))
            {
                return PassKind::P2B;
            }
            if (HasUniform(program, "uOpaqueMode"))
            {
                return PassKind::P2A;
            }
            // Before uGather: the field's composite reads the gather too. The
            // edge ring's composite (NEON_FIELD_RING) draws the ring's pixels
            // in pass 2c's place, so it is filed as the ring.
            if (HasUniform(program, "uRingHole"))
            {
                return PassKind::P2C;
            }
            if (HasUniform(program, "uField"))
            {
                return PassKind::P1C;
            }
            if (HasUniform(program, "uGather"))
            {
                if (ontoCaller)
                {
                    return PassKind::P2C;
                }
                // Offscreen, one source twice over: pass 1b, into the RGBA
                // reduced buffer, or the field bake, into its R16F / RG16F
                // field - at either scale, so told apart by the target. By its
                // ALPHA, not by float: the reduced buffer is half float too
                // wherever the driver renders to it (SCALED_FORMATS, R7).
                GLint alphaBits = 0;
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                      GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alphaBits);
                return (alphaBits == 0) ? PassKind::P1F : PassKind::P1B;
            }
            // The gather is the one program that reads the emission table.
            if (HasUniform(program, "uEmission"))
            {
                return PassKind::P1A;
            }
            if (!ontoCaller && HasUniform(program, "uArcLUT"))
            {
                return PassKind::P0;
            }
            if (!ontoCaller && HasUniform(program, "uHaloWidth"))
            {
                return PassKind::P0B;
            }
            return PassKind::OTHER;
        }

        /// Every colour attachment of @p framebuffer, as floats. Restores the
        /// read binding, its read buffer, the 2D texture binding and the pack
        /// alignment, so the renderer's next pass sees nothing changed.
        std::vector<Attachment> ReadAttachments(GLint framebuffer)
        {
            std::vector<Attachment> out;
            if (framebuffer == 0)
            {
                return out;
            }
            GLint prevRead = 0;
            GLint prevPack = 0;
            GLint prevTexture = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(framebuffer));
            GLint prevReadBuffer = GL_COLOR_ATTACHMENT0;
            glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);

            for (int i = 0; i < 4; ++i)
            {
                const GLenum point = GLenum(GL_COLOR_ATTACHMENT0 + i);
                GLint type = GL_NONE;
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, point,
                                                      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
                if (type != GL_TEXTURE)
                {
                    break;
                }
                GLint name = 0;
                glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, point,
                                                      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
                Attachment a;
                glBindTexture(GL_TEXTURE_2D, GLuint(name));
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &a.width);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &a.height);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &a.internalFormat);
                a.texels.resize(size_t(a.width) * size_t(a.height));
                glReadBuffer(point);
                glReadPixels(0, 0, a.width, a.height, GL_RGBA, GL_FLOAT, glm::value_ptr(a.texels[0]));
                out.push_back(std::move(a));
            }

            glReadBuffer(GLenum(prevReadBuffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevRead));
            glBindTexture(GL_TEXTURE_2D, GLuint(prevTexture));
            glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
            return out;
        }

        void APIENTRY HookDrawArrays(GLenum mode, GLint first, GLsizei count)
        {
            gRealDrawArrays(mode, first, count);
            if (!gRecording)
            {
                return;
            }

            GLint program = 0;
            GLint drawFramebuffer = 0;
            GLint vertexArray = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);

            DrawRecord record;
            record.kind = Classify(GLuint(program), drawFramebuffer == gCallerFramebuffer);
            if (record.kind == PassKind::OTHER)
            {
                return;
            }
            record.program = GLuint(program);
            record.vertexArray = GLuint(vertexArray);
            record.mode = mode;
            record.first = first;
            record.count = count;
            const GLint mvp = glGetUniformLocation(GLuint(program), "uMVP");
            if (mvp >= 0)
            {
                glGetUniformfv(GLuint(program), mvp, glm::value_ptr(record.mvp));
            }
            if (record.kind == PassKind::P2B)
            {
                glGetUniformfv(GLuint(program), glGetUniformLocation(GLuint(program), "uUVScale"),
                               glm::value_ptr(record.uvScale));
                glGetUniformfv(GLuint(program), glGetUniformLocation(GLuint(program), "uUVOffset"),
                               glm::value_ptr(record.uvOffset));
            }
            if (gReadback)
            {
                record.written = ReadAttachments(drawFramebuffer);
            }
            gDraws.push_back(std::move(record));
        }
    }

    void PassRecorder::Install()
    {
        if (gRealDrawArrays == nullptr)
        {
            gRealDrawArrays = glad_glDrawArrays;
            glad_glDrawArrays = HookDrawArrays;
        }
    }

    void PassRecorder::Release()
    {
        gFlatProgram.reset();
        gDraws.clear();
    }

    void PassRecorder::SetReadback(bool enabled)
    {
        gReadback = enabled;
    }

    void PassRecorder::Begin()
    {
        gDraws.clear();
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &gCallerFramebuffer);
        gRecording = true;
    }

    void PassRecorder::End()
    {
        gRecording = false;
    }

    const std::vector<DrawRecord> &PassRecorder::GetDraws()
    {
        return gDraws;
    }

    const DrawRecord *PassRecorder::Find(PassKind kind)
    {
        for (auto it = gDraws.rbegin(); it != gDraws.rend(); ++it)
        {
            if (it->kind == kind)
            {
                return &*it;
            }
        }
        return nullptr;
    }

    void PassRecorder::Redraw(const DrawRecord &record, const glm::vec4 &colour, bool wire)
    {
        Redraw(record, record.mvp, colour, wire);
    }

    void PassRecorder::Redraw(const DrawRecord &record, const glm::mat4 &mvp, const glm::vec4 &colour, bool wire)
    {
        if (!gFlatProgram)
        {
            gFlatProgram.reset(new EdgeLighting::ShaderProgram(EdgeLighting::ShaderSource::NEON_VERT_SRC, FLAT_FRAG_SRC, "NeonTools.Flat"));
        }
        GLint prevProgram = 0;
        GLint prevVertexArray = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);

        gFlatProgram->Use();
        gFlatProgram->SetUniform("uMVP", mvp);
        gFlatProgram->SetUniform("uColor", colour);
        glBindVertexArray(record.vertexArray);
        glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
        gRealDrawArrays(record.mode, record.first, record.count);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        glBindVertexArray(GLuint(prevVertexArray));
        glUseProgram(GLuint(prevProgram));
    }
}
