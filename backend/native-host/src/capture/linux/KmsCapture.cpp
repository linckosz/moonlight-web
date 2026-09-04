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

#include "KmsCapture.h"

#include "../../core/Log.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace mw::native::capture {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string errnoText()
{
    return std::strerror(errno);
}

const char* connectorTypeName(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    default: return "Unknown";
    }
}

/// A CRTC's index within the card's list — what the vblank ioctl wants, and
/// not the CRTC's object id.
int crtcIndex(int card, uint32_t crtcId)
{
    drmModeRes* res = drmModeGetResources(card);
    if (!res) return -1;
    int index = -1;
    for (int i = 0; i < res->count_crtcs; ++i)
        if (res->crtcs[i] == crtcId) index = i;
    drmModeFreeResources(res);
    return index;
}

/// Read one property's current value from a plane.
bool planeProperty(int card, uint32_t planeId, uint32_t propId, uint64_t& value)
{
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(card, planeId, DRM_MODE_OBJECT_PLANE);
    if (!props) return false;
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        if (props->props[i] == propId) {
            value = props->prop_values[i];
            found = true;
            break;
        }
    }
    drmModeFreeObjectProperties(props);
    return found;
}

/// Find a plane property's id by name, and the plane's "type" value.
struct PlaneProps
{
    uint32_t type = 0;
    uint32_t crtcX = 0;
    uint32_t crtcY = 0;
    uint32_t fbId = 0;
    uint64_t typeValue = 0;
};

bool readPlaneProps(int card, uint32_t planeId, PlaneProps& out)
{
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(card, planeId, DRM_MODE_OBJECT_PLANE);
    if (!props) return false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(card, props->props[i]);
        if (!p) continue;
        if (std::strcmp(p->name, "type") == 0) {
            out.type = p->prop_id;
            out.typeValue = props->prop_values[i];
        } else if (std::strcmp(p->name, "CRTC_X") == 0) {
            out.crtcX = p->prop_id;
        } else if (std::strcmp(p->name, "CRTC_Y") == 0) {
            out.crtcY = p->prop_id;
        } else if (std::strcmp(p->name, "FB_ID") == 0) {
            out.fbId = p->prop_id;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return out.type != 0;
}

} // namespace

// ── Enumeration ─────────────────────────────────────────────────────────────

std::vector<KmsOutput> KmsCapture::listOutputs(const std::string& cardPath, std::string& error)
{
    std::vector<KmsOutput> outputs;
    const int card = ::open(cardPath.c_str(), O_RDWR | O_CLOEXEC);
    if (card < 0) {
        error = "cannot open " + cardPath + " (" + errnoText() + ")";
        return outputs;
    }
    drmModeRes* res = drmModeGetResources(card);
    if (!res) {
        error = cardPath + " has no mode-setting resources (a render-only node?)";
        ::close(card);
        return outputs;
    }

    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* c = drmModeGetConnector(card, res->connectors[i]);
        if (!c) continue;
        KmsOutput out;
        out.cardPath = cardPath;
        out.connectorId = c->connector_id;
        out.name = std::string(connectorTypeName(c->connector_type)) + "-" +
                   std::to_string(c->connector_type_id);
        out.connected = c->connection == DRM_MODE_CONNECTED;
        if (out.connected && c->encoder_id) {
            drmModeEncoder* e = drmModeGetEncoder(card, c->encoder_id);
            if (e && e->crtc_id) {
                out.crtcId = e->crtc_id;
                drmModeCrtc* crtc = drmModeGetCrtc(card, e->crtc_id);
                if (crtc) {
                    out.width = static_cast<int>(crtc->width);
                    out.height = static_cast<int>(crtc->height);
                    out.x = crtc->x;
                    out.y = crtc->y;
                    out.active = crtc->buffer_id != 0 && crtc->mode_valid;
                    if (crtc->mode_valid) {
                        // vrefresh is rounded to whole hertz; the exact rate is
                        // clock / (htotal × vtotal), which is what a 59.94 Hz
                        // panel really runs at and what the cadence wants.
                        const drmModeModeInfo& m = crtc->mode;
                        if (m.htotal > 0 && m.vtotal > 0)
                            out.refreshMilliHz =
                                static_cast<int>((static_cast<int64_t>(m.clock) * 1000 * 1000) /
                                                 (static_cast<int64_t>(m.htotal) * m.vtotal));
                        else
                            out.refreshMilliHz = static_cast<int>(m.vrefresh) * 1000;
                    }
                    drmModeFreeCrtc(crtc);
                }
            }
            if (e) drmModeFreeEncoder(e);
        }
        outputs.push_back(out);
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    ::close(card);
    return outputs;
}

bool KmsCapture::canReadFramebuffers(const std::string& cardPath, std::string& why)
{
    const int card = ::open(cardPath.c_str(), O_RDWR | O_CLOEXEC);
    if (card < 0) {
        why = "cannot open " + cardPath + " (" + errnoText() + ")";
        return false;
    }
    drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes* planes = drmModeGetPlaneResources(card);
    bool sawBuffer = false;
    bool readable = false;
    for (uint32_t i = 0; planes && i < planes->count_planes && !readable; ++i) {
        drmModePlane* p = drmModeGetPlane(card, planes->planes[i]);
        if (!p) continue;
        if (p->fb_id) {
            sawBuffer = true;
            drmModeFB2* fb = drmModeGetFB2(card, p->fb_id);
            // The metadata is public; the HANDLE is what the kernel withholds
            // from an unprivileged process. That is the whole test.
            if (fb) {
                readable = fb->handles[0] != 0;
                drmModeFreeFB2(fb);
            }
        }
        drmModeFreePlane(p);
    }
    if (planes) drmModeFreePlaneResources(planes);
    ::close(card);

    if (!sawBuffer) {
        why = "no framebuffer is being scanned out on " + cardPath +
              " (no display, or the screen is off)";
        return false;
    }
    if (!readable) {
        why = "the kernel withholds framebuffer handles: this process needs CAP_SYS_ADMIN "
              "(the package sets it on the binary; a build run by hand does not have it)";
        return false;
    }
    return true;
}

// ── The capture ─────────────────────────────────────────────────────────────

KmsCapture::KmsCapture(std::string cardPath, uint32_t connectorId)
    : m_CardPath(std::move(cardPath))
    , m_ConnectorId(connectorId)
{}

KmsCapture::~KmsCapture()
{
    stop();
}

bool KmsCapture::resolveTopology(std::string& error)
{
    drmModeConnector* c = drmModeGetConnector(m_Card, m_ConnectorId);
    if (!c) {
        error = "connector " + std::to_string(m_ConnectorId) + " is gone";
        return false;
    }
    if (c->connection != DRM_MODE_CONNECTED || !c->encoder_id) {
        error = "the display is disconnected";
        drmModeFreeConnector(c);
        return false;
    }
    drmModeEncoder* e = drmModeGetEncoder(m_Card, c->encoder_id);
    drmModeFreeConnector(c);
    if (!e || !e->crtc_id) {
        error = "the display has no CRTC (screen off, or being reconfigured)";
        if (e) drmModeFreeEncoder(e);
        return false;
    }
    m_CrtcId = e->crtc_id;
    drmModeFreeEncoder(e);

    drmModeCrtc* crtc = drmModeGetCrtc(m_Card, m_CrtcId);
    if (!crtc || !crtc->mode_valid) {
        error = "the CRTC has no mode";
        if (crtc) drmModeFreeCrtc(crtc);
        return false;
    }
    m_Width = static_cast<int>(crtc->width);
    m_Height = static_cast<int>(crtc->height);
    m_Rect.left = crtc->x;
    m_Rect.top = crtc->y;
    m_Rect.right = crtc->x + m_Width;
    m_Rect.bottom = crtc->y + m_Height;
    const drmModeModeInfo& m = crtc->mode;
    m_RefreshMilliHz = (m.htotal > 0 && m.vtotal > 0)
                           ? static_cast<int>((static_cast<int64_t>(m.clock) * 1000 * 1000) /
                                              (static_cast<int64_t>(m.htotal) * m.vtotal))
                           : static_cast<int>(m.vrefresh) * 1000;
    drmModeFreeCrtc(crtc);

    m_CrtcIndex = crtcIndex(m_Card, m_CrtcId);
    if (m_CrtcIndex < 0) {
        error = "the CRTC is not in the card's list";
        return false;
    }

    // The planes attached to this CRTC: one primary, one cursor (if the
    // compositor uses a hardware cursor — GNOME does, and on the VM that did
    // not, the pointer was simply inside the primary buffer).
    m_PrimaryPlane = m_CursorPlane = 0;
    drmModePlaneRes* planes = drmModeGetPlaneResources(m_Card);
    for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
        drmModePlane* p = drmModeGetPlane(m_Card, planes->planes[i]);
        if (!p) continue;
        if (p->crtc_id == m_CrtcId) {
            PlaneProps props;
            if (readPlaneProps(m_Card, p->plane_id, props)) {
                if (props.typeValue == DRM_PLANE_TYPE_PRIMARY) {
                    m_PrimaryPlane = p->plane_id;
                } else if (props.typeValue == DRM_PLANE_TYPE_CURSOR) {
                    m_CursorPlane = p->plane_id;
                    m_PropCrtcX = props.crtcX;
                    m_PropCrtcY = props.crtcY;
                    m_PropFbId = props.fbId;
                }
            }
        }
        drmModeFreePlane(p);
    }
    if (planes) drmModeFreePlaneResources(planes);

    if (!m_PrimaryPlane) {
        error = "no primary plane on the display's CRTC";
        return false;
    }
    return true;
}

bool KmsCapture::start(std::string& error)
{
    stop();

    m_Card = ::open(m_CardPath.c_str(), O_RDWR | O_CLOEXEC);
    if (m_Card < 0) {
        error = "cannot open " + m_CardPath + " (" + errnoText() + ")";
        return false;
    }
    // Universal planes is what exposes the primary and cursor planes as
    // objects; without it only overlays are listed and the CRTC's buffer has
    // to be read through the legacy CRTC ioctl, which cannot see the cursor.
    drmSetClientCap(m_Card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    char* render = drmGetRenderDeviceNameFromFd(m_Card);
    m_RenderNode = render ? render : "";
    if (render) ::free(render);

    if (!resolveTopology(error)) {
        stop();
        return false;
    }

    // Prove the privilege now, at start, rather than on the first frame: a
    // capture that opens fine and then hands out zero handles is the confusing
    // version of this failure.
    drmModePlane* p = drmModeGetPlane(m_Card, m_PrimaryPlane);
    if (!p) {
        error = "the primary plane vanished";
        stop();
        return false;
    }
    if (p->fb_id) {
        drmModeFB2* fb = drmModeGetFB2(m_Card, p->fb_id);
        if (!fb) {
            error = "GETFB2 refused (" + errnoText() + ") — a kernel older than 5.4?";
            drmModeFreePlane(p);
            stop();
            return false;
        }
        if (!fb->handles[0]) {
            error = "the kernel withholds framebuffer handles: this process needs CAP_SYS_ADMIN "
                    "(the package sets it on the binary; a build run by hand does not have it)";
            drmModeFreeFB2(fb);
            drmModeFreePlane(p);
            stop();
            return false;
        }
        m_Fourcc = fb->pixel_format;
        drmModeFreeFB2(fb);
    }
    drmModeFreePlane(p);

    m_LastFbId = 0;
    m_CursorFbId = 0;
    m_Cursor = CursorState{};
    m_SteadyOriginUs = steadyNowUs();

    log::info("[native] KMS capture started: " + std::to_string(m_Width) + "x" +
              std::to_string(m_Height) + " @ " + std::to_string(m_RefreshMilliHz / 1000) +
              " Hz on " + m_CardPath + " connector " + std::to_string(m_ConnectorId) + ", crtc " +
              std::to_string(m_CrtcId) + ", primary plane " + std::to_string(m_PrimaryPlane) +
              (m_CursorPlane ? ", cursor plane " + std::to_string(m_CursorPlane)
                             : ", no cursor plane (pointer is in the picture)") +
              ", render node " + m_RenderNode);
    return true;
}

bool KmsCapture::exportFramebuffer(uint32_t fbId, KmsFrame& frame, std::string& error)
{
    drmModeFB2* fb = drmModeGetFB2(m_Card, fbId);
    if (!fb) {
        error = "GETFB2 failed (" + errnoText() + ")";
        return false;
    }
    if (!fb->handles[0]) {
        error = "framebuffer handles withheld (CAP_SYS_ADMIN lost?)";
        drmModeFreeFB2(fb);
        return false;
    }

    frame = KmsFrame{};
    frame.width = static_cast<int>(fb->width);
    frame.height = static_cast<int>(fb->height);
    frame.fourcc = fb->pixel_format;
    frame.modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : 0;

    // One fd per DISTINCT handle: a DCC buffer's three planes share one GEM
    // object and differ by offset, and exporting it three times would be three
    // fds to close for one buffer.
    uint32_t handles[4] = {};
    int fds[4] = {-1, -1, -1, -1};
    m_HeldCount = 0;
    for (int i = 0; i < 4 && fb->handles[i]; ++i) {
        int fd = -1;
        for (int j = 0; j < i; ++j)
            if (handles[j] == fb->handles[i]) fd = fds[j];
        if (fd < 0) {
            if (drmPrimeHandleToFD(m_Card, fb->handles[i], DRM_CLOEXEC | DRM_RDWR, &fd) != 0) {
                error = "PrimeHandleToFD failed (" + errnoText() + ")";
                closeFrameFds();
                drmModeFreeFB2(fb);
                return false;
            }
            m_HeldFds[m_HeldCount++] = fd;
        }
        handles[i] = fb->handles[i];
        fds[i] = fd;
        frame.fds[i] = fd;
        frame.offsets[i] = fb->offsets[i];
        frame.pitches[i] = fb->pitches[i];
        frame.planeCount = i + 1;
    }
    drmModeFreeFB2(fb);

    // GETFB2 hands back GEM handles the caller now owns a reference to; they
    // are per-process and must be closed or they accumulate, one per frame.
    for (int i = 0; i < 4 && handles[i]; ++i) {
        bool dup = false;
        for (int j = 0; j < i; ++j)
            if (handles[j] == handles[i]) dup = true;
        if (!dup) {
            struct drm_gem_close close = {};
            close.handle = handles[i];
            drmIoctl(m_Card, DRM_IOCTL_GEM_CLOSE, &close);
        }
    }
    return true;
}

bool KmsCapture::updateCursor()
{
    if (!m_CursorPlane) return false;

    uint64_t fbId = 0, x = 0, y = 0;
    if (!planeProperty(m_Card, m_CursorPlane, m_PropFbId, fbId)) return false;
    planeProperty(m_Card, m_CursorPlane, m_PropCrtcX, x);
    planeProperty(m_Card, m_CursorPlane, m_PropCrtcY, y);

    bool changed = false;
    const bool visible = fbId != 0;
    // CRTC_X/Y are signed in the kernel (a cursor can be half off the left
    // edge) but the property carries them as unsigned 64-bit.
    const int px = static_cast<int>(static_cast<int32_t>(x));
    const int py = static_cast<int>(static_cast<int32_t>(y));

    if (visible && fbId != m_CursorFbId) {
        // The pointer's image changed. It is a small linear ARGB buffer, read
        // once per shape through a CPU mapping — rare (a shape lasts thousands
        // of frames), and a GPU path for 256 KB would be a lot of machinery for
        // no latency anyone could measure.
        drmModeFB2* fb = drmModeGetFB2(m_Card, static_cast<uint32_t>(fbId));
        if (fb && fb->handles[0]) {
            int fd = -1;
            if (drmPrimeHandleToFD(m_Card, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &fd) == 0) {
                const size_t size =
                    static_cast<size_t>(fb->pitches[0]) * fb->height + fb->offsets[0];
                void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    const int w = static_cast<int>(fb->width);
                    const int h = static_cast<int>(fb->height);
                    m_Cursor.width = w;
                    m_Cursor.height = h;
                    m_Cursor.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
                    m_Cursor.invert.assign(static_cast<size_t>(w) * h, 0);
                    int inkW = 0, inkH = 0;
                    const auto* base = static_cast<const uint8_t*>(map) + fb->offsets[0];
                    for (int row = 0; row < h; ++row) {
                        const uint8_t* src = base + static_cast<size_t>(row) * fb->pitches[0];
                        uint8_t* dst = &m_Cursor.pixels[static_cast<size_t>(row) * w * 4];
                        // ARGB8888 little-endian is B,G,R,A in memory: BGRA, as
                        // CursorState wants it. Straight copy.
                        std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                        for (int col = 0; col < w; ++col) {
                            if (dst[col * 4 + 3] != 0) {
                                if (col + 1 > inkW) inkW = col + 1;
                                if (row + 1 > inkH) inkH = row + 1;
                            }
                        }
                    }
                    m_Cursor.inkWidth = inkW;
                    m_Cursor.inkHeight = inkH;
                    ++m_Cursor.shapeVersion;
                    changed = true;
                    ::munmap(map, size);
                }
                ::close(fd);
            }
            struct drm_gem_close close = {};
            close.handle = fb->handles[0];
            drmIoctl(m_Card, DRM_IOCTL_GEM_CLOSE, &close);
        }
        if (fb) drmModeFreeFB2(fb);
        m_CursorFbId = static_cast<uint32_t>(fbId);
    } else if (!visible) {
        m_CursorFbId = 0;
    }

    if (m_Cursor.visible != visible || m_Cursor.x != px || m_Cursor.y != py) changed = true;
    m_Cursor.visible = visible;
    // KMS positions the plane where the IMAGE goes — the compositor subtracted
    // the hotspot before placing it — which is exactly what CursorState::x/y
    // means. No hotspot to subtract here, and none to report either.
    m_Cursor.x = px;
    m_Cursor.y = py;
    return changed;
}

AcquireStatus KmsCapture::acquire(int timeoutMs, KmsFrame& frame)
{
    if (m_Card < 0) return AcquireStatus::Failed;
    // The frame handed out last time is NOT closed here. It stays valid until
    // a new buffer is exported (below), so a caller that only got a PointerOnly
    // or a Timeout can re-convert the picture it already has with the pointer
    // at its new place — the KMS equivalent of the desktop copy the Windows
    // session keeps for the same purpose, at the cost of no copy at all: the
    // compositor rotates between buffers, and holding the fd of one keeps it.

    const int64_t deadlineUs = steadyNowUs() + static_cast<int64_t>(timeoutMs) * 1000;
    bool cursorMoved = false;

    for (;;) {
        // Wait for the next vblank on OUR crtc. This is the display's clock:
        // the compositor flips at vblank, so a new buffer is visible here within
        // one vblank of being shown — the KMS equivalent of DDA waking the
        // caller on the present.
        drmVBlank vbl = {};
        vbl.request.type =
            static_cast<drmVBlankSeqType>(DRM_VBLANK_RELATIVE | ((static_cast<unsigned>(m_CrtcIndex)
                                                                  << DRM_VBLANK_HIGH_CRTC_SHIFT) &
                                                                 DRM_VBLANK_HIGH_CRTC_MASK));
        vbl.request.sequence = 1;
        if (drmWaitVBlank(m_Card, &vbl) != 0) {
            // EINVAL/EBUSY here is the CRTC going away: a mode change, the
            // screen turning off. Recoverable through start().
            return AcquireStatus::Lost;
        }
        const int64_t vblankUs =
            static_cast<int64_t>(vbl.reply.tval_sec) * 1000000 + vbl.reply.tval_usec;

        cursorMoved |= updateCursor();

        drmModePlane* p = drmModeGetPlane(m_Card, m_PrimaryPlane);
        if (!p) return AcquireStatus::Lost;
        const uint32_t fbId = p->fb_id;
        const uint32_t crtc = p->crtc_id;
        drmModeFreePlane(p);
        if (!fbId || crtc != m_CrtcId) return AcquireStatus::Lost;

        if (fbId != m_LastFbId) {
            // The previous buffer's fds go now, with a new one about to replace
            // it — see the note at the top.
            closeFrameFds();
            std::string error;
            if (!exportFramebuffer(fbId, frame, error)) {
                log::warning("[native] KMS: " + error);
                return AcquireStatus::Lost;
            }
            if (frame.width != m_Width || frame.height != m_Height) {
                // The mode changed under us: the caller rebuilds everything.
                closeFrameFds();
                return AcquireStatus::Lost;
            }
            m_LastFbId = fbId;
            // The vblank timestamp is CLOCK_MONOTONIC, which is what
            // steady_clock is on Linux: same domain, no conversion.
            frame.capturedUs = steadyNowUs();
            frame.presentUs = vblankUs;
            // ⚠️ Capped at "now", for the same reason as WgcCapture. The kernel
            // does not read the vblank's time off a clock when the interrupt
            // fires: it PREDICTS the edge from the scanout position, and the
            // prediction lands a few hundred microseconds ahead of the wake-up
            // (measured: 406 µs on a 780M at 60 Hz). Left alone that is a
            // negative capture latency, and the link governor reads the
            // client's one-way delay off (arrival − present) — a present in the
            // future has it cut the bitrate on a healthy link.
            if (frame.presentUs > frame.capturedUs) frame.presentUs = frame.capturedUs;
            return AcquireStatus::Ok;
        }

        if (steadyNowUs() >= deadlineUs)
            return cursorMoved ? AcquireStatus::PointerOnly : AcquireStatus::Timeout;
    }
}

void KmsCapture::closeFrameFds()
{
    for (int i = 0; i < m_HeldCount; ++i)
        if (m_HeldFds[i] >= 0) ::close(m_HeldFds[i]);
    m_HeldCount = 0;
}

void KmsCapture::release()
{
    closeFrameFds();
}

void KmsCapture::stop()
{
    closeFrameFds();
    if (m_Card >= 0) ::close(m_Card);
    m_Card = -1;
    m_CrtcId = 0;
    m_PrimaryPlane = m_CursorPlane = 0;
    m_LastFbId = 0;
}

} // namespace mw::native::capture
