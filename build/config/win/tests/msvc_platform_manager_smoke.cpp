/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

// Runtime smoke for the Phase 3 native Windows Device Layer foundation. It
// proves the PlatformManager lifecycle (init / event-loop task / run-loop /
// stop / shutdown) and cross-thread work posting composed on top of the native
// System::LayerImplWindows event loop. Returns 0 on success, 1 on any failure.

#include <platform/PlatformManager.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace chip;
using namespace chip::DeviceLayer;
using namespace std::chrono_literals;

namespace {

struct Signal
{
    std::mutex mutex;
    std::condition_variable cond;
    bool fired = false;

    void Fire()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            fired = true;
        }
        cond.notify_all();
    }

    bool Wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cond.wait_for(lock, 5s, [this]() { return fired; });
    }
};

std::atomic<unsigned int> gWorkRuns{ 0 };
std::atomic<unsigned int> gHandlerRuns{ 0 };

void MarkSignal(intptr_t arg)
{
    ++gWorkRuns;
    reinterpret_cast<Signal *>(arg)->Fire();
}

void StopLoopWork(intptr_t arg)
{
    ++gWorkRuns;
    reinterpret_cast<Signal *>(arg)->Fire();
    (void) PlatformMgr().StopEventLoopTask();
}

void EventHandler(const ChipDeviceEvent * event, intptr_t arg)
{
    if (event->Type == DeviceEventType::kServerReady)
    {
        ++gHandlerRuns;
        reinterpret_cast<Signal *>(arg)->Fire();
    }
}

int StopAndShutdown()
{
    (void) PlatformMgr().StopEventLoopTask();
    PlatformMgr().Shutdown();
    return 1;
}

} // namespace

int main()
{
    // ---- Scenario 1: StartEventLoopTask + cross-thread work and event dispatch.
    if (PlatformMgr().InitChipStack() != CHIP_NO_ERROR)
    {
        return 1;
    }

    Signal handlerSignal;
    if (PlatformMgr().AddEventHandler(EventHandler, reinterpret_cast<intptr_t>(&handlerSignal)) != CHIP_NO_ERROR)
    {
        return StopAndShutdown();
    }

    if (PlatformMgr().StartEventLoopTask() != CHIP_NO_ERROR)
    {
        return StopAndShutdown();
    }
    if (PlatformMgr().StartEventLoopTask() != CHIP_ERROR_INCORRECT_STATE)
    {
        return StopAndShutdown();
    }

    // Post work from this (non-CHIP) thread and confirm it executes on the loop.
    Signal workSignal;
    if (PlatformMgr().ScheduleWork(MarkSignal, reinterpret_cast<intptr_t>(&workSignal)) != CHIP_NO_ERROR)
    {
        return StopAndShutdown();
    }
    if (!workSignal.Wait() || gWorkRuns.load() != 1)
    {
        return StopAndShutdown();
    }

    // Post work from a separate thread to exercise cross-thread posting.
    Signal crossThreadSignal;
    std::thread poster([&crossThreadSignal]() {
        (void) PlatformMgr().ScheduleWork(MarkSignal, reinterpret_cast<intptr_t>(&crossThreadSignal));
    });
    poster.join();
    if (!crossThreadSignal.Wait() || gWorkRuns.load() != 2)
    {
        return StopAndShutdown();
    }

    // Post a public device event and confirm it reaches the registered handler.
    ChipDeviceEvent readyEvent{};
    readyEvent.Type = DeviceEventType::kServerReady;
    if (PlatformMgr().PostEvent(&readyEvent) != CHIP_NO_ERROR)
    {
        return StopAndShutdown();
    }
    if (!handlerSignal.Wait() || gHandlerRuns.load() != 1)
    {
        return StopAndShutdown();
    }

    if (PlatformMgr().StopEventLoopTask() != CHIP_NO_ERROR)
    {
        PlatformMgr().Shutdown();
        return 1;
    }
    PlatformMgr().RemoveEventHandler(EventHandler, reinterpret_cast<intptr_t>(&handlerSignal));
    PlatformMgr().Shutdown();

    // ---- Scenario 2: stop an internally managed loop while holding the CHIP
    // stack lock. StopEventLoopTask must release and restore it while waiting.
    if (PlatformMgr().InitChipStack() != CHIP_NO_ERROR || PlatformMgr().StartEventLoopTask() != CHIP_NO_ERROR)
    {
        return 1;
    }
    PlatformMgr().LockChipStack();
    if (PlatformMgr().StopEventLoopTask() != CHIP_NO_ERROR)
    {
        PlatformMgr().UnlockChipStack();
        PlatformMgr().Shutdown();
        return 1;
    }
    PlatformMgr().UnlockChipStack();
    PlatformMgr().Shutdown();

    // ---- Scenario 3: RunEventLoop on a caller-managed thread, stopped by work.
    if (PlatformMgr().InitChipStack() != CHIP_NO_ERROR)
    {
        return 1;
    }

    std::thread loopThread([]() { PlatformMgr().RunEventLoop(); });

    Signal stopSignal;
    if (PlatformMgr().ScheduleWork(StopLoopWork, reinterpret_cast<intptr_t>(&stopSignal)) != CHIP_NO_ERROR)
    {
        (void) PlatformMgr().StopEventLoopTask();
        loopThread.join();
        PlatformMgr().Shutdown();
        return 1;
    }
    if (!stopSignal.Wait())
    {
        (void) PlatformMgr().StopEventLoopTask();
        loopThread.join();
        PlatformMgr().Shutdown();
        return 1;
    }

    // RunEventLoop must return once StopEventLoopTask has been requested.
    loopThread.join();
    if (gWorkRuns.load() != 3)
    {
        PlatformMgr().Shutdown();
        return 1;
    }

    PlatformMgr().Shutdown();
    return 0;
}
