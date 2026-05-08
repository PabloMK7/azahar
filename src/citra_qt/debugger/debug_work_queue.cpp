// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include "citra_qt/debugger/debug_work_queue.h"
#include "core/core.h"

DebugWorkQueue::DebugWorkQueue(Core::System& system_) : system{system_} {
    work_id = system.RegisterWork(Core::WorkPoint::FrameEnd | Core::WorkPoint::FrameLimiterWait |
                                      Core::WorkPoint::PicaBreakpoint,
                                  [this] { Process(); });
}

DebugWorkQueue::~DebugWorkQueue() {
    system.UnregisterWork(work_id);
}

void DebugWorkQueue::Queue(std::function<void()> work) {
    {
        std::scoped_lock lock{mutex};
        queue.push_back(std::move(work));
        has_work = true;
    }

    system.NotifyPendingWork();
}

void DebugWorkQueue::Clear() {
    std::scoped_lock lock{mutex};
    queue.clear();
    has_work = false;
}

void DebugWorkQueue::Process() {
    if (!has_work.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::vector<std::function<void()>> work;
    {
        std::scoped_lock lock{mutex};
        work.swap(queue);
        has_work = false;
    }
    for (const auto& function : work) {
        function();
    }
}
