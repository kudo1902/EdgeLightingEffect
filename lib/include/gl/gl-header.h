#ifndef _EDGE_LIGHTING_GL_HEADER_H_
#define _EDGE_LIGHTING_GL_HEADER_H_

// Single include for OpenGL symbols. Which header / loader is used is decided
// at compile time by the EL_PLATFORM_* define set in lib/CMakeLists.txt.
//
//   macOS    → desktop GL 3.3 Core, GLAD loader   (<glad/glad.h>)
//   Windows  → GLES 3.0 via GLAD-GLES2 + ANGLE    (<glad/glad.h>)
//   Linux    → native GLES 3.0, no loader         (<GLES3/gl3.h>)
//
// All three configurations expose the same gl* symbols we use; pick at link
// time via the corresponding glad.c / system library.

#if defined(PLATFORM_LINUX)
#include <GLES3/gl3.h>
#define GLSL_VERSION "#version 300 es"
#elif defined(PLATFORM_WINDOWS)
#include <glad/glad.h>
#define GLSL_VERSION "#version 300 es"
#elif defined(PLATFORM_MACOS)
#include <glad/glad.h>
#define GLSL_VERSION "#version 330 core"
#else
#error "Unknown platform — set PLATFORM_MACOS / _WINDOWS / _LINUX via CMake"
#endif

#endif // _EDGE_LIGHTING_GL_HEADER_H_
