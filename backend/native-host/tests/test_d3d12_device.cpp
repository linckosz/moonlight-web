/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "platform/windows/StreamPriority.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <string>

using namespace mw::native;
using Microsoft::WRL::ComPtr;
#endif

// The D3D12 foundation of the second pipeline (plan pipeline-video-d3d12-v2,
// C2.1-C2.3), on WARP so it runs on a machine with no GPU: one device per
// adapter, queues that step their priority down instead of failing, fences
// that round-trip and a CPU wait that gives up, and timestamps that read back.
void run_d3d12_device_tests()
{
#if defined(_WIN32)
    SECTION("StreamPriority — the guests' feed stands one step below the owner");
    {
        using GpuClass = StreamPriority::GpuClass;
        using Role = StreamPriority::Role;
        // The class it asks: one below what the owner gets with the same token.
        CHECK(StreamPriority::feedClassToAsk(true) == GpuClass::High);
        CHECK(StreamPriority::feedClassToAsk(false) == GpuClass::AboveNormal);
        // Its queues: never above HIGH, and NORMAL unless its class is HIGH.
        CHECK(StreamPriority::queuePriorityFor(Role::Feed, GpuClass::High) ==
              D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        CHECK(StreamPriority::queuePriorityFor(Role::Feed, GpuClass::AboveNormal) ==
              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        CHECK(StreamPriority::queuePriorityFor(Role::Feed, GpuClass::Normal) ==
              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        CHECK(StreamPriority::queuePriorityFor(Role::Feed, GpuClass::LeftAlone) ==
              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        CHECK(StreamPriority::queuePriorityFor(Role::Feed, GpuClass::Unknown) ==
              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        // The owner's, as before.
        CHECK(StreamPriority::queuePriorityFor(Role::Owner, GpuClass::Realtime) ==
              D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME);
        CHECK(StreamPriority::queuePriorityFor(Role::Owner, GpuClass::High) ==
              D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        CHECK(StreamPriority::queuePriorityFor(Role::Owner, GpuClass::Unknown) ==
              D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        CHECK(StreamPriority::queuePriorityFor(Role::Owner, GpuClass::LeftAlone) ==
              D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        // Its devices are not raised; the owner's go to the top.
        CHECK_EQ(StreamPriority::deviceThreadPriorityFor(Role::Feed), 0);
        CHECK_EQ(StreamPriority::deviceThreadPriorityFor(Role::Owner), 7);

        // A process is an owner's unless named the feed.
        CHECK(StreamPriority::role() == Role::Owner);
        CHECK(StreamPriority::takesMmcss());
        // Named the feed: it is one (when its priorities were not taken yet in
        // this runner), and its threads take no MMCSS.
        StreamPriority::setRole(Role::Feed);
        CHECK(StreamPriority::takesMmcss() == (StreamPriority::role() != Role::Feed));
        if (StreamPriority::role() == Role::Feed)
            CHECK(StreamPriority::queuePriority() ==
                  StreamPriority::queuePriorityFor(Role::Feed, StreamPriority::grantedClass()));
        StreamPriority::setRole(Role::Owner);
        CHECK(StreamPriority::role() == Role::Owner);
        // MW_FEED_PRIORITY=same: the feed keeps the owner's standing.
        ::SetEnvironmentVariableA("MW_FEED_PRIORITY", "same");
        StreamPriority::setRole(Role::Feed);
        CHECK(StreamPriority::role() == Role::Owner);
        CHECK(StreamPriority::takesMmcss());
        ::SetEnvironmentVariableA("MW_FEED_PRIORITY", nullptr);
    }

    SECTION("D3D12 device — queues, fences and timers, on WARP");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> warp;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
        std::fprintf(stderr, "  no WARP adapter here — skipped\n");
        return;
    }

    std::string error;
    std::shared_ptr<d3d12::D3d12Device> device = d3d12::D3d12Device::forAdapter(warp.Get(), error);
    if (!device) {
        std::fprintf(stderr, "  no D3D12 on WARP (%s) — skipped\n", error.c_str());
        return;
    }
    CHECK(device->device() != nullptr);
    CHECK(!device->name().empty());
    // WARP works in the CPU's memory, like an iGPU: never pipelined by default.
    CHECK(device->unifiedMemory());
    CHECK_EQ(device->vendorId(), 0x1414u);

    // One device per adapter: by the adapter and by its LUID alike.
    {
        std::string again;
        CHECK(d3d12::D3d12Device::forAdapter(warp.Get(), again) == device);
        CHECK(d3d12::D3d12Device::forAdapter(device->luid(), again) == device);
        CHECK(d3d12::D3d12Device::forAdapter(0x7fffffff00000000ull, again) == nullptr);
        CHECK(again.find("LUID") != std::string::npos);
    }

    // Before any session asked for a class, a queue asks HIGH — nothing a
    // limited token can be refused.
    if (StreamPriority::grantedClass() == StreamPriority::GpuClass::Unknown)
        CHECK(StreamPriority::queuePriority() == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);

    d3d12::Queue direct;
    {
        d3d12::QueueRequest request;
        request.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        request.priority = d3d12::QueuePriority::High;
        request.name = L"test direct";
        CHECK(device->createQueue(request, direct, error));
        CHECK(direct.queue != nullptr);
        CHECK(direct.priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
        CHECK(direct.description.find("DIRECT queue HIGH") == 0);
        std::fprintf(stderr, "  %s\n", direct.description.c_str());
    }

    // GLOBAL_REALTIME takes the base-priority privilege: a limited token is
    // refused and steps down to HIGH with the refusal said; an elevated one
    // may keep it. Either way there is a queue.
    {
        d3d12::QueueRequest request;
        request.type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        request.priority = d3d12::QueuePriority::GlobalRealtime;
        d3d12::Queue queue;
        CHECK(device->createQueue(request, queue, error));
        CHECK(queue.queue != nullptr);
        if (queue.priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME) {
            CHECK(queue.description.find("refused") == std::string::npos);
        } else {
            CHECK(queue.priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH);
            CHECK(queue.description.find("GLOBAL_REALTIME refused") != std::string::npos);
        }
        std::fprintf(stderr, "  %s\n", queue.description.c_str());
    }

    // Without a CreatorID of its own, the queue says so.
    {
        d3d12::QueueRequest request;
        request.type = D3D12_COMMAND_LIST_TYPE_COPY;
        request.priority = d3d12::QueuePriority::Normal;
        request.ownCreator = false;
        d3d12::Queue queue;
        CHECK(device->createQueue(request, queue, error));
        CHECK(!queue.ownCreator);
        CHECK(queue.description.find("default CreatorID") != std::string::npos);
    }

    // A fence signalled by an empty queue is reached; one never signalled
    // times out in about the time asked, and says where it stands.
    {
        d3d12::GpuFence fence;
        CHECK(fence.create(device->device(), false, error));
        const uint64_t value = fence.signal(direct.queue.Get(), error);
        CHECK(value == 1);
        CHECK(fence.wait(value, 1000, error) == d3d12::GpuFence::Wait::Done);
        CHECK(fence.completed() >= value);

        const auto started = std::chrono::steady_clock::now();
        std::string late;
        CHECK(fence.wait(value + 5, 50, late) == d3d12::GpuFence::Wait::TimedOut);
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        CHECK(waited >= 40 && waited < 1000);
        CHECK(late.find("did not reach fence value 6") != std::string::npos);
        CHECK(late.find("device alive") != std::string::npos);

        // A plain fence has no handle to give.
        std::string why;
        CHECK(fence.sharedHandle(why) == nullptr);
        CHECK(!why.empty());
    }

    // A GPU wait holds the queue until the CPU lets it go: the signal behind
    // it is not reached before.
    {
        d3d12::GpuFence gate;
        d3d12::GpuFence done;
        CHECK(gate.create(device->device(), false, error));
        CHECK(done.create(device->device(), true, error));
        CHECK(gate.gpuWait(direct.queue.Get(), 1, error));
        const uint64_t after = done.signal(direct.queue.Get(), error);
        std::string held;
        CHECK(done.wait(after, 30, held) == d3d12::GpuFence::Wait::TimedOut);
        CHECK(SUCCEEDED(gate.fence()->Signal(1)));
        CHECK(done.wait(after, 1000, error) == d3d12::GpuFence::Wait::Done);

        // A shared fence hands out an NT handle.
        HANDLE handle = done.sharedHandle(error);
        CHECK(handle != nullptr);
        if (handle) ::CloseHandle(handle);
    }

    // Timestamps around a stretch of a direct list read back once its fence
    // has passed; a slot is not read twice.
    {
        d3d12::QueueTimer timer;
        if (!timer.init(device->device(), direct.queue.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                        error)) {
            std::fprintf(stderr, "  timestamps skipped: %s\n", error.c_str());
        } else {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            CHECK(SUCCEEDED(device->device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     IID_PPV_ARGS(&allocator))));
            CHECK(SUCCEEDED(device->device()->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
            if (list) {
                const int slot = timer.begin(list.Get());
                CHECK(slot >= 0);
                timer.end(list.Get(), slot);
                CHECK(SUCCEEDED(list->Close()));
                ID3D12CommandList* lists[] = {list.Get()};
                direct.queue->ExecuteCommandLists(1, lists);
                d3d12::GpuFence fence;
                CHECK(fence.create(device->device(), false, error));
                const uint64_t value = fence.signal(direct.queue.Get(), error);
                CHECK(fence.wait(value, 1000, error) == d3d12::GpuFence::Wait::Done);
                d3d12::QueueTimer::Sample sample;
                CHECK(timer.read(slot, sample));
                CHECK(sample.gpuUs >= 0 && sample.gpuUs < 1000000);
                if (sample.startQpc != 0) CHECK(sample.endQpc >= sample.startQpc);
                CHECK(!timer.read(slot, sample));
            }
        }
    }

    // A video-encode queue is not timed here (its list cannot resolve into a
    // readback buffer): refused with a reason, not half-made.
    {
        d3d12::QueueTimer timer;
        std::string why;
        CHECK(!timer.init(device->device(), direct.queue.Get(),
                          D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, why));
        CHECK(!timer.ready());
        CHECK(why.find("VIDEO_ENCODE") != std::string::npos);
    }

    std::string gone;
    CHECK(!device->removed(gone));
#endif
}
