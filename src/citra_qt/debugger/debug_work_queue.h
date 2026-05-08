// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

/**
 * Work of debugging tools that inspect state owned by the emulation thread, such as the
 * rasterizer cache. It runs on the emulation thread at the end of the next frame, or right away
 * if the frame limiter is waiting (such as while paused with frame advancing) or a Pica
 * breakpoint is hit. Constructed and destroyed while the emulation is not running, as it
 * registers its work with the system.
 */
class DebugWorkQueue {
public:
    explicit DebugWorkQueue(Core::System& system);
    ~DebugWorkQueue();

    /// Queues work for the emulation thread. This function is thread safe.
    void Queue(std::function<void()> work);

    /// Drops the work that has not run yet. Called once the emulation thread has stopped.
    void Clear();

private:
    void Process();

    Core::System& system;
    u32 work_id;
    std::mutex mutex;
    std::vector<std::function<void()>> queue;
    std::atomic_bool has_work{};
};
