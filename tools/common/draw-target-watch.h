#ifndef _NEON_TOOLS_DRAW_TARGET_WATCH_H_
#define _NEON_TOOLS_DRAW_TARGET_WATCH_H_

// Which formats the library's offscreen draws write, seen from the outside -
// for neon-scale-check `formats`, which has to know that no draw lands in a
// format RenderTargetFormats refused.
//
// The same trick as PassRecorder: GLAD reaches glDrawArrays (the only draw
// call the library makes) through a global function pointer, and this puts a
// wrapper in front of it. Unlike PassRecorder it watches EVERY draw, the
// flare's and the spotlight's included, and reads nothing back - only the
// internal format of each colour attachment of the framebuffer a draw is
// about to write, unless that framebuffer is the caller's own. The format is
// derived from the attachment's own parameters (component type, bits per
// channel, colour encoding), so it binds nothing, raises no GL error, and
// sees a renderbuffer or a texture of any target alike. It chains to
// whatever the pointer held when it was installed, so it can sit beside
// PassRecorder. Static library only: the C ABI dylib carries its own glad.

#include "gl/gl-header.h"

#include <map>

namespace NeonTools
{
    class DrawTargetWatch
    {
    public:
        /// Put the wrapper in front of glad_glDrawArrays. Call once, after
        /// gladLoadGLLoader - loading GL again replaces the pointer.
        static void Install();

        /// Watch every draw until @ref End. The framebuffer bound for drawing
        /// at this moment is taken as the caller's, so a draw onto it is not
        /// counted: only the library's own targets are.
        static void Begin();
        static void End();

        /// Forget every count.
        static void Reset();

        /// Since the last @ref Reset: offscreen draws by the internal format
        /// of each colour attachment they wrote (a two-attachment target
        /// counts twice per draw); 0 for an attachment whose component type
        /// and channel sizes name no format the watcher lists.
        static const std::map<GLint, long> &GetFormats();
    };
}

#endif
