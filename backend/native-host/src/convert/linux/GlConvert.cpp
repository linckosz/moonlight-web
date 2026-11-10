/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "GlConvert.h"

#include "../../core/Log.h"
#include "../../platform/linux/ScopedCapability.h"
#include "../../platform/macos/FrameFit.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/capability.h>
#include <unistd.h>

// EGL_IMG_context_priority. eglext.h has carried the names since 2016; these
// are for a header older than that — the values are the extension's.
#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG 0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG 0x3103
#endif

// gl2ext.h defines nothing of its own: it needs GL_APIENTRY and the GL types
// from a core header first, and an alphabetical sort puts it before gl3.h. Kept
// in its own block so the formatter leaves the order alone.
// clang-format off
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mw::native::convert {
namespace {

// ── The conversion, in GLSL ES 3.00 ────────────────────────────────────────
//
// ColorConvert.cpp's HLSL, transcribed. BT.709, limited ("TV") range — the
// same colour space the SDR stream negotiates on every platform, so a Linux host
// and a Windows host look identical on the same client.
//
// One difference is not cosmetic: the vertex shader does NOT flip Y. D3D11's
// render targets have their first row at the top; a GL framebuffer backed by a
// DMA-BUF has its first row at NDC y = -1. Mapping uv (0,0) to (-1,-1) writes
// the source's first memory row into the target's first memory row, which is
// what the encoder — and the DMA-BUF the compositor wrote — both mean by "top".
constexpr char kVertex[] = R"GLSL(#version 300 es
out vec2 uv;
void main() {
    uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr char kCommon[] = R"GLSL(#version 300 es
precision highp float;
in vec2 uv;
uniform sampler2D Source;
uniform sampler2D CursorPixels;
uniform sampler2D CursorInvert;
// xy: the cursor's top-left in source UV. zw: its size in source UV.
uniform vec4 CursorRect;
uniform float CursorEnabled;
// xy: the picture's top-left in the texture's UV. zw: its size. (0,0,1,1) but
// for a screen of an X11 desktop over several, which reads its window of the
// root window's buffer (KmsFrame::sourceX).
uniform vec4 SourceRect;
// How the texture holds the picture (KmsFrame::orientation): x the quarter
// turns counter-clockwise, y 1 when mirrored first. (0,0) but for a display
// turned against its panel (issue #54), and for the resampled picture, which
// the horizontal pass has stood upright already.
uniform ivec2 Orient;

// Where the texture holds point p of the upright picture, in the picture's
// window: Orientation.h's orientedTexel, in uv. Everything else — the
// pointer's rectangle above all — is in the upright picture. A branch on a
// uniform: every pixel of a draw takes the same one.
vec2 PictureUv(vec2 p) {
    if (Orient.y == 1) p.x = 1.0 - p.x;
    if (Orient.x == 1) return vec2(p.y, 1.0 - p.x);
    if (Orient.x == 2) return 1.0 - p;
    if (Orient.x == 3) return vec2(1.0 - p.y, p.x);
    return p;
}

// The desktop with the mouse pointer drawn on it. See ColorConvert.cpp: the
// scanout buffer does not contain the pointer (it is on its own hardware
// plane), so it is put back here, costing one fetch on the pixels it covers.
vec3 Scene(vec2 p) {
    // Held half a texel inside the picture, so a bilinear read at its edge
    // never takes a pixel of the screen beside it; for a whole texture that
    // is the edge clamp the sampler already does.
    vec2 texel = 1.0 / vec2(textureSize(Source, 0));
    vec2 q = clamp(SourceRect.xy + PictureUv(p) * SourceRect.zw, SourceRect.xy + 0.5 * texel,
                   SourceRect.xy + SourceRect.zw - 0.5 * texel);
    vec3 rgb = texture(Source, q).rgb;
    if (CursorEnabled < 0.5) return rgb;
    vec2 c = (p - CursorRect.xy) / CursorRect.zw;
    if (c.x < 0.0 || c.y < 0.0 || c.x > 1.0 || c.y > 1.0) return rgb;
    if (texture(CursorInvert, c).r > 0.5) return 1.0 - rgb;
    vec4 cursor = texture(CursorPixels, c);
    return mix(rgb, cursor.rgb, cursor.a);
}

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
const float kLumaScale = 219.0 / 255.0;
const float kLumaBias = 16.0 / 255.0;
const float kChromaScale = 224.0 / 255.0;
const float kChromaBias = 128.0 / 255.0;
)GLSL";

constexpr char kLumaMain[] = R"GLSL(
out float o;
void main() {
    o = dot(Scene(uv), kLuma) * kLumaScale + kLumaBias;
}
)GLSL";

constexpr char kChromaMain[] = R"GLSL(
out vec2 o;
void main() {
    vec3 rgb = Scene(uv);
    float y = dot(rgb, kLuma);
    // The BT.709 denominators: 2*(1-Kb) and 2*(1-Kr).
    o = vec2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748) * kChromaScale + kChromaBias;
}
)GLSL";

// ── The resample pass: Lanczos-2 dilated to the ratio, separable ────────────
//
// ColorConvert.cpp's kScaleShaderSource, transcribed (see there for the why:
// the bench of 17/09/2026). Two 1-D passes, in linear light: the scanout is
// decoded per tap on the way in, and — GLES having no free sRGB encode on an
// imported target the way D3D11's _SRGB view gives one — the vertical pass
// re-encodes its result itself, once per output pixel, into a plain RGBA8 the
// conversion then reads as it read the scanout. The sizes are #defines
// prepended per geometry, so the tap loop is a constant. The horizontal pass
// also stands a picture held turned upright (Orient, see Texel), so that
// everything after it — the vertical pass, the conversion — reads it upright.
constexpr char kScaleBody[] = R"GLSL(
precision highp float;
in vec2 uv;
uniform sampler2D Source;
// Where the picture starts in the input: a screen's window of an X11 root
// (KmsFrame::sourceX) for the horizontal pass, 0,0 for the vertical one.
uniform ivec2 Offset;
// How the input holds the picture (KmsFrame::orientation), as the
// conversion's Orient: the horizontal pass only, (0,0) for the vertical one.
uniform ivec2 Orient;
out vec4 o;

// The texel under pixel (x, y) of the upright picture, MW_LEN × MW_FIXED in
// the horizontal pass: Orientation.h's orientedTexel. The vertical pass reads
// the intermediate, upright already.
ivec2 Texel(int x, int y) {
#if MW_HORIZONTAL
    int w = int(MW_LEN);
    int h = int(MW_FIXED);
    if (Orient.y == 1) x = w - 1 - x;
    if (Orient.x == 1) return ivec2(y, w - 1 - x);
    if (Orient.x == 2) return ivec2(w - 1 - x, h - 1 - y);
    if (Orient.x == 3) return ivec2(h - 1 - y, x);
#endif
    return ivec2(x, y);
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow(max(c + 0.055, 0.0) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 linearToSrgb(vec3 c) {
    c = max(c, 0.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
// Lanczos with two lobes, x already divided by the dilation.
float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}
vec3 fetch(int x, int y) {
    vec3 c = texelFetch(Source, Texel(x, y) + Offset, 0).rgb;
#if MW_DECODE
    c = srgbToLinear(c);
#endif
    return c;
}
void main() {
#if MW_HORIZONTAL
    float centre = uv.x * MW_LEN - 0.5;
    int fixedCoord = int(uv.y * MW_FIXED);
#else
    float centre = uv.y * MW_LEN - 0.5;
    int fixedCoord = int(uv.x * MW_FIXED);
#endif
    int first = int(ceil(centre - 2.0 * MW_DILATE));
    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int t = 0; t < MW_TAPS; ++t) {
        int p = first + t;
        float w = lanczos2((float(p) - centre) / MW_DILATE);
        p = clamp(p, 0, int(MW_LEN) - 1);
#if MW_HORIZONTAL
        acc += fetch(p, fixedCoord) * w;
#else
        acc += fetch(fixedCoord, p) * w;
#endif
        sum += w;
    }
    vec3 c = max(acc / sum, 0.0);
#if MW_ENCODE
    o = vec4(linearToSrgb(c), 1.0);
#else
    o = vec4(c, 1.0);
#endif
}
)GLSL";

/// One direction of the resample pass as a complete fragment shader: the
/// geometry as #defines, then the body.
std::string scaleShader(bool horizontal, bool decode, bool encode, int length, int output,
                        int fixed)
{
    const double dilate = std::max(1.0, static_cast<double>(length) / output);
    const int taps = static_cast<int>(std::ceil(4.0 * dilate)) + 1;
    // The dilation goes in as the ratio it is, never as a formatted double:
    // "%f" follows LC_NUMERIC, and a host whose region writes decimals with a
    // comma would emit `1,166667` — which GLSL reads as two arguments and the
    // compile fails, with an error about lanczos2 that says nothing about the
    // locale. The ratio is also exact where nine digits are not.
    char dilateText[48];
    if (length <= output)
        std::snprintf(dilateText, sizeof(dilateText), "1.0");
    else
        std::snprintf(dilateText, sizeof(dilateText), "(%d.0 / %d.0)", length, output);
    char text[512];
    std::snprintf(text, sizeof(text),
                  "#version 300 es\n#define MW_HORIZONTAL %d\n#define MW_DECODE %d\n"
                  "#define MW_ENCODE %d\n#define MW_DILATE %s\n#define MW_TAPS %d\n"
                  "#define MW_LEN %d.0\n#define MW_FIXED %d.0\n",
                  horizontal ? 1 : 0, decode ? 1 : 0, encode ? 1 : 0, dilateText, taps, length,
                  fixed);
    return std::string(text) + kScaleBody;
}

PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay = nullptr;
PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;

bool loadEntryPoints()
{
    pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    pCreateImage =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    return pGetPlatformDisplay && pCreateImage && pDestroyImage && pImageTargetTexture;
}

/// Import up to four DMA-BUF planes as one EGLImage.
///
/// ALL the planes GETFB2 reported, not just the pixels: a DCC-compressed AMD
/// buffer carries its compression metadata in planes 1 and 2, and an import
/// that names only plane 0 is refused with EGL_BAD_MATCH — the failure that
/// looks like "the modifier is unsupported" and is in fact an incomplete
/// description of a buffer the driver knows perfectly well.
EGLImageKHR importPlanes(EGLDisplay display, int planeCount, const int* fds,
                         const uint32_t* offsets, const uint32_t* pitches, uint32_t fourcc,
                         uint64_t modifier, int width, int height)
{
    static const EGLint kFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                  EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint kOffset[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                      EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint kPitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                     EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint kModLo[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint kModHi[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = static_cast<EGLint>(fourcc);
    for (int i = 0; i < planeCount && i < 4; ++i) {
        attribs[n++] = kFd[i];
        attribs[n++] = fds[i];
        attribs[n++] = kOffset[i];
        attribs[n++] = static_cast<EGLint>(offsets[i]);
        attribs[n++] = kPitch[i];
        attribs[n++] = static_cast<EGLint>(pitches[i]);
        attribs[n++] = kModLo[i];
        attribs[n++] = static_cast<EGLint>(modifier & 0xffffffffu);
        attribs[n++] = kModHi[i];
        attribs[n++] = static_cast<EGLint>(modifier >> 32);
    }
    attribs[n++] = EGL_NONE;
    return pCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
}

GLuint compile(GLenum type, const std::string& source, std::string& error)
{
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char logText[1024] = {};
        glGetShaderInfoLog(shader, sizeof(logText), nullptr, logText);
        error = std::string("shader compile failed: ") + logText;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link(GLuint vs, GLuint fs, std::string& error)
{
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char logText[1024] = {};
        glGetProgramInfoLog(program, sizeof(logText), nullptr, logText);
        error = std::string("shader link failed: ") + logText;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint newTexture(GLenum filter)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

} // namespace

struct GlConvert::Impl
{
    int renderFd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;

    GLuint lumaProgram = 0;
    GLuint chromaProgram = 0;

    GLuint sourceTexture = 0;
    EGLImageKHR sourceImage = EGL_NO_IMAGE_KHR;

    EGLImageKHR lumaImage = EGL_NO_IMAGE_KHR;
    EGLImageKHR chromaImage = EGL_NO_IMAGE_KHR;
    GLuint lumaTexture = 0;
    GLuint chromaTexture = 0;
    GLuint lumaFbo = 0;
    GLuint chromaFbo = 0;

    GLuint cursorPixels = 0;
    GLuint cursorInvert = 0;
    bool haveCursorTextures = false;

    // The resample pass, when the filter is Lanczos2: horizontal into
    // scaledMid (picture-wide, source-high, RGBA16F linear), vertical into
    // scaled (output-sized RGBA8, sRGB-encoded) — which the conversion then
    // reads where it read the scanout.
    GLuint scaleHProgram = 0;
    GLuint scaleVProgram = 0;
    GLuint scaledMidTexture = 0;
    GLuint scaledMidFbo = 0;
    GLuint scaledTexture = 0;
    GLuint scaledFbo = 0;
    // The resample timed on the GPU (ResampleCost.h), with the
    // GL_EXT_disjoint_timer_query elapsed-time query. One query, not the ring
    // D3D11 needs: convert() finishes the GPU before it returns, so the result
    // is in by then.
    GLuint resampleQuery = 0;
};

GlConvert::GlConvert()
    : d(std::make_unique<Impl>())
{}

GlConvert::~GlConvert()
{
    stop();
}

bool GlConvert::createContext(const std::string& renderNode, std::string& error)
{
    if (!loadEntryPoints()) {
        error = "EGL lacks the DMA-BUF import entry points";
        return false;
    }
    d->renderFd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (d->renderFd < 0) {
        error = "cannot open the render node " + renderNode;
        return false;
    }
    d->gbm = gbm_create_device(d->renderFd);
    if (!d->gbm) {
        error = "GBM refused the render node";
        return false;
    }
    // The GBM platform is what gives EGL a display with no window system at
    // all: this runs before anyone logs in, on a machine that may have no X11
    // and no Wayland socket to speak to.
    d->display = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, d->gbm, nullptr);
    EGLint major = 0, minor = 0;
    if (d->display == EGL_NO_DISPLAY || !eglInitialize(d->display, &major, &minor)) {
        error = "EGL could not initialize on the render node";
        return false;
    }
    const char* extensions = eglQueryString(d->display, EGL_EXTENSIONS);
    if (!extensions || !std::strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
        !std::strstr(extensions, "EGL_KHR_surfaceless_context")) {
        error = "EGL lacks dma_buf_import_modifiers or surfaceless_context";
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0,
                                    EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(d->display, configAttribs, &config, 1, &count) || count == 0) {
        error = "no ES3 EGL config";
        return false;
    }
    // The conversion's place on the GPU (plan §9-18, bench §8o.1). A context
    // at the game's priority waits for the game's frame in flight AND for the
    // next one: 46 ms under a load that saturates a Radeon 780M. At HIGH it
    // goes ahead of the next one: 23 ms. amdgpu, like i915 and xe, grants a
    // context above normal only to CAP_SYS_NICE, which the package's launcher
    // hands over and the app keeps permitted only — raised on this thread for
    // the one call, since the priority is fixed when the context is made.
    //
    // Asked for only when held. The driver's answer proves nothing: Mesa 23.2
    // reads back HIGH for a context the kernel refused (-13), measured
    // 28/09/2026. Holding the capability is the proof.
    const bool priorityExtension = std::strstr(extensions, "EGL_IMG_context_priority") != nullptr;
    const bool mayRaise = platform::ScopedCapability::permitted(CAP_SYS_NICE);
    const EGLint plainAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint highAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                                  EGL_CONTEXT_PRIORITY_HIGH_IMG, EGL_NONE};
    m_HighPriority = false;
    if (priorityExtension && mayRaise) {
        platform::ScopedCapability nice(CAP_SYS_NICE);
        d->context = eglCreateContext(d->display, config, EGL_NO_CONTEXT, highAttribs);
        m_HighPriority = d->context != EGL_NO_CONTEXT && nice.effective();
        m_Priority = m_HighPriority                 ? "high (CAP_SYS_NICE)"
                     : d->context == EGL_NO_CONTEXT ? "normal (the driver refused a high one)"
                                                    : "normal (CAP_SYS_NICE could not be raised)";
    } else {
        m_Priority = !priorityExtension
                         ? "normal (the driver has no EGL_IMG_context_priority)"
                         : "normal (no CAP_SYS_NICE: the package's launcher hands it over)";
    }
    // A driver that refuses the attribute rather than lowering it: the
    // conversion at normal priority beats no conversion at all.
    if (d->context == EGL_NO_CONTEXT)
        d->context = eglCreateContext(d->display, config, EGL_NO_CONTEXT, plainAttribs);
    if (d->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->context)) {
        error = "could not create or bind an ES3 context";
        return false;
    }
    log::info(std::string("[native] GL conversion on ") +
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)) + " (" +
              reinterpret_cast<const char*>(glGetString(GL_VERSION)) + "), priority " + m_Priority);
    return true;
}

bool GlConvert::importableModifiers(const std::string& renderNode, uint32_t fourcc,
                                    std::vector<uint64_t>& modifiers, std::string& error)
{
    modifiers.clear();
    const auto query = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!loadEntryPoints() || !query) {
        error = "EGL cannot list the DMA-BUF modifiers it imports";
        return false;
    }
    // A display of its own, no context: nothing is drawn, and the one the
    // conversion makes later belongs to the thread that converts.
    const int fd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open the render node " + renderNode;
        return false;
    }
    gbm_device* gbm = gbm_create_device(fd);
    EGLDisplay display =
        gbm ? pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr) : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    const bool up = display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor);
    const char* extensions = up ? eglQueryString(display, EGL_EXTENSIONS) : nullptr;
    EGLint count = 0;
    bool listed = extensions &&
                  std::strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr &&
                  query(display, static_cast<EGLint>(fourcc), 0, nullptr, nullptr, &count);
    if (listed && count > 0) {
        std::vector<EGLuint64KHR> all(static_cast<size_t>(count));
        std::vector<EGLBoolean> externalOnly(static_cast<size_t>(count));
        listed = query(display, static_cast<EGLint>(fourcc), count, all.data(), externalOnly.data(),
                       &count);
        // External-only means sampled through samplerExternalOES, which the
        // conversion's shaders do not use: such a buffer would import and then
        // fail to bind.
        for (EGLint i = 0; listed && i < count; ++i)
            if (!externalOnly[static_cast<size_t>(i)])
                modifiers.push_back(all[static_cast<size_t>(i)]);
    }
    if (up) eglTerminate(display);
    if (gbm) gbm_device_destroy(gbm);
    ::close(fd);
    if (!listed) {
        error = "EGL on " + renderNode + " lists no DMA-BUF modifiers";
        return false;
    }
    return true;
}

bool GlConvert::createShaders(std::string& error)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    if (!vs) return false;
    const GLuint fsLuma = compile(GL_FRAGMENT_SHADER, std::string(kCommon) + kLumaMain, error);
    if (!fsLuma) return false;
    const GLuint fsChroma = compile(GL_FRAGMENT_SHADER, std::string(kCommon) + kChromaMain, error);
    if (!fsChroma) return false;
    d->lumaProgram = link(vs, fsLuma, error);
    if (!d->lumaProgram) return false;
    d->chromaProgram = link(vs, fsChroma, error);
    if (!d->chromaProgram) return false;
    glDeleteShader(vs);
    glDeleteShader(fsLuma);
    glDeleteShader(fsChroma);

    d->sourceTexture = newTexture(GL_LINEAR);
    // Point sampling for the invert mask: a half-inverted pixel is not a thing.
    d->cursorPixels = newTexture(GL_LINEAR);
    d->cursorInvert = newTexture(GL_NEAREST);
    return true;
}

bool GlConvert::init(const std::string& renderNode, uint32_t sourceFourcc, int sourceWidth,
                     int sourceHeight, int outputWidth, int outputHeight, ScaleFilter filter,
                     std::string& error)
{
    stop();
    d = std::make_unique<Impl>();

    // Ten bits a channel is still the SDR desktop: KWin scans out in
    // XRGB2101010 wherever the display takes it — the Radeon 780M under Plasma
    // 5.24, measured 15/09/2026, where this refusal left KDE with no stream at
    // all. An imported image is sampled normalised, whatever its depth, so the
    // shaders are the same. HDR (a PQ desktop, ABGR16161616F) is another matter
    // and is not written on this platform: refused, as ColorConvert does for a
    // format it lacks.
    if (sourceFourcc != DRM_FORMAT_XRGB8888 && sourceFourcc != DRM_FORMAT_ARGB8888 &&
        sourceFourcc != DRM_FORMAT_XBGR8888 && sourceFourcc != DRM_FORMAT_ABGR8888 &&
        sourceFourcc != DRM_FORMAT_XRGB2101010 && sourceFourcc != DRM_FORMAT_ARGB2101010 &&
        sourceFourcc != DRM_FORMAT_XBGR2101010 && sourceFourcc != DRM_FORMAT_ABGR2101010) {
        error = "no GL conversion for scanout format " + std::to_string(sourceFourcc);
        return false;
    }

    m_SourceFourcc = sourceFourcc;
    m_ResampleCost.reset();
    m_ResampleCostTaken = false;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    // The resample pass only where there is something to resample.
    const bool scaling = m_OutputWidth != m_SourceWidth || m_OutputHeight != m_SourceHeight;
    m_Filter = scaling ? filter : ScaleFilter::Bilinear;
    m_Letterboxed = false;
    m_PictureX = m_PictureY = 0;
    m_PictureWidth = m_OutputWidth;
    m_PictureHeight = m_OutputHeight;
    if (m_Filter != ScaleFilter::Bilinear) {
        // A source of another shape is fitted between bars, as macOS does
        // (FrameFit.h), rather than stretched as the bilinear path does.
        const platform::FrameFit fit =
            platform::frameFit(m_SourceWidth, m_SourceHeight, m_OutputWidth, m_OutputHeight);
        const int w = static_cast<int>(std::lround(m_SourceWidth * fit.scale));
        const int h = static_cast<int>(std::lround(m_SourceHeight * fit.scale));
        if (w < m_OutputWidth - 1 || h < m_OutputHeight - 1) {
            m_Letterboxed = true;
            m_PictureWidth = std::max(2, w);
            m_PictureHeight = std::max(2, h);
            m_PictureX = (m_OutputWidth - m_PictureWidth) / 2;
            m_PictureY = (m_OutputHeight - m_PictureHeight) / 2;
        }
    }

    if (!createContext(renderNode, error)) return false;
    if (!createShaders(error)) return false;
    if (m_Filter != ScaleFilter::Bilinear && !createScaler(error)) return false;
    // Built on this thread, converted on another: let go of the context so the
    // capture thread can take it (an EGL context is current on one thread).
    detachThread();

    log::info("[native] colour conversion: " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) + " XRGB -> " + std::to_string(m_OutputWidth) + "x" +
              std::to_string(m_OutputHeight) + " NV12 4:2:0 (BT.709 limited), via EGL" +
              (!scaling                            ? ", 1:1"
               : m_Filter == ScaleFilter::Bilinear ? ", scaled bilinear in the pass"
                                                   : ", scaled Lanczos-2 (linear light)") +
              (m_Letterboxed ? ", letterboxed to " + std::to_string(m_PictureWidth) + "x" +
                                   std::to_string(m_PictureHeight)
                             : ""));
    return true;
}

bool GlConvert::createScaler(std::string& error)
{
    // The intermediate is linear light and needs more than 8 bits, so it is
    // RGBA16F — colour-renderable only with GL_EXT_color_buffer_float (core
    // in ES 3.2, an extension below). Without it the pass cannot be built;
    // the session keeps the bilinear pass and the log says why.
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool floatTargets =
        extensions && (std::strstr(extensions, "GL_EXT_color_buffer_float") ||
                       std::strstr(extensions, "GL_EXT_color_buffer_half_float"));
    if (!floatTargets) {
        log::info("[native] no float render targets on this GL (GL_EXT_color_buffer_float) — "
                  "the resample pass is unavailable, scaling bilinear in the conversion pass");
        m_Filter = ScaleFilter::Bilinear;
        m_Letterboxed = false;
        m_PictureX = m_PictureY = 0;
        m_PictureWidth = m_OutputWidth;
        m_PictureHeight = m_OutputHeight;
        return true;
    }

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    if (!vs) return false;
    const GLuint fsH = compile(
        GL_FRAGMENT_SHADER,
        scaleShader(true, true, false, m_SourceWidth, m_PictureWidth, m_SourceHeight), error);
    if (!fsH) return false;
    const GLuint fsV = compile(
        GL_FRAGMENT_SHADER,
        scaleShader(false, false, true, m_SourceHeight, m_PictureHeight, m_PictureWidth), error);
    if (!fsV) return false;
    d->scaleHProgram = link(vs, fsH, error);
    if (!d->scaleHProgram) return false;
    d->scaleVProgram = link(vs, fsV, error);
    if (!d->scaleVProgram) return false;
    glDeleteShader(vs);
    glDeleteShader(fsH);
    glDeleteShader(fsV);

    d->scaledMidTexture = newTexture(GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_PictureWidth, m_SourceHeight, 0, GL_RGBA,
                 GL_HALF_FLOAT, nullptr);
    glGenFramebuffers(1, &d->scaledMidFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->scaledMidFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->scaledMidTexture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error = "the resample intermediate cannot be rendered into";
        return false;
    }

    // The scaled picture: sampled linearly by the conversion, as the scanout
    // was, so a 1:1 read lands on texel centres and the chroma pass's read at
    // its own centre is a true 2x2 average.
    d->scaledTexture = newTexture(GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_OutputWidth, m_OutputHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &d->scaledFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->scaledFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->scaledTexture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error = "the scaled picture cannot be rendered into";
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        error = "GL reported an error building the resample pass";
        return false;
    }

    // Without a GPU timer the cost cannot be known, and the pass stays: the
    // rule that drops it is about a measured cost, never a guessed one.
    if (extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query"))
        glGenQueries(1, &d->resampleQuery);
    else
        log::info("[native] no GPU timer on this GL (GL_EXT_disjoint_timer_query) — the "
                  "resample pass is kept without its cost measured");
    return true;
}

bool GlConvert::dropResample()
{
    if (m_Filter == ScaleFilter::Bilinear || m_Letterboxed) return false;
    // convert() branches on m_Filter alone, so this is the whole switch: the
    // conversion samples the scanout again, as init() would have set it up.
    m_Filter = ScaleFilter::Bilinear;
    releaseScaler();
    return true;
}

bool GlConvert::takeResampleCost(int64_t& costUs)
{
    if (m_ResampleCostTaken || !m_ResampleCost.done()) return false;
    m_ResampleCostTaken = true;
    costUs = m_ResampleCost.medianUs();
    return true;
}

void GlConvert::releaseScaler()
{
    // The objects go with the pass — VRAM an iGPU shares with the desktop —
    // when the context is at hand; stop() deletes whatever is left.
    if (d->context == EGL_NO_CONTEXT || eglGetCurrentContext() != d->context) return;
    if (d->scaledMidFbo) glDeleteFramebuffers(1, &d->scaledMidFbo);
    if (d->scaledFbo) glDeleteFramebuffers(1, &d->scaledFbo);
    const GLuint textures[] = {d->scaledMidTexture, d->scaledTexture};
    glDeleteTextures(2, textures);
    if (d->scaleHProgram) glDeleteProgram(d->scaleHProgram);
    if (d->scaleVProgram) glDeleteProgram(d->scaleVProgram);
    if (d->resampleQuery) glDeleteQueries(1, &d->resampleQuery);
    d->scaledMidFbo = d->scaledFbo = d->scaledMidTexture = d->scaledTexture = 0;
    d->scaleHProgram = d->scaleVProgram = d->resampleQuery = 0;
}

void GlConvert::collectResampleTiming()
{
    GLuint available = 0;
    glGetQueryObjectuiv(d->resampleQuery, GL_QUERY_RESULT_AVAILABLE, &available);
    // After glFinish it is; a driver that says otherwise loses one sample.
    if (!available) return;
    GLuint ns = 0;
    glGetQueryObjectuiv(d->resampleQuery, GL_QUERY_RESULT, &ns);
    // A disjoint interval (the GPU changed clock mid-way, or was reset) says
    // nothing reliable: dropped, the next frame is timed instead.
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (!disjoint) m_ResampleCost.add(static_cast<int64_t>(ns / 1000));
}

bool GlConvert::makeCurrent(std::string& error)
{
    // An EGL context is current on ONE thread, and init() ran on whichever
    // thread built the session while convert() runs on the capture thread.
    // Without this, every GL call below is a silent no-op — no context, no
    // error either — and the encoder reads a surface nothing ever wrote:
    // a black picture with nothing in the log. Cheap when already current.
    if (eglGetCurrentContext() == d->context) return true;
    if (!eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->context)) {
        // EGL_BAD_ACCESS here means another thread still holds it: whoever
        // used the converter last did not detachThread().
        char code[16];
        std::snprintf(code, sizeof(code), "0x%X", static_cast<unsigned>(eglGetError()));
        error = std::string("could not bind the ES3 context to this thread (") + code + ")";
        return false;
    }
    return true;
}

void GlConvert::detachThread()
{
    if (!d || d->context == EGL_NO_CONTEXT) return;
    if (eglGetCurrentContext() == d->context)
        eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool GlConvert::bindTarget(const Nv12Target& target, std::string& error)
{
    if (d->context == EGL_NO_CONTEXT) {
        error = "GL conversion is not initialized";
        return false;
    }
    if (!makeCurrent(error)) return false;
    if (target.width != m_OutputWidth || target.height != m_OutputHeight) {
        error = "the encoder's surface is not the size the converter produces";
        return false;
    }

    // One plane per image, exactly as the D3D11 path views one plane per RTV:
    // R8 addresses the luma, GR88 the interleaved chroma at half size.
    const uint32_t offY = target.offsetY, pitchY = target.pitchY;
    const uint32_t offUV = target.offsetUV, pitchUV = target.pitchUV;
    d->lumaImage = importPlanes(d->display, 1, &target.fdY, &offY, &pitchY, DRM_FORMAT_R8,
                                target.modifier, target.width, target.height);
    d->chromaImage = importPlanes(d->display, 1, &target.fdUV, &offUV, &pitchUV, DRM_FORMAT_GR88,
                                  target.modifier, target.width / 2, target.height / 2);
    if (d->lumaImage == EGL_NO_IMAGE_KHR || d->chromaImage == EGL_NO_IMAGE_KHR) {
        error = "EGL refused the encoder's NV12 planes (0x" + std::to_string(eglGetError()) + ")";
        return false;
    }

    d->lumaTexture = newTexture(GL_NEAREST);
    pImageTargetTexture(GL_TEXTURE_2D, d->lumaImage);
    d->chromaTexture = newTexture(GL_NEAREST);
    pImageTargetTexture(GL_TEXTURE_2D, d->chromaImage);

    glGenFramebuffers(1, &d->lumaFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->lumaFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->lumaTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error = "the luma plane cannot be rendered into";
        return false;
    }
    glGenFramebuffers(1, &d->chromaFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, d->chromaFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->chromaTexture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        error = "the chroma plane cannot be rendered into";
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    detachThread();
    return true;
}

bool GlConvert::updateCursorTextures(const capture::CursorState& cursor, std::string& error)
{
    if (cursor.width <= 0 || cursor.height <= 0) return true;
    if (d->haveCursorTextures && m_CursorShapeVersion == cursor.shapeVersion) return true;

    // CursorState is BGRA in memory and GLES has no BGRA upload format, so the
    // channels are swapped here, once per shape — rather than in the shader,
    // which then stays a line-for-line transcription of the D3D11 one.
    std::vector<uint8_t> rgba(cursor.pixels.size());
    for (size_t i = 0; i + 3 < cursor.pixels.size(); i += 4) {
        rgba[i + 0] = cursor.pixels[i + 2];
        rgba[i + 1] = cursor.pixels[i + 1];
        rgba[i + 2] = cursor.pixels[i + 0];
        rgba[i + 3] = cursor.pixels[i + 3];
    }
    glBindTexture(GL_TEXTURE_2D, d->cursorPixels);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cursor.width, cursor.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, d->cursorInvert);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cursor.width, cursor.height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 cursor.invert.data());
    if (glGetError() != GL_NO_ERROR) {
        error = "could not upload the cursor";
        return false;
    }
    d->haveCursorTextures = true;
    m_CursorShapeVersion = cursor.shapeVersion;
    return true;
}

bool GlConvert::convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                        const CursorDraw& draw, std::string& error)
{
    if (d->context == EGL_NO_CONTEXT || !d->lumaFbo) {
        error = "GL conversion has no target bound";
        return false;
    }
    if (!makeCurrent(error)) return false;
    if (!updateCursorTextures(cursor, error)) return false;

    // The scanout buffer, imported fresh. The compositor rotates between two
    // or three buffers; an image per frame is a few microseconds, and caching
    // by fb_id is a refinement for when a measurement asks for it.
    if (d->sourceImage != EGL_NO_IMAGE_KHR) pDestroyImage(d->display, d->sourceImage);
    // At the buffer's size, which is the picture's but for a screen of an X11
    // desktop over several: sampled below through its window.
    d->sourceImage =
        importPlanes(d->display, frame.planeCount, frame.fds, frame.offsets, frame.pitches,
                     frame.fourcc, frame.modifier, frame.importWidth(), frame.importHeight());
    if (d->sourceImage == EGL_NO_IMAGE_KHR) {
        error = "EGL refused the scanout buffer (0x" + std::to_string(eglGetError()) + ")";
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, d->sourceTexture);
    pImageTargetTexture(GL_TEXTURE_2D, d->sourceImage);

    // The resample pass, when there is one: the scanout through the
    // horizontal filter into the intermediate, the intermediate through the
    // vertical one into the scaled picture, which the conversion below reads
    // at 1:1. Bars, if any, are cleared to black first — the vertical pass
    // only paints the fitted rectangle.
    const bool resampled = m_Filter != ScaleFilter::Bilinear;
    const bool timed = resampled && d->resampleQuery && m_ResampleCost.timeThisFrame(0);
    if (timed) {
        // Reading the flag clears it: what it says after the query is about
        // this frame alone.
        GLint disjoint = 0;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        glBeginQuery(GL_TIME_ELAPSED_EXT, d->resampleQuery);
    }
    // How the scanout holds the picture: upright but on a display turned
    // against its panel (issue #54), stood upright by the first read below.
    const int turns = frame.orientation.turns & 3;
    const int mirrored = frame.orientation.mirrored ? 1 : 0;
    if (resampled) {
        const auto scale = [&](GLuint program, GLuint fbo, GLuint input, int x, int y, int w, int h,
                               int offsetX, int offsetY, int orientTurns, int orientMirrored) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(x, y, w, h);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, input);
            glUniform1i(glGetUniformLocation(program, "Source"), 0);
            glUniform2i(glGetUniformLocation(program, "Offset"), offsetX, offsetY);
            glUniform2i(glGetUniformLocation(program, "Orient"), orientTurns, orientMirrored);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        };
        // The horizontal pass reads the picture where and as it sits in the
        // scanout; the vertical one reads the intermediate, which is the
        // picture alone, upright.
        scale(d->scaleHProgram, d->scaledMidFbo, d->sourceTexture, 0, 0, m_PictureWidth,
              m_SourceHeight, frame.sourceX, frame.sourceY, turns, mirrored);
        if (m_Letterboxed) {
            glBindFramebuffer(GL_FRAMEBUFFER, d->scaledFbo);
            glViewport(0, 0, m_OutputWidth, m_OutputHeight);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        scale(d->scaleVProgram, d->scaledFbo, d->scaledMidTexture, m_PictureX, m_PictureY,
              m_PictureWidth, m_PictureHeight, 0, 0, 0, 0);
    }
    if (timed) glEndQuery(GL_TIME_ELAPSED_EXT);
    const GLuint scene = resampled ? d->scaledTexture : d->sourceTexture;

    const bool drawCursor = cursor.visible && d->haveCursorTextures && cursor.width > 0 &&
                            cursor.height > 0 && m_SourceWidth > 0 && m_SourceHeight > 0;
    const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
    const float cw = static_cast<float>(cursor.width) * magnify;
    const float ch = static_cast<float>(cursor.height) * magnify;
    const float cx =
        static_cast<float>(cursor.x + draw.hotspotX) - static_cast<float>(draw.hotspotX) * magnify;
    const float cy =
        static_cast<float>(cursor.y + draw.hotspotY) - static_cast<float>(draw.hotspotY) * magnify;
    // The cursor rectangle in the uv of whatever the conversion samples: the
    // scanout, or the scaled picture — the same square unless letterboxed.
    float rect[4] = {
        cx / static_cast<float>(m_SourceWidth), cy / static_cast<float>(m_SourceHeight),
        cw / static_cast<float>(m_SourceWidth), ch / static_cast<float>(m_SourceHeight)};
    if (resampled) {
        const float sx = static_cast<float>(m_PictureWidth) / static_cast<float>(m_OutputWidth);
        const float sy = static_cast<float>(m_PictureHeight) / static_cast<float>(m_OutputHeight);
        rect[0] = static_cast<float>(m_PictureX) / static_cast<float>(m_OutputWidth) + rect[0] * sx;
        rect[1] =
            static_cast<float>(m_PictureY) / static_cast<float>(m_OutputHeight) + rect[1] * sy;
        rect[2] *= sx;
        rect[3] *= sy;
    }
    // The picture inside what the conversion samples: the scaled picture is all
    // picture; the scanout is too, but for a screen's window of an X11 root.
    // The window is the picture as the scanout holds it, turned or not.
    float sourceRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (!resampled &&
        (frame.sourceX || frame.sourceY || frame.importWidth() != frame.windowWidth() ||
         frame.importHeight() != frame.windowHeight())) {
        const float bw = static_cast<float>(frame.importWidth());
        const float bh = static_cast<float>(frame.importHeight());
        sourceRect[0] = static_cast<float>(frame.sourceX) / bw;
        sourceRect[1] = static_cast<float>(frame.sourceY) / bh;
        sourceRect[2] = static_cast<float>(frame.windowWidth()) / bw;
        sourceRect[3] = static_cast<float>(frame.windowHeight()) / bh;
    }

    const auto pass = [&](GLuint program, GLuint fbo, int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene);
        glUniform1i(glGetUniformLocation(program, "Source"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, d->cursorPixels);
        glUniform1i(glGetUniformLocation(program, "CursorPixels"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, d->cursorInvert);
        glUniform1i(glGetUniformLocation(program, "CursorInvert"), 2);
        glUniform4fv(glGetUniformLocation(program, "CursorRect"), 1, rect);
        glUniform1f(glGetUniformLocation(program, "CursorEnabled"), drawCursor ? 1.0f : 0.0f);
        glUniform4fv(glGetUniformLocation(program, "SourceRect"), 1, sourceRect);
        // The scaled picture was stood upright by its horizontal pass.
        glUniform2i(glGetUniformLocation(program, "Orient"), resampled ? 0 : turns,
                    resampled ? 0 : mirrored);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    pass(d->lumaProgram, d->lumaFbo, m_OutputWidth, m_OutputHeight);
    pass(d->chromaProgram, d->chromaFbo, m_OutputWidth / 2, m_OutputHeight / 2);

    // The encoder reads the surface on its own queue, which GL knows nothing
    // about. A full finish is the plain way to make the writes visible; a fence
    // handed to VA-API would let the two overlap, and is the refinement to
    // measure for when the convert stage shows up in the stats.
    glFinish();
    if (timed) collectResampleTiming();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        error = "GL reported an error during conversion";
        return false;
    }
    return true;
}

void GlConvert::stop()
{
    if (!d) return;
    if (d->display != EGL_NO_DISPLAY && d->context != EGL_NO_CONTEXT) {
        // Fails (EGL_BAD_ACCESS) if the capture thread still holds the
        // context — LinuxSession detaches before its thread ends — in which
        // case the GL deletes below are no-ops and eglDestroyContext still
        // frees everything once the context is released.
        eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->context);
        if (d->lumaFbo) glDeleteFramebuffers(1, &d->lumaFbo);
        if (d->chromaFbo) glDeleteFramebuffers(1, &d->chromaFbo);
        if (d->scaledMidFbo) glDeleteFramebuffers(1, &d->scaledMidFbo);
        if (d->scaledFbo) glDeleteFramebuffers(1, &d->scaledFbo);
        const GLuint textures[] = {d->sourceTexture, d->lumaTexture,  d->chromaTexture,
                                   d->cursorPixels,  d->cursorInvert, d->scaledMidTexture,
                                   d->scaledTexture};
        glDeleteTextures(7, textures);
        if (d->lumaProgram) glDeleteProgram(d->lumaProgram);
        if (d->chromaProgram) glDeleteProgram(d->chromaProgram);
        if (d->scaleHProgram) glDeleteProgram(d->scaleHProgram);
        if (d->scaleVProgram) glDeleteProgram(d->scaleVProgram);
        if (d->resampleQuery) glDeleteQueries(1, &d->resampleQuery);
        if (d->sourceImage != EGL_NO_IMAGE_KHR) pDestroyImage(d->display, d->sourceImage);
        if (d->lumaImage != EGL_NO_IMAGE_KHR) pDestroyImage(d->display, d->lumaImage);
        if (d->chromaImage != EGL_NO_IMAGE_KHR) pDestroyImage(d->display, d->chromaImage);
        eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(d->display, d->context);
    }
    if (d->display != EGL_NO_DISPLAY) eglTerminate(d->display);
    if (d->gbm) gbm_device_destroy(d->gbm);
    if (d->renderFd >= 0) ::close(d->renderFd);
    d = std::make_unique<Impl>();
    m_HighPriority = false;
    m_Priority.clear();
}

} // namespace mw::native::convert
