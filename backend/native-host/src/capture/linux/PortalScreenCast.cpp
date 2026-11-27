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
#include "../../core/PortalCursorMode.h"
#include "KwinVirtualOutput.h"
#include "MutterScreenCast.h"

#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern char** environ;

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
    bool hasPosition = false;
    int x = 0;
    int y = 0;
};

/// Read the streams array of a Start response: a(ua{sv}), of which we take the
/// first entry's node id and, when they are there, its size and position.
void readStreams(sd_bus_message* m, Answer& out)
{
    if (sd_bus_message_enter_container(m, 'v', "a(ua{sv})") <= 0) return;
    if (sd_bus_message_enter_container(m, 'a', "(ua{sv})") > 0) {
        if (sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0) {
            sd_bus_message_read(m, "u", &out.nodeId);
            // The stream's own properties. "size" is (ii) inside a variant and
            // is advisory: PipeWire's negotiated format is the truth, this only
            // saves a round trip when the portal bothers to say. "position" is
            // (ii) too, and is the one thing only the portal can say: where
            // the monitor sits in the compositor's space, for the absolute
            // pointer (issue #18).
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
                    } else if (key && std::strcmp(key, "position") == 0 &&
                               sd_bus_message_enter_container(m, 'v', "(ii)") > 0) {
                        int32_t x = 0, y = 0;
                        if (sd_bus_message_read(m, "(ii)", &x, &y) > 0) {
                            out.x = x;
                            out.y = y;
                            out.hasPosition = true;
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

// ── The helper (see the header: who may ask) ────────────────────────────────

/// Set in the helper's environment, with the descriptor its socket sits on.
constexpr const char* kHelperVariable = "MW_PORTAL_HELPER";
constexpr int kHelperFd = 3;
constexpr const char* kHelperFdText = "3";

/// True in the helper process: its handshake runs here, whatever it holds.
std::atomic<bool> g_InHelper{false};

/// Whether the portal would be refused a look at this process. It opens
/// /proc/<pid>/root, which the kernel grants a caller holding no capability
/// only on a process that holds none either, and that may be dumped.
bool unreadableByPortal()
{
    if (::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 1) return true;
    FILE* status = std::fopen("/proc/self/status", "r");
    if (!status) return false;
    char line[256];
    bool held = false;
    while (std::fgets(line, sizeof(line), status)) {
        unsigned long long caps = 0;
        if (std::sscanf(line, "CapPrm: %llx", &caps) == 1) {
            held = caps != 0;
            break;
        }
    }
    std::fclose(status);
    return held;
}

/// @p text cut at its first @p parts - 1 newlines; the last part keeps the
/// rest, newlines and all (an error's text, an empty token).
std::vector<std::string> splitLines(const std::string& text, size_t parts)
{
    std::vector<std::string> out;
    size_t from = 0;
    while (out.size() + 1 < parts) {
        const size_t end = text.find('\n', from);
        if (end == std::string::npos) break;
        out.push_back(text.substr(from, end - from));
        from = end + 1;
    }
    out.push_back(text.substr(from));
    return out;
}

/// The helper's whole life, in the process startInHelper exec'd: one request,
/// its handshake, the answer with the PipeWire descriptor — then the portal
/// session held until the socket closes, which is the stream's stop() or its
/// owner's death. The session is this connection's: closing it ends the cast.
/// Two requests: "1" the portal (source, timeout, token), "2" KWin's virtual
/// output (width, height, name), whose node needs no descriptor. The source is
/// "0" a monitor, "1" a virtual one, "2" a virtual one with the pointer painted
/// in (setEmbedCursor).
int helperMain(int socket)
{
    char buffer[4096];
    ssize_t n = 0;
    do {
        n = ::recv(socket, buffer, sizeof(buffer), 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return 1;
    const std::vector<std::string> request =
        splitLines(std::string(buffer, static_cast<size_t>(n)), 4);
    if (request.size() != 4 || (request[0] != "1" && request[0] != "2")) return 1;

    PortalScreenCast cast;
    PortalStream stream;
    std::string error;
    bool ok = false;
    if (request[0] == "2") {
        cast.setKwinVirtualOutput(request[3], std::atoi(request[1].c_str()),
                                  std::atoi(request[2].c_str()));
        ok = cast.start(std::string(), 0, stream, error);
    } else {
        cast.setVirtual(request[1] != "0");
        cast.setEmbedCursor(request[1] == "2");
        ok = cast.start(request[3], std::atoi(request[2].c_str()), stream, error);
    }
    std::string reply =
        ok ? "ok\n" + std::to_string(stream.nodeId) + "\n" + std::to_string(stream.width) + "\n" +
                 std::to_string(stream.height) + "\n" + (stream.hasPosition ? "1" : "0") + "\n" +
                 std::to_string(stream.x) + "\n" + std::to_string(stream.y) + "\n" +
                 std::to_string(stream.cursorMode) + "\n" + stream.restoreToken
           : "error\n" + error;
    iovec part = {reply.data(), reply.size()};
    msghdr message = {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    if (ok && stream.pipewireFd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(rights), &stream.pipewireFd, sizeof(int));
    }
    const bool sent = ::sendmsg(socket, &message, MSG_NOSIGNAL) >= 0;
    if (stream.pipewireFd >= 0) ::close(stream.pipewireFd);
    if (!ok || !sent) return 1;

    char byte = 0;
    while (::recv(socket, &byte, 1, 0) < 0 && errno == EINTR) {}
    cast.stop();
    return 0;
}

/// The helper's way in: before main(), in whichever binary startInHelper
/// exec'd — the app, the tests, the bench — since every one of them links this
/// file. Anywhere else the variable is absent and this returns at once.
__attribute__((constructor)) void portalHelperEntry()
{
    const char* value = std::getenv(kHelperVariable);
    if (!value || std::strcmp(value, kHelperFdText) != 0) return;
    ::unsetenv(kHelperVariable);
    g_InHelper.store(true);
    std::_Exit(helperMain(kHelperFd));
}

} // namespace

struct PortalScreenCast::Impl
{
    sd_bus* bus = nullptr;
    std::string session;
    int calls = 0; ///< makes each handle token unique within one session
    bool virtualMonitor = false;
    /// The pointer painted into the pictures (setEmbedCursor).
    bool embedCursor = false;
    pid_t helper = -1;     ///< the helper holding the session, when one does
    int helperSocket = -1; ///< its socket: closing it is the helper's cue to end

    /// KWin's virtual output instead of the portal (setKwinVirtualOutput).
    bool kwin = false;
    std::string kwinName;
    int kwinWidth = 0;
    int kwinHeight = 0;
    std::unique_ptr<KwinVirtualOutput> kwinOutput;

    /// Mutter's own screen cast instead of the portal (setMutter).
    bool mutter = false;
    MutterScreenCast::Request mutterRequest;
    std::unique_ptr<MutterScreenCast> mutterCast;
    /// Its last start was refused outright (routeRefused).
    bool mutterRefused = false;

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

uint32_t PortalScreenCast::sourceTypes()
{
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) return 0;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    uint32_t types = 0;
    if (sd_bus_get_property_trivial(bus, kBus, kObject, kScreenCast, "AvailableSourceTypes", &error,
                                    'u', &types) < 0)
        types = 0;
    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return types;
}

void PortalScreenCast::setVirtual(bool virtualMonitor)
{
    d->virtualMonitor = virtualMonitor;
}

void PortalScreenCast::setKwinVirtualOutput(const std::string& name, int width, int height)
{
    d->kwin = true;
    d->virtualMonitor = true;
    d->kwinName = name;
    d->kwinWidth = width;
    d->kwinHeight = height;
}

void PortalScreenCast::setMutter(const std::string& connector, int width, int height, int refreshHz)
{
    d->mutter = true;
    d->virtualMonitor = connector.empty();
    d->mutterRequest.connector = connector;
    d->mutterRequest.width = width;
    d->mutterRequest.height = height;
    d->mutterRequest.refreshHz = refreshHz;
}

void PortalScreenCast::setEmbedCursor(bool embed)
{
    d->embedCursor = embed;
}

bool PortalScreenCast::ended()
{
    return d->mutterCast && d->mutterCast->closed();
}

bool PortalScreenCast::routeRefused() const
{
    return d->mutterRefused;
}

bool PortalScreenCast::start(const std::string& restoreToken, int timeoutMs, PortalStream& out,
                             std::string& error)
{
    // Mutter's own screen cast asks nobody and looks at no caller: here,
    // whatever this process holds.
    if (d->mutter) {
        out = PortalStream{};
        d->mutterCast = std::make_unique<MutterScreenCast>();
        d->mutterRequest.embedCursor = d->embedCursor;
        uint32_t node = 0;
        if (!d->mutterCast->start(d->mutterRequest, node, error)) {
            d->mutterRefused = d->mutterCast->refused();
            d->mutterCast.reset();
            return false;
        }
        out.nodeId = node;
        out.sessionPipeWire = true;
        const std::string& connector = d->mutterRequest.connector;
        log::info("[native] GNOME: " +
                  (connector.empty() ? std::string("a virtual monitor")
                                     : "monitor " + connector + ", recorded as it is") +
                  ", node " + std::to_string(node) + " — Mutter's own screen cast, no dialog" +
                  (d->embedCursor ? ", the pointer painted in" : ""));
        return true;
    }
    const bool helper = !g_InHelper.load() && unreadableByPortal();
    if (!(helper ? startInHelper(restoreToken, timeoutMs, out, error)
                 : startHere(restoreToken, timeoutMs, out, error)))
        return false;
    // Said by the stream's own process: the helper has no one to say it to.
    if (g_InHelper.load()) return true;
    if (d->kwin) {
        log::info("[native] KWin: virtual output \"" + d->kwinName + "\" " +
                  std::to_string(d->kwinWidth) + "x" + std::to_string(d->kwinHeight) + ", node " +
                  std::to_string(out.nodeId) +
                  (helper ? " (asked through a helper without capabilities, which KWin would "
                            "not trust)"
                          : ""));
        return true;
    }
    log::info(
        std::string("[native] portal: ") + (d->virtualMonitor ? "virtual monitor, " : "") +
        (d->embedCursor ? "the pointer painted in, " : "") + "node " + std::to_string(out.nodeId) +
        (out.width > 0 ? " (" + std::to_string(out.width) + "x" + std::to_string(out.height) + ")"
                       : std::string()) +
        (out.hasPosition ? " at " + std::to_string(out.x) + "," + std::to_string(out.y) +
                               " in the compositor's space"
                         : std::string()) +
        (out.restoreToken.empty() ? " — no restore token, the dialog will come back"
                                  : " — restore token kept, later sessions are silent") +
        (helper ? " (asked through a helper without capabilities, which the portal would "
                  "refuse)"
                : ""));
    return true;
}

bool PortalScreenCast::startInHelper(const std::string& restoreToken, int timeoutMs,
                                     PortalStream& out, std::string& error)
{
    out = PortalStream{};
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) {
        error = std::string("the portal helper's socket: ") + std::strerror(errno);
        return false;
    }
    // Everything exec needs is made before the fork: the child of a threaded
    // process may only call what is async-signal-safe, and a string allocates.
    const std::string variable = std::string(kHelperVariable) + "=" + kHelperFdText;
    const size_t nameLength = std::strlen(kHelperVariable) + 1;
    std::vector<char*> env;
    for (char** e = environ; e && *e; ++e)
        if (std::strncmp(*e, variable.c_str(), nameLength) != 0) env.push_back(*e);
    env.push_back(const_cast<char*>(variable.c_str()));
    env.push_back(nullptr);
    char name[] = "mw-portal-helper";
    char* argv[] = {name, nullptr};

    // _Fork, not fork: no atfork handler of a driver or a library runs in a
    // child that only talks to the kernel and then execs.
    const pid_t pid = ::_Fork();
    if (pid < 0) {
        error = std::string("cannot start the portal helper: ") + std::strerror(errno);
        ::close(pair[0]);
        ::close(pair[1]);
        return false;
    }
    if (pid == 0) {
        if (::dup2(pair[1], kHelperFd) < 0) ::_exit(126);
        ::fcntl(kHelperFd, F_SETFD, 0);
        // Nothing else of this process goes with it: DRM cards, buffers and
        // sockets stay here.
#if defined(SYS_close_range)
        if (::syscall(SYS_close_range, static_cast<unsigned long>(kHelperFd + 1),
                      static_cast<unsigned long>(~0U), 0UL) != 0)
#endif
            for (int fd = kHelperFd + 1; fd < 65536; ++fd)
                ::close(fd);
        // Every capability dropped, and none to be had again at exec — not
        // from the ambient set, not from the file's own (a setcap'd binary).
        // A process that gains nothing at exec is dumpable, and not AT_SECURE:
        // the bus address in its environment is read as usual.
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
        __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
        __user_cap_data_struct none[2] = {};
        ::syscall(SYS_capset, &header, none);
        ::execve("/proc/self/exe", argv, env.data());
        ::_exit(127);
    }
    ::close(pair[1]);
    d->helper = pid;
    d->helperSocket = pair[0];

    const char* source = !d->virtualMonitor ? "0" : d->embedCursor ? "2" : "1";
    const std::string request = d->kwin ? "2\n" + std::to_string(d->kwinWidth) + "\n" +
                                              std::to_string(d->kwinHeight) + "\n" + d->kwinName
                                        : std::string("1\n") + source + "\n" +
                                              std::to_string(timeoutMs) + "\n" + restoreToken;
    if (::send(d->helperSocket, request.data(), request.size(), MSG_NOSIGNAL) < 0) {
        error = std::string("the portal helper took no request: ") + std::strerror(errno);
        stopHelper();
        return false;
    }

    // As long as the handshake may take in the helper: the dialog's wait, and
    // the quick steps around it.
    const int waitMs = (timeoutMs > 0 ? timeoutMs : 120000) + 45000;
    pollfd ready = {d->helperSocket, POLLIN, 0};
    int r = 0;
    do {
        r = ::poll(&ready, 1, waitMs);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
        error = "the portal helper never answered";
        stopHelper();
        return false;
    }
    char buffer[4096];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    iovec part = {buffer, sizeof(buffer)};
    msghdr message = {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t n = 0;
    do {
        n = ::recvmsg(d->helperSocket, &message, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    int fd = -1;
    for (cmsghdr* c = n > 0 ? CMSG_FIRSTHDR(&message) : nullptr; c; c = CMSG_NXTHDR(&message, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
    if (n <= 0) {
        error = "the portal helper went away without an answer";
        stopHelper();
        return false;
    }
    const std::vector<std::string> reply =
        splitLines(std::string(buffer, static_cast<size_t>(n)), 9);
    if (reply.front() != "ok") {
        error =
            reply.size() > 1 && reply.front() == "error" ? reply[1] : "the portal helper failed";
        if (fd >= 0) ::close(fd);
        stopHelper();
        return false;
    }
    // KWin's node is on the session's PipeWire: no descriptor comes with it.
    if ((fd < 0 && !d->kwin) || reply.size() != 9) {
        error = "the portal helper's answer carried no PipeWire descriptor";
        if (fd >= 0) ::close(fd);
        stopHelper();
        return false;
    }
    // The cursor_mode asked of the portal: one of its three, or none on KWin's
    // virtual output, which asks no portal for one. Anything else is a helper
    // gone wrong, not a pointer to guess about.
    const std::string& mode = reply[7];
    if (!(mode == "1" || mode == "2" || mode == "4" || (d->kwin && mode == "0"))) {
        error = "the portal helper's answer named no cursor mode";
        if (fd >= 0) ::close(fd);
        stopHelper();
        return false;
    }
    out.nodeId = static_cast<uint32_t>(std::strtoul(reply[1].c_str(), nullptr, 10));
    out.pipewireFd = fd;
    out.width = std::atoi(reply[2].c_str());
    out.height = std::atoi(reply[3].c_str());
    out.hasPosition = reply[4] == "1";
    out.x = std::atoi(reply[5].c_str());
    out.y = std::atoi(reply[6].c_str());
    out.cursorMode = static_cast<uint32_t>(mode[0] - '0');
    out.restoreToken = reply[8];
    out.sessionPipeWire = d->kwin;
    return true;
}

void PortalScreenCast::stopHelper()
{
    // Its cue: it closes the portal session and leaves.
    if (d->helperSocket >= 0) ::close(d->helperSocket);
    d->helperSocket = -1;
    if (d->helper <= 0) return;
    // A helper stuck on the bus is not waited on for ever.
    for (int waited = 0; waited < 3000; waited += 10) {
        const pid_t r = ::waitpid(d->helper, nullptr, WNOHANG);
        if (r == d->helper || (r < 0 && errno == ECHILD)) {
            d->helper = -1;
            return;
        }
        ::usleep(10 * 1000);
    }
    ::kill(d->helper, SIGKILL);
    ::waitpid(d->helper, nullptr, 0);
    d->helper = -1;
}

bool PortalScreenCast::startHere(const std::string& restoreToken, int timeoutMs, PortalStream& out,
                                 std::string& error)
{
    out = PortalStream{};
    if (d->kwin) {
        d->kwinOutput = std::make_unique<KwinVirtualOutput>();
        uint32_t node = 0;
        if (!d->kwinOutput->start(d->kwinName, d->kwinWidth, d->kwinHeight, node, error)) {
            d->kwinOutput.reset();
            return false;
        }
        out.nodeId = node;
        out.sessionPipeWire = true;
        return true;
    }
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

    // The pointers this portal can hand over. One it does not offer fails the
    // whole SelectSources, and the stream with it: xdg-desktop-portal-wlr 0.7
    // has no METADATA, so a Sway desktop got no picture at all.
    uint32_t offeredCursorModes = 0;
    {
        sd_bus_error modesError = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_trivial(d->bus, kBus, kObject, kScreenCast, "AvailableCursorModes",
                                        &modesError, 'u', &offeredCursorModes) < 0)
            offeredCursorModes = 0;
        sd_bus_error_free(&modesError);
    }
    const uint32_t cursorMode = portalCursorMode(offeredCursorModes, d->embedCursor);
    if (cursorMode != (d->embedCursor ? kPortalCursorEmbedded : kPortalCursorMetadata))
        log::info(
            "[native] portal: no pointer beside the pictures here (cursor modes " +
            std::to_string(offeredCursorModes) + "): " +
            (cursorMode == kPortalCursorEmbedded ? "it is painted into them" : "none at all"));

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
                // lets the client keep drawing its own. 2 = EMBEDDED where
                // GNOME paints it into the pictures anyway (setEmbedCursor),
                // and where the portal offers no METADATA (portalCursorMode).
                // persist_mode 2 = remember until the user revokes it, which is
                // what buys a restore token and, with it, silence next time.
                // types 4 = VIRTUAL instead: a monitor made for this session.
                const uint32_t types = d->virtualMonitor ? kSourceVirtual : kSourceMonitor;
                if (restoreToken.empty())
                    return sd_bus_message_append(m, "a{sv}", 5, "handle_token", "s", token.c_str(),
                                                 "types", "u", types, "multiple", "b", 0,
                                                 "cursor_mode", "u", cursorMode, "persist_mode",
                                                 "u", uint32_t{2});
                return sd_bus_message_append(m, "a{sv}", 6, "handle_token", "s", token.c_str(),
                                             "types", "u", types, "multiple", "b", 0, "cursor_mode",
                                             "u", cursorMode, "persist_mode", "u", uint32_t{2},
                                             "restore_token", "s", restoreToken.c_str());
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
    out.hasPosition = started.hasPosition;
    out.x = started.x;
    out.y = started.y;
    out.cursorMode = cursorMode;
    return true;
}

void PortalScreenCast::stop()
{
    if (d->mutterCast) {
        d->mutterCast->stop();
        d->mutterCast.reset();
    }
    if (d->helper > 0 || d->helperSocket >= 0) {
        stopHelper();
        return;
    }
    if (d->kwinOutput) {
        d->kwinOutput->stop();
        d->kwinOutput.reset();
    }
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
