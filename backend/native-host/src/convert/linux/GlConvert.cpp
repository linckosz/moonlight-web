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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

// gl2ext.h defines nothing of its own: it needs GL_APIENTRY and the GL types
// from a core header first, and an alphabetical sort puts it before gl3.h. Kept
// in its own block so the formatter leaves the order alone.
// clang-format off
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
// clang-format on

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

// The desktop with the mouse pointer drawn on it. See ColorConvert.cpp: the
// scanout buffer does not contain the pointer (it is on its own hardware
// plane), so it is put back here, costing one fetch on the pixels it covers.
vec3 Scene(vec2 p) {
    vec3 rgb = texture(Source, p).rgb;
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
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    d->context = eglCreateContext(d->display, config, EGL_NO_CONTEXT, contextAttribs);
    if (d->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->context)) {
        error = "could not create or bind an ES3 context";
        return false;
    }
    log::info(std::string("[native] GL conversion on ") +
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)) + " (" +
              reinterpret_cast<const char*>(glGetString(GL_VERSION)) + ")");
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
                     int sourceHeight, int outputWidth, int outputHeight, std::string& error)
{
    stop();
    d = std::make_unique<Impl>();

    if (sourceFourcc != DRM_FORMAT_XRGB8888 && sourceFourcc != DRM_FORMAT_ARGB8888 &&
        sourceFourcc != DRM_FORMAT_XBGR8888 && sourceFourcc != DRM_FORMAT_ABGR8888) {
        // HDR (XRGB2101010, ABGR16161616F) is not written on this platform yet,
        // and a 10-bit desktop read as 8-bit would be a picture that is merely
        // wrong. Refuse instead, as ColorConvert does for a format it lacks.
        error = "no GL conversion for scanout format " + std::to_string(sourceFourcc);
        return false;
    }

    m_SourceFourcc = sourceFourcc;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    if (!createContext(renderNode, error)) return false;
    if (!createShaders(error)) return false;

    log::info("[native] colour conversion: " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) + " XRGB -> " + std::to_string(m_OutputWidth) + "x" +
              std::to_string(m_OutputHeight) + " NV12 4:2:0 (BT.709 limited), via EGL");
    return true;
}

bool GlConvert::bindTarget(const Nv12Target& target, std::string& error)
{
    if (d->context == EGL_NO_CONTEXT) {
        error = "GL conversion is not initialized";
        return false;
    }
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
    if (!updateCursorTextures(cursor, error)) return false;

    // The scanout buffer, imported fresh. The compositor rotates between two
    // or three buffers; an image per frame is a few microseconds, and caching
    // by fb_id is a refinement for when a measurement asks for it.
    if (d->sourceImage != EGL_NO_IMAGE_KHR) pDestroyImage(d->display, d->sourceImage);
    d->sourceImage =
        importPlanes(d->display, frame.planeCount, frame.fds, frame.offsets, frame.pitches,
                     frame.fourcc, frame.modifier, frame.width, frame.height);
    if (d->sourceImage == EGL_NO_IMAGE_KHR) {
        error = "EGL refused the scanout buffer (0x" + std::to_string(eglGetError()) + ")";
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, d->sourceTexture);
    pImageTargetTexture(GL_TEXTURE_2D, d->sourceImage);

    const bool drawCursor = cursor.visible && d->haveCursorTextures && cursor.width > 0 &&
                            cursor.height > 0 && m_SourceWidth > 0 && m_SourceHeight > 0;
    const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
    const float cw = static_cast<float>(cursor.width) * magnify;
    const float ch = static_cast<float>(cursor.height) * magnify;
    const float cx =
        static_cast<float>(cursor.x + draw.hotspotX) - static_cast<float>(draw.hotspotX) * magnify;
    const float cy =
        static_cast<float>(cursor.y + draw.hotspotY) - static_cast<float>(draw.hotspotY) * magnify;
    const float rect[4] = {
        cx / static_cast<float>(m_SourceWidth), cy / static_cast<float>(m_SourceHeight),
        cw / static_cast<float>(m_SourceWidth), ch / static_cast<float>(m_SourceHeight)};

    const auto pass = [&](GLuint program, GLuint fbo, int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, w, h);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->sourceTexture);
        glUniform1i(glGetUniformLocation(program, "Source"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, d->cursorPixels);
        glUniform1i(glGetUniformLocation(program, "CursorPixels"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, d->cursorInvert);
        glUniform1i(glGetUniformLocation(program, "CursorInvert"), 2);
        glUniform4fv(glGetUniformLocation(program, "CursorRect"), 1, rect);
        glUniform1f(glGetUniformLocation(program, "CursorEnabled"), drawCursor ? 1.0f : 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    pass(d->lumaProgram, d->lumaFbo, m_OutputWidth, m_OutputHeight);
    pass(d->chromaProgram, d->chromaFbo, m_OutputWidth / 2, m_OutputHeight / 2);

    // The encoder reads the surface on its own queue, which GL knows nothing
    // about. A full finish is the plain way to make the writes visible; a fence
    // handed to VA-API would let the two overlap, and is the refinement to
    // measure for when the convert stage shows up in the stats.
    glFinish();
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
        eglMakeCurrent(d->display, EGL_NO_SURFACE, EGL_NO_SURFACE, d->context);
        if (d->lumaFbo) glDeleteFramebuffers(1, &d->lumaFbo);
        if (d->chromaFbo) glDeleteFramebuffers(1, &d->chromaFbo);
        const GLuint textures[] = {d->sourceTexture, d->lumaTexture, d->chromaTexture,
                                   d->cursorPixels, d->cursorInvert};
        glDeleteTextures(5, textures);
        if (d->lumaProgram) glDeleteProgram(d->lumaProgram);
        if (d->chromaProgram) glDeleteProgram(d->chromaProgram);
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
}

} // namespace mw::native::convert
