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

/**
 *    @file
 *          Provides a generic implementation of PlatformManager features for
 *          native Windows hosts.
 *
 *          This is the Phase 3 Device Layer foundation. It composes the native
 *          System::LayerImplWindows event loop with the standard PlatformManager
 *          lifecycle, cross-thread work posting, and shutdown contracts using
 *          the C++ standard threading library (std::thread / std::mutex /
 *          std::condition_variable) rather than POSIX pthreads.
 *
 *          The event dispatch surface intentionally handles only the platform
 *          neutral event kinds (no-op, lambda, and scheduled work) plus
 *          application event handlers. Dispatch to Device Layer component
 *          managers (Connectivity, BLE, Thread) is deliberately absent because
 *          those managers are not part of this milestone; wiring them in is a
 *          later phase and must not be faked here.
 */

#pragma once

#include <platform/DeviceSafeQueue.h>
#include <platform/PlatformManager.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <windows.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

/**
 * Provides a generic implementation of PlatformManager features for native
 * Windows hosts. It is intended to be inherited by the Windows
 * PlatformManagerImpl, which also appears as the template's ImplClass parameter.
 */
template <class ImplClass>
class GenericPlatformManagerImpl_Windows
{
protected:
    // The maximum number of concurrently registered application event handlers.
    // A fixed pool avoids heap use on the event registration path.
    static constexpr size_t kMaxAppEventHandlers = 8;

    enum class State
    {
        kStopped  = 0,
        kRunning  = 1,
        kStopping = 2,
    };

    struct AppEventHandler
    {
        PlatformManager::EventHandlerFunct Handler;
        intptr_t Arg;
        bool InUse;
    };

    // ===== Methods that implement the PlatformManager abstract interface.

    CHIP_ERROR _InitChipStack();
    void _Shutdown();

    CHIP_ERROR _AddEventHandler(PlatformManager::EventHandlerFunct handler, intptr_t arg);
    void _RemoveEventHandler(PlatformManager::EventHandlerFunct handler, intptr_t arg);
    void _HandleServerStarted();
    void _HandleServerShuttingDown();

    CHIP_ERROR _ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg);
    CHIP_ERROR _PostEvent(const ChipDeviceEvent * event);
    void _DispatchEvent(const ChipDeviceEvent * event);

    void _RunEventLoop();
    CHIP_ERROR _StartEventLoopTask();
    CHIP_ERROR _StopEventLoopTask();
    CHIP_ERROR _StartChipTimer(System::Clock::Timeout duration);

    void _LockChipStack();
    bool _TryLockChipStack();
    void _UnlockChipStack();

#if CHIP_STACK_LOCK_TRACKING_ENABLED
    bool _IsChipStackLockedByCurrentThread() const;
#endif

    // Background event processing is not supported by the foundation and
    // delegates to the foreground queue where a fallback is required.
    CHIP_ERROR _ScheduleBackgroundWork(AsyncWorkFunct workFunct, intptr_t arg);
    CHIP_ERROR _PostBackgroundEvent(const ChipDeviceEvent * event);
    void _RunBackgroundEventLoop();
    CHIP_ERROR _StartBackgroundEventLoopTask();
    CHIP_ERROR _StopBackgroundEventLoopTask();

private:
    inline ImplClass * Impl() { return static_cast<ImplClass *>(this); }

    static unsigned __stdcall EventLoopThread(void * context);
    void ProcessDeviceEvents();

    DeviceSafeQueue mChipEventQueue;
    AppEventHandler mAppEventHandlers[kMaxAppEventHandlers] = {};

    std::mutex mChipStackLock;
    std::mutex mLifecycleLock;
    std::mutex mStateLock;
    std::condition_variable mEventQueueStoppedCond;

    HANDLE mChipTask                   = nullptr;
    bool mInternallyManagedChipTask = false;
    DWORD mEventLoopThreadId           = 0;
    std::atomic<State> mState{ State::kStopped };
    std::atomic<bool> mShouldRunEventLoop{ true };

    inline static thread_local bool sChipStackLockedByCurrentThread = false;
};

// Instruct the compiler to instantiate the template only when explicitly told to do so.
extern template class GenericPlatformManagerImpl_Windows<PlatformManagerImpl>;

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
