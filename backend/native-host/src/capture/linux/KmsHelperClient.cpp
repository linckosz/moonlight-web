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

#include "KmsHelperClient.h"

#include "KmsHelperProtocol.h"
#include "../../platform/linux/ScopedCapability.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mw::native::capture {

namespace {

/// How long one answer may take. GETFB2 and an export are microseconds; past
/// this the helper is stuck, and the capture is better off failing loudly.
constexpr int kAnswerMs = 1000;

std::string errnoName(int e)
{
    return std::strerror(e);
}

} // namespace

void KmsBuffer::close()
{
    for (int i = 0; i < fdCount; ++i)
        if (fds[i] >= 0) ::close(fds[i]);
    for (int& fd : fds)
        fd = -1;
    for (int& index : fdIndex)
        index = -1;
    fdCount = 0;
    planeCount = 0;
}

KmsHelperClient::~KmsHelperClient()
{
    stop();
}

std::string KmsHelperClient::helperPath()
{
    char exe[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return {};
    exe[n] = '\0';
    std::string path(exe);
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return {};
    path = path.substr(0, slash + 1) + "moonlightweb-kms";
    return ::access(path.c_str(), X_OK) == 0 ? path : std::string();
}

bool KmsHelperClient::wanted()
{
    return !platform::ScopedCapability::permitted(CAP_SYS_ADMIN) && !helperPath().empty();
}

int KmsHelperClient::cardIndex(const std::string& cardPath)
{
    static const char kPrefix[] = "/dev/dri/card";
    if (cardPath.compare(0, sizeof kPrefix - 1, kPrefix) != 0) return -1;
    const std::string digits = cardPath.substr(sizeof kPrefix - 1);
    if (digits.empty() || digits.size() > 2) return -1;
    int index = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return -1;
        index = index * 10 + (c - '0');
    }
    return index <= static_cast<int>(MW_KMS_MAX_CARD) ? index : -1;
}

bool KmsHelperClient::start(std::string& error)
{
    const std::string path = helperPath();
    if (path.empty()) {
        error = "moonlightweb-kms is not next to this program";
        return false;
    }
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        error = "socketpair: " + errnoName(errno);
        return false;
    }
    // The helper's end goes to its descriptor MW_KMS_FD. Kept off that number
    // here first: a dup2 onto itself would leave it close-on-exec.
    int theirs = sv[1];
    if (theirs == MW_KMS_FD) {
        theirs = ::fcntl(sv[1], F_DUPFD_CLOEXEC, MW_KMS_FD + 1);
        ::close(sv[1]);
        if (theirs < 0) {
            error = "fcntl: " + errnoName(errno);
            ::close(sv[0]);
            return false;
        }
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, theirs, MW_KMS_FD);
    char* const argv[] = {const_cast<char*>(path.c_str()), nullptr};
    char* const envp[] = {nullptr}; // it reads none
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    ::close(theirs);
    if (spawned != 0) {
        error = "cannot start " + path + ": " + errnoName(spawned);
        ::close(sv[0]);
        return false;
    }
    m_Socket = sv[0];
    m_Pid = pid;
    return true;
}

void KmsHelperClient::stop()
{
    if (m_Socket >= 0) {
        ::close(m_Socket); // the helper reads EOF and exits
        m_Socket = -1;
    }
    if (m_Pid > 0) {
        int status = 0;
        // It exits on EOF at once; give it a moment, then make sure.
        for (int i = 0; i < 50 && ::waitpid(m_Pid, &status, WNOHANG) == 0; ++i)
            ::usleep(2000);
        if (::waitpid(m_Pid, &status, WNOHANG) == 0) {
            ::kill(m_Pid, SIGKILL);
            ::waitpid(m_Pid, &status, 0);
        }
        m_Pid = -1;
    }
}

bool KmsHelperClient::read(uint32_t card, uint32_t crtcId, uint32_t planeId, uint32_t fbId,
                           KmsBuffer& out, std::string& error, int* errorCode)
{
    // A helper found gone is started again once, so no frame is lost to it:
    // PR_SET_PDEATHSIG follows the THREAD that started it, not this process, so
    // it dies with a thread that ends while the capture goes on — the session's
    // start, say, when it is the one that opened the capture.
    for (int attempt = 0; attempt < 2; ++attempt) {
        out = KmsBuffer{};
        if (errorCode) *errorCode = 0;
        if (m_Socket < 0 && !start(error)) return false;
        const Exchange result = exchange(card, crtcId, planeId, fbId, out, error, errorCode);
        if (result != Exchange::Gone) return result == Exchange::Answered;
    }
    return false;
}

KmsHelperClient::Exchange KmsHelperClient::exchange(uint32_t card, uint32_t crtcId,
                                                    uint32_t planeId, uint32_t fbId, KmsBuffer& out,
                                                    std::string& error, int* errorCode)
{
    mw_kms_request request{};
    request.magic = MW_KMS_REQUEST_MAGIC;
    request.op = MW_KMS_OP_PLANE_FB;
    request.seq = ++m_Seq;
    request.card = card;
    request.crtc_id = crtcId;
    request.plane_id = planeId;
    request.fb_id = fbId;
    if (!mw_kms_request_valid(&request, sizeof request)) {
        error = "not a request moonlightweb-kms answers";
        if (errorCode) *errorCode = EINVAL;
        return Exchange::Failed;
    }
    if (::send(m_Socket, &request, sizeof request, MSG_NOSIGNAL) !=
        static_cast<ssize_t>(sizeof request)) {
        error = "moonlightweb-kms is gone (" + errnoName(errno) + ")";
        stop();
        return Exchange::Gone;
    }

    pollfd ready{m_Socket, POLLIN, 0};
    if (::poll(&ready, 1, kAnswerMs) != 1) {
        error = "moonlightweb-kms did not answer";
        stop();
        return Exchange::Failed;
    }
    mw_kms_reply reply{};
    iovec iov{&reply, sizeof reply};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * MW_KMS_MAX_PLANES)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    const ssize_t n = ::recvmsg(m_Socket, &msg, MSG_CMSG_CLOEXEC);

    // Whatever came, it is ours to close unless it is used.
    int fds[MW_KMS_MAX_PLANES] = {-1, -1, -1, -1};
    uint32_t received = 0;
    bool foreign = false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
            foreign = true;
            continue;
        }
        const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
            if (received < MW_KMS_MAX_PLANES)
                fds[received++] = fd;
            else {
                ::close(fd);
                foreign = true;
            }
        }
    }
    const auto drop = [&] {
        for (uint32_t i = 0; i < received; ++i)
            ::close(fds[i]);
    };
    if (n <= 0) {
        drop();
        error = n == 0 ? "moonlightweb-kms closed" : "moonlightweb-kms: " + errnoName(errno);
        stop();
        return Exchange::Gone;
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || foreign ||
        !mw_kms_reply_valid(&reply, static_cast<size_t>(n), request.seq, fbId, received)) {
        drop();
        error = "moonlightweb-kms answered something malformed";
        stop();
        return Exchange::Failed;
    }
    if (reply.status != 0) {
        drop();
        if (errorCode) *errorCode = reply.status;
        error = reply.status == EACCES
                    ? "moonlightweb-kms refused: this user's session is not the active one "
                      "on the card's seat"
                : reply.status == ENOENT ? "the framebuffer left the plane"
                : reply.status == EAGAIN
                    ? "the screen still shows what was on it before this session became "
                      "active; waiting for this session's own picture"
                : reply.status == EPERM
                    ? "moonlightweb-kms does not hold CAP_SYS_ADMIN: its file capability is "
                      "missing (the AppImage, or a package whose setup did not run)"
                    : "moonlightweb-kms: " + errnoName(reply.status);
        return Exchange::Failed;
    }

    out.width = reply.width;
    out.height = reply.height;
    out.fourcc = reply.pixel_format;
    out.modifier = reply.modifier;
    out.planeCount = static_cast<int>(reply.plane_count);
    out.fdCount = static_cast<int>(reply.fd_count);
    for (int i = 0; i < 4; ++i) {
        out.pitches[i] = reply.pitches[i];
        out.offsets[i] = reply.offsets[i];
        out.fdIndex[i] = reply.fd_index[i];
        out.fds[i] = fds[i];
    }
    return Exchange::Answered;
}

} // namespace mw::native::capture
