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

#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace mw::native::capture {

/// A framebuffer's buffers, as GETFB2 describes them and as DMA-BUF descriptors
/// this process owns: one per DISTINCT buffer object (a DCC surface's planes
/// share one), named by each plane's fdIndex. close() gives them back.
struct KmsBuffer
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0; ///< 0 (linear) when the framebuffer has none
    int planeCount = 0;
    uint32_t pitches[4] = {};
    uint32_t offsets[4] = {};
    int fdIndex[4] = {-1, -1, -1, -1};
    int fds[4] = {-1, -1, -1, -1};
    int fdCount = 0;

    /// The descriptor plane @p i lives in.
    int fdOf(int i) const { return i < planeCount ? fds[fdIndex[i]] : -1; }
    void close();
};

/// moonlightweb-kms, from the stream worker's side.
///
/// The kernel hands a framebuffer's buffers only to a process that holds
/// CAP_SYS_ADMIN. The package gives that capability to one small program, next
/// to this binary, and to nothing else; this asks it — one SOCK_SEQPACKET
/// socketpair, the helper's end on its descriptor 3, one request at a time
/// (KmsHelperProtocol.h). The helper answers this user only while their session
/// is the active one on the card's seat, and only for what is on the screen.
///
/// Started on the first request and kept for the capture's life; it exits when
/// this object closes its end, and dies with the thread that started it — then
/// read() starts another, once, rather than lose the frame.
class KmsHelperClient
{
public:
    KmsHelperClient() = default;
    ~KmsHelperClient();
    KmsHelperClient(const KmsHelperClient&) = delete;
    KmsHelperClient& operator=(const KmsHelperClient&) = delete;

    /// The helper next to this executable, when there is one this process can
    /// run; empty otherwise (a tree run by hand, the AppImage).
    static std::string helperPath();

    /// Whether framebuffers are read through the helper: this process does not
    /// hold CAP_SYS_ADMIN itself, and the helper is there.
    static bool wanted();

    /// The buffers of @p fbId, on @p planeId of @p crtcId of /dev/dri/card<card>.
    /// False with @p error on any refusal (not the active user: EACCES; not on
    /// that plane any more: ENOENT) — @p errorCode carries the errno.
    bool read(uint32_t card, uint32_t crtcId, uint32_t planeId, uint32_t fbId, KmsBuffer& out,
              std::string& error, int* errorCode = nullptr);

    /// The card index of a "/dev/dri/cardN" path; -1 when it is not one.
    static int cardIndex(const std::string& cardPath);

private:
    /// One request and its answer. Gone: the helper was not there to answer
    /// (it is stopped, and read() starts another once).
    enum class Exchange
    {
        Answered,
        Failed,
        Gone
    };
    Exchange exchange(uint32_t card, uint32_t crtcId, uint32_t planeId, uint32_t fbId,
                      KmsBuffer& out, std::string& error, int* errorCode);
    bool start(std::string& error);
    void stop();

    int m_Socket = -1;
    pid_t m_Pid = -1;
    uint32_t m_Seq = 0;
};

} // namespace mw::native::capture
