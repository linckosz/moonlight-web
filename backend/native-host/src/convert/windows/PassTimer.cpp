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

#include "PassTimer.h"

namespace mw::native::convert {

int PassTimer::begin(ID3D11Device* device, ID3D11DeviceContext* context)
{
    collect(context);
    size_t inFlight = 0;
    for (const Slot& slot : m_Slots)
        if (slot.pending) ++inFlight;
    if (!m_Cost.timeThisFrame(inFlight)) return -1;

    const int index = m_Next;
    Slot& slot = m_Slots[index];
    if (slot.pending) return -1; // the GPU is further behind than the ring: skip this one
    if (!slot.disjoint) {
        D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(device->CreateQuery(&desc, slot.disjoint.GetAddressOf()))) return -1;
        desc.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(device->CreateQuery(&desc, slot.begin.GetAddressOf())) ||
            FAILED(device->CreateQuery(&desc, slot.end.GetAddressOf()))) {
            slot = Slot{};
            return -1;
        }
    }
    context->Begin(slot.disjoint.Get());
    context->End(slot.begin.Get());
    return index;
}

void PassTimer::end(ID3D11DeviceContext* context, int index)
{
    if (index < 0) return;
    Slot& slot = m_Slots[index];
    context->End(slot.end.Get());
    context->End(slot.disjoint.Get());
    slot.pending = true;
    m_Next = (index + 1) % kSlots;
}

void PassTimer::collect(ID3D11DeviceContext* context)
{
    for (Slot& slot : m_Slots) {
        if (!slot.pending) continue;
        // DONOTFLUSH: a result not there yet is read at a later frame, never
        // waited for — waiting here would be the very latency being measured.
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        if (context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        UINT64 begin = 0, end = 0;
        if (context->GetData(slot.begin.Get(), &begin, sizeof(begin),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) !=
                S_OK)
            continue;
        slot.pending = false;
        // A disjoint interval (the GPU changed clock mid-way, or was reset)
        // says nothing reliable: dropped, the next frame is timed instead.
        if (!disjoint.Disjoint && disjoint.Frequency > 0 && end > begin)
            m_Cost.add(static_cast<int64_t>((end - begin) * 1000000 / disjoint.Frequency));
    }
}

bool PassTimer::take(int64_t& costUs)
{
    if (m_Taken || !m_Cost.done()) return false;
    m_Taken = true;
    costUs = m_Cost.medianUs();
    release();
    return true;
}

void PassTimer::reset()
{
    release();
    m_Cost.reset();
    m_Taken = false;
}

void PassTimer::release()
{
    for (Slot& slot : m_Slots)
        slot = Slot{};
    m_Next = 0;
}

} // namespace mw::native::convert
