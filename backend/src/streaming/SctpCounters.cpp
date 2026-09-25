/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "SctpCounters.h"

#include <cstring>

#include <usrsctp.h>

namespace mw::sctp {

std::array<uint32_t, 4> readCounters()
{
    struct sctpstat st;
    std::memset(&st, 0, sizeof st);
    usrsctp_get_stat(&st);
    return {st.sctps_senddata, st.sctps_sendretransdata, st.sctps_sendfastretrans,
            st.sctps_timodata};
}

} // namespace mw::sctp
