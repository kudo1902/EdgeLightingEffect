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
// sees a renderbuffer or a texture of any target alike. It wraps
// glGenTextures the same way, only to keep the highest name made, so a scan
// of the live textures knows where to stop. And glTexImage2D: a texture
// allocated with no data - every buffer the library allocates - starts out
// 0x3C in every byte instead of whatever the driver hands back, which on a
// desktop GL is zeros. The GL leaves those texels undefined, and a device's
// may be another buffer's old contents; filled, a buffer that reads a texel
// it never cleared or drew reads a value a run can see, on any GPU -
// `sequence` and `shared-buffer` install that wrapper alone
// (InstallFreshTextureFill), since they compare buffer texels too. It chains
// to whatever the pointers held when it was installed, so it can sit beside
// PassRecorder. Static library only: the C ABI dylib carries its own glad.

#include "gl/gl-header.h"

#include <map>

namespace NeonTools
{
    class DrawTargetWatch
    {
    public:
        /// Put the wrappers in front of glad_glDrawArrays, glad_glGenTextures
        /// and glad_glTexImage2D - from here on every texture allocated with
        /// no data starts out filled (see above). Call once, after
        /// gladLoadGLLoader - loading GL again replaces the pointers.
        static void Install();

        /// The glTexImage2D wrapper alone: every texture allocated with no
        /// data starts out filled, and nothing is watched. For a check that
        /// compares buffer texels exactly - `sequence`, `shared-buffer` -
        /// where a texel nobody wrote would otherwise read the zeros a
        /// desktop GL hands back in every run alike. Safe to call more than
        /// once, and before or after @ref Install.
        static void InstallFreshTextureFill();

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

        /// The highest texture name glGenTextures has returned since
        /// @ref Install, watched or not - how far a scan of the live textures
        /// has to look.
        static GLuint GetHighestTextureName();
    };
}

#endif
