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

#include "PortalScreenCast.h"

#include "../../core/Log.h"

#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mw::native::capture {
namespace {

constexpr const char* kBus = "org.freedesktop.portal.Desktop";
constexpr const char* kObject = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequest = "org.freedesktop.portal.Request";

/// Our unique bus name as the portal spells it in an object path: the leading
/// ':' dropped, every '.' an underscore. Getting this wrong is the classic way
/// to wait for a signal that is being sent somewhere else.
std::string senderToken(sd_bus* bus)
{
    const char* unique = nullptr;
    if (sd_bus_get_unique_name(bus, &unique) < 0 || !unique) return {};
    if (*unique == ':') ++unique;
    std::string out;
    out.reserve(std::strlen(unique));
    for (const char* p = unique; *p; ++p)
        out.push_back(*p == '.' ? '_' : *p);
    return out;
}

/// What a Response signal carried, of the few keys we asked about.
struct Answer
{
    bool arrived = false;
    uint32_t code = 0; ///< 0 accepted, 1 cancelled by the user, 2 failed
    std::string sessionHandle;
    std::string restoreToken;
    uint32_t nodeId = 0;
    int width = 0;
    int height = 0;
};

/// Read the streams array of a Start response: a(ua{sv}), of which we take the
/// first entry's node id and, when it is there, its size.
void readStreams(sd_bus_message* m, Answer& out)
{
    if (sd_bus_message_enter_container(m, 'v', "a(ua{sv})") <= 0) return;
    if (sd_bus_message_enter_container(m, 'a', "(ua{sv})") > 0) {
        if (sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0) {
            sd_bus_message_read(m, "u", &out.nodeId);
            // The stream's own properties. "size" is (ii) inside a variant and
            // is advisory: PipeWire's negotiated format is the truth, this only
            // saves a round trip when the portal bothers to say.
            if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                    const char* key = nullptr;
                    sd_bus_message_read(m, "s", &key);
                    if (key && std::strcmp(key, "size") == 0 &&
                        sd_bus_message_enter_container(m, 'v', "(ii)") > 0) {
                        int32_t w = 0, h = 0;
                        if (sd_bus_message_read(m, "(ii)", &w, &h) > 0) {
                            out.width = w;
                            out.height = h;
                        }
                        sd_bus_message_exit_container(m);
                    } else {
                        sd_bus_message_skip(m, "v");
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
}

/// a{sv} of results, keeping the three keys we use and stepping over the rest.
///
/// The skipping is not politeness: portals differ and gain keys between
/// versions, and a reader that stops at the first unknown one breaks on the
/// next desktop it meets.
void readResults(sd_bus_message* m, Answer& out)
{
    if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) return;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* key = nullptr;
        sd_bus_message_read(m, "s", &key);
        const char* value = nullptr;
        if (key && std::strcmp(key, "session_handle") == 0) {
            if (sd_bus_message_read(m, "v", "s", &value) > 0 && value) out.sessionHandle = value;
        } else if (key && std::strcmp(key, "restore_token") == 0) {
            if (sd_bus_message_read(m, "v", "s", &value) > 0 && value) out.restoreToken = value;
        } else if (key && std::strcmp(key, "streams") == 0) {
            readStreams(m, out);
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
}

int onResponse(sd_bus_message* m, void* userdata, sd_bus_error*)
{
    auto* out = static_cast<Answer*>(userdata);
    if (sd_bus_message_read(m, "u", &out->code) < 0) return 0;
    readResults(m, *out);
    out->arrived = true;
    return 0;
}

const char* describe(uint32_t code)
{
    switch (code) {
    case 0: return "accepted";
    case 1: return "the user cancelled the dialog";
    default: return "the portal refused";
    }
}

} // namespace

struct PortalScreenCast::Impl
{
    sd_bus* bus = nullptr;
    std::string session;
    int calls = 0; ///< makes each handle token unique within one session

    ~Impl()
    {
        if (bus) sd_bus_unref(bus);
    }
};

PortalScreenCast::PortalScreenCast()
    : d(std::make_unique<Impl>())
{}

PortalScreenCast::~PortalScreenCast()
{
    stop();
}

bool PortalScreenCast::available(std::string& reason)
{
    sd_bus* bus = nullptr;
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        reason = std::string("no session bus (") + std::strerror(-r) +
                 ") — a service without a user session has no portal";
        return false;
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    uint32_t version = 0;
    r = sd_bus_get_property_trivial(bus, kBus, kObject, kScreenCast, "version", &error, 'u',
                                    &version);
    const std::string message = error.message ? error.message : std::strerror(r < 0 ? -r : 0);
    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    if (r < 0) {
        reason = "no ScreenCast portal on this desktop (" + message + ")";
        return false;
    }
    reason = "ScreenCast portal version " + std::to_string(version);
    return true;
}

bool PortalScreenCast::start(const std::string& restoreToken, int timeoutMs, PortalStream& out,
                             std::string& error)
{
    out = PortalStream{};
    if (!d->bus) {
        const int r = sd_bus_open_user(&d->bus);
        if (r < 0) {
            error = std::string("no session bus: ") + std::strerror(-r);
            d->bus = nullptr;
            return false;
        }
    }
    const std::string sender = senderToken(d->bus);
    if (sender.empty()) {
        error = "the session bus gave us no unique name";
        return false;
    }

    // One step of the conversation: subscribe to the path the answer will come
    // on, make the call, then pump the bus until the signal lands.
    const auto step = [&](const char* method, const auto& fillArguments, Answer& answer,
                          int waitMs) -> bool {
        const std::string token = "mw" + std::to_string(++d->calls);
        const std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;

        sd_bus_slot* slot = nullptr;
        int r = sd_bus_match_signal(d->bus, &slot, kBus, path.c_str(), kRequest, "Response",
                                    onResponse, &answer);
        if (r < 0) {
            error = std::string("cannot listen for the portal's answer: ") + std::strerror(-r);
            return false;
        }

        sd_bus_message* call = nullptr;
        r = sd_bus_message_new_method_call(d->bus, &call, kBus, kObject, kScreenCast, method);
        if (r >= 0) r = fillArguments(call, token);
        if (r < 0) {
            sd_bus_message_unref(call);
            sd_bus_slot_unref(slot);
            error = std::string(method) + ": cannot build the call: " + std::strerror(-r);
            return false;
        }

        sd_bus_error busError = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        r = sd_bus_call(d->bus, call, 30ULL * 1000 * 1000, &busError, &reply);
        sd_bus_message_unref(call);
        if (r < 0) {
            error = std::string(method) +
                    " failed: " + (busError.message ? busError.message : std::strerror(-r));
            sd_bus_error_free(&busError);
            sd_bus_slot_unref(slot);
            return false;
        }
        sd_bus_message_unref(reply);
        sd_bus_error_free(&busError);

        // The answer, whenever it comes. 100 ms slices so a cancelled session
        // does not sit here for the whole timeout.
        for (int waited = 0; !answer.arrived && waited < waitMs; waited += 100) {
            const int processed = sd_bus_process(d->bus, nullptr);
            if (processed > 0) {
                waited -= 100; // work happened; do not spend the budget on it
                continue;
            }
            if (processed < 0) break;
            sd_bus_wait(d->bus, 100ULL * 1000);
        }
        sd_bus_slot_unref(slot);
        if (!answer.arrived) {
            error = std::string(method) + ": the portal never answered";
            return false;
        }
        if (answer.code != 0) {
            error = std::string(method) + ": " + describe(answer.code);
            return false;
        }
        return true;
    };

    Answer created;
    if (!step(
            "CreateSession",
            [](sd_bus_message* m, const std::string& token) {
                return sd_bus_message_append(m, "a{sv}", 2, "handle_token", "s", token.c_str(),
                                             "session_handle_token", "s", "moonlightweb");
            },
            created, 15000))
        return false;
    d->session = created.sessionHandle;
    if (d->session.empty()) {
        error = "the portal opened a session but did not name it";
        return false;
    }

    Answer selected;
    if (!step(
            "SelectSources",
            [&](sd_bus_message* m, const std::string& token) {
                int r = sd_bus_message_append(m, "o", d->session.c_str());
                if (r < 0) return r;
                // types 1 = monitor (never a single window: this is a desktop
                // host). cursor_mode 4 = METADATA, so the pointer arrives
                // beside the picture rather than burnt into it — the same
                // contract CursorState carries everywhere else, which is what
                // lets the client keep drawing its own.
                // persist_mode 2 = remember until the user revokes it, which is
                // what buys a restore token and, with it, silence next time.
                if (restoreToken.empty())
                    return sd_bus_message_append(m, "a{sv}", 5, "handle_token", "s", token.c_str(),
                                                 "types", "u", uint32_t{1}, "multiple", "b", 0,
                                                 "cursor_mode", "u", uint32_t{4}, "persist_mode",
                                                 "u", uint32_t{2});
                return sd_bus_message_append(
                    m, "a{sv}", 6, "handle_token", "s", token.c_str(), "types", "u", uint32_t{1},
                    "multiple", "b", 0, "cursor_mode", "u", uint32_t{4}, "persist_mode", "u",
                    uint32_t{2}, "restore_token", "s", restoreToken.c_str());
            },
            selected, 15000))
        return false;

    // ⚠️ The one that waits on a human, unless the restore token replays an
    // earlier grant.
    Answer started;
    if (!step(
            "Start",
            [&](sd_bus_message* m, const std::string& token) {
                int r = sd_bus_message_append(m, "os", d->session.c_str(), "");
                if (r < 0) return r;
                return sd_bus_message_append(m, "a{sv}", 1, "handle_token", "s", token.c_str());
            },
            started, timeoutMs > 0 ? timeoutMs : 120000))
        return false;
    if (started.nodeId == 0) {
        error = "the portal accepted but named no PipeWire node";
        return false;
    }

    // The fd to connect PipeWire on. An ordinary call: no Request, no signal.
    sd_bus_message* call = nullptr;
    int r = sd_bus_message_new_method_call(d->bus, &call, kBus, kObject, kScreenCast,
                                           "OpenPipeWireRemote");
    if (r >= 0) r = sd_bus_message_append(call, "o", d->session.c_str());
    if (r >= 0) r = sd_bus_message_append(call, "a{sv}", 0);
    if (r < 0) {
        sd_bus_message_unref(call);
        error = std::string("cannot ask for the PipeWire fd: ") + std::strerror(-r);
        return false;
    }
    sd_bus_error busError = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    r = sd_bus_call(d->bus, call, 10ULL * 1000 * 1000, &busError, &reply);
    sd_bus_message_unref(call);
    if (r < 0) {
        error = std::string("OpenPipeWireRemote failed: ") +
                (busError.message ? busError.message : std::strerror(-r));
        sd_bus_error_free(&busError);
        return false;
    }
    sd_bus_error_free(&busError);
    int fd = -1;
    r = sd_bus_message_read(reply, "h", &fd);
    // The fd belongs to the message; it closes with it, so take a copy that
    // outlives this function.
    out.pipewireFd = fd >= 0 ? ::dup(fd) : -1;
    sd_bus_message_unref(reply);
    if (r < 0 || out.pipewireFd < 0) {
        error = "the portal returned no usable PipeWire descriptor";
        return false;
    }

    out.nodeId = started.nodeId;
    out.restoreToken = started.restoreToken.empty() ? selected.restoreToken : started.restoreToken;
    out.width = started.width;
    out.height = started.height;
    log::info("[native] portal: node " + std::to_string(out.nodeId) +
              (out.width > 0
                   ? " (" + std::to_string(out.width) + "x" + std::to_string(out.height) + ")"
                   : std::string()) +
              (out.restoreToken.empty() ? " — no restore token, the dialog will come back"
                                        : " — restore token kept, later sessions are silent"));
    return true;
}

void PortalScreenCast::stop()
{
    if (!d->bus) return;
    if (!d->session.empty()) {
        // Closing the session is a courtesy: dropping the bus would do it. It
        // is done explicitly so the compositor's "screen is being shared"
        // indicator goes away at the moment the stream stops, not whenever the
        // process happens to exit.
        sd_bus_call_method(d->bus, kBus, d->session.c_str(), "org.freedesktop.portal.Session",
                           "Close", nullptr, nullptr, "");
        d->session.clear();
    }
    sd_bus_unref(d->bus);
    d->bus = nullptr;
}

} // namespace mw::native::capture
