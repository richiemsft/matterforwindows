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
 *          Contains non-inline method definitions for the
 *          GenericPlatformManagerImpl_Windows<> template.
 */

#ifndef GENERIC_PLATFORM_MANAGER_IMPL_WINDOWS_CPP
#define GENERIC_PLATFORM_MANAGER_IMPL_WINDOWS_CPP

#include <platform/PlatformManager.h>
#include <platform/internal/GenericPlatformManagerImpl_Windows.h>

#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemLayer.h>
#include <system/SystemError.h>

#include <cerrno>
#include <process.h>

namespace chip {
namespace DeviceLayer {

// Provided by the focused Windows Device Layer globals translation unit
// (SystemLayerGlobals.cpp). Declared here to avoid pulling the full Device
// Layer manager closure through <platform/CHIPDeviceLayer.h>.
System::Layer & SystemLayer();

namespace Internal {

namespace {
System::LayerSelectLoop & SystemLayerSelectLoop()
{
    return static_cast<System::LayerSelectLoop &>(DeviceLayer::SystemLayer());
}
} // anonymous namespace

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_InitChipStack()
{
    // Arrange for CHIP core errors to be translated to text. The Device Layer
    // error formatter is registered together with the rest of GeneralUtils in a
    // later phase, since that translation unit reaches the full manager closure.
    RegisterCHIPLayerErrorFormatter();

    for (auto & handler : mAppEventHandlers)
    {
        handler.InUse = false;
    }

    mShouldRunEventLoop.store(true, std::memory_order_relaxed);
    mState.store(State::kStopped, std::memory_order_relaxed);

    // Initialize the native Windows System Layer (WSAPoll event loop, timers,
    // and cross-thread wake).
    ReturnErrorOnFailure(DeviceLayer::SystemLayer().Init());

    return CHIP_NO_ERROR;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_Shutdown()
{
    std::lock_guard<std::mutex> lifecycleLock(mLifecycleLock);
    HANDLE completedTask = nullptr;
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // The event loop must have fully stopped before shutdown. Reap an
        // internally managed loop that stopped itself from a work item.
        VerifyOrDie(mState.load(std::memory_order_relaxed) == State::kStopped);
        completedTask              = mChipTask;
        mChipTask                  = nullptr;
        mInternallyManagedChipTask = false;
    }
    if (completedTask != nullptr)
    {
        VerifyOrDie(WaitForSingleObject(completedTask, INFINITE) == WAIT_OBJECT_0);
        VerifyOrDie(CloseHandle(completedTask) != FALSE);
    }

    ChipLogProgress(DeviceLayer, "System Layer shutdown");
    DeviceLayer::SystemLayer().Shutdown();
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_AddEventHandler(PlatformManager::EventHandlerFunct handler, intptr_t arg)
{
    // Do nothing if the event handler is already registered.
    for (auto & entry : mAppEventHandlers)
    {
        if (entry.InUse && entry.Handler == handler && entry.Arg == arg)
        {
            return CHIP_NO_ERROR;
        }
    }

    for (auto & entry : mAppEventHandlers)
    {
        if (!entry.InUse)
        {
            entry.Handler = handler;
            entry.Arg     = arg;
            entry.InUse   = true;
            return CHIP_NO_ERROR;
        }
    }

    return CHIP_ERROR_NO_MEMORY;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_RemoveEventHandler(PlatformManager::EventHandlerFunct handler, intptr_t arg)
{
    for (auto & entry : mAppEventHandlers)
    {
        if (entry.InUse && entry.Handler == handler && entry.Arg == arg)
        {
            entry.InUse = false;
        }
    }
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_HandleServerStarted()
{
    PlatformManagerDelegate * platformManagerDelegate = PlatformMgr().GetDelegate();

    if (platformManagerDelegate != nullptr)
    {
        // The configuration manager that reports the operational software
        // version is not part of this foundation, so report the compile-time
        // configured version.
        platformManagerDelegate->OnStartUp(CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION);
    }
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_HandleServerShuttingDown()
{
    PlatformManagerDelegate * platformManagerDelegate = PlatformMgr().GetDelegate();

    if (platformManagerDelegate != nullptr)
    {
        platformManagerDelegate->OnShutDown();
    }
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_ScheduleWork(AsyncWorkFunct workFunct, intptr_t arg)
{
    ChipDeviceEvent event{};
    event.Type                    = DeviceEventType::kCallWorkFunct;
    event.CallWorkFunct.WorkFunct = workFunct;
    event.CallWorkFunct.Arg       = arg;
    CHIP_ERROR err                = Impl()->PostEvent(&event);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to schedule work: %" CHIP_ERROR_FORMAT, err.Format());
    }
    return err;
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_PostEvent(const ChipDeviceEvent * event)
{
    mChipEventQueue.Push(*event);
    SystemLayerSelectLoop().Signal(); // Wake the event loop on the CHIP thread.
    return CHIP_NO_ERROR;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_DispatchEvent(const ChipDeviceEvent * event)
{
    switch (event->Type)
    {
    case DeviceEventType::kNoOp:
        break;

    case DeviceEventType::kChipLambdaEvent:
        event->LambdaEvent();
        break;

    case DeviceEventType::kCallWorkFunct:
        event->CallWorkFunct.WorkFunct(event->CallWorkFunct.Arg);
        break;

    default:
        // Device Layer component dispatch (Connectivity, BLE, Thread) is added
        // when those managers are ported. Application handlers still receive
        // non-internal events so the public contract is preserved.
        if (!event->IsInternal())
        {
            for (auto & entry : mAppEventHandlers)
            {
                if (entry.InUse)
                {
                    entry.Handler(event, entry.Arg);
                }
            }
        }
        break;
    }
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::ProcessDeviceEvents()
{
    while (!mChipEventQueue.Empty())
    {
        const ChipDeviceEvent event = mChipEventQueue.PopFront();
        Impl()->DispatchEvent(&event);
    }
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_StartChipTimer(System::Clock::Timeout delay)
{
    // System::LayerSelectLoop::PrepareEvents() computes the next timer expiry
    // directly from the timer list, so no additional action is required here.
    return CHIP_NO_ERROR;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_RunEventLoop()
{
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // Record the thread actually servicing the loop so that a
        // StopEventLoopTask() call made from within a work item is recognized as
        // running on the CHIP thread and does not attempt to re-lock the stack.
        mEventLoopThreadId = GetCurrentThreadId();

        // If StartEventLoopTask() was not used, the caller is running the loop
        // on its own (externally managed) thread.
        if (!mInternallyManagedChipTask)
        {
            mState.store(State::kRunning, std::memory_order_relaxed);
            mShouldRunEventLoop.store(true, std::memory_order_relaxed);
        }
    }

    Impl()->LockChipStack();

    SystemLayerSelectLoop().EventLoopBegins();
    do
    {
        SystemLayerSelectLoop().PrepareEvents();

        Impl()->UnlockChipStack();
        SystemLayerSelectLoop().WaitForEvents();
        Impl()->LockChipStack();

        SystemLayerSelectLoop().HandleEvents();

        ProcessDeviceEvents();
    } while (mShouldRunEventLoop.load(std::memory_order_relaxed));
    SystemLayerSelectLoop().EventLoopEnds();

    Impl()->UnlockChipStack();

    {
        std::lock_guard<std::mutex> lock(mStateLock);
        mEventLoopThreadId = 0;
        mState.store(State::kStopped, std::memory_order_relaxed);
    }

    // Publish completion only after the loop has reached its final state.
    mEventQueueStoppedCond.notify_all();
}

template <class ImplClass>
unsigned __stdcall GenericPlatformManagerImpl_Windows<ImplClass>::EventLoopThread(void * context)
{
    auto * manager = static_cast<GenericPlatformManagerImpl_Windows<ImplClass> *>(context);
    ChipLogDetail(DeviceLayer, "CHIP task running");
    manager->Impl()->RunEventLoop();
    return 0;
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_StartEventLoopTask()
{
    std::lock_guard<std::mutex> lifecycleLock(mLifecycleLock);
    std::lock_guard<std::mutex> lock(mStateLock);

    VerifyOrReturnError(mState.load(std::memory_order_relaxed) == State::kStopped, CHIP_ERROR_INCORRECT_STATE);

    // A loop may stop itself from a work item. Reap that completed native
    // thread before replacing its handle with a newly started loop.
    if (mChipTask != nullptr)
    {
        VerifyOrReturnError(WaitForSingleObject(mChipTask, INFINITE) == WAIT_OBJECT_0, CHIP_ERROR_INTERNAL);
        VerifyOrReturnError(CloseHandle(mChipTask) != FALSE, CHIP_ERROR_WINDOWS(GetLastError()));
        mChipTask = nullptr;
    }

    mShouldRunEventLoop.store(true, std::memory_order_relaxed);
    mInternallyManagedChipTask = true;
    mState.store(State::kRunning, std::memory_order_relaxed);

    unsigned int threadId = 0;
    const uintptr_t threadHandle =
        _beginthreadex(nullptr, 0, &GenericPlatformManagerImpl_Windows<ImplClass>::EventLoopThread, this, 0, &threadId);
    if (threadHandle == 0)
    {
        mInternallyManagedChipTask = false;
        mState.store(State::kStopped, std::memory_order_relaxed);
        return CHIP_ERROR_POSIX(errno);
    }

    mChipTask          = reinterpret_cast<HANDLE>(threadHandle);
    mEventLoopThreadId = static_cast<DWORD>(threadId);
    return CHIP_NO_ERROR;
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_StopEventLoopTask()
{
    // When called from within the loop, only the stop request is needed; the
    // loop observes mShouldRunEventLoop and exits on its next iteration.
    {
        std::lock_guard<std::mutex> lock(mStateLock);
        if (GetCurrentThreadId() == mEventLoopThreadId)
        {
            mShouldRunEventLoop.store(false, std::memory_order_relaxed);
            return CHIP_NO_ERROR;
        }
    }

    const bool restoreChipStackLock = sChipStackLockedByCurrentThread;
    if (restoreChipStackLock)
    {
        // Release before lifecycle serialization: another stopping caller may
        // acquire the CHIP lock while waiting for mLifecycleLock, and neither
        // lock may be awaited while holding the other.
        Impl()->UnlockChipStack();
    }

    // Serialize the complete external stop (including its wait and handle
    // reclamation) against both restart and another stop.
    std::unique_lock<std::mutex> lifecycleLock(mLifecycleLock);
    std::unique_lock<std::mutex> lock(mStateLock);
    mShouldRunEventLoop.store(false, std::memory_order_relaxed);

    if (mState.load(std::memory_order_relaxed) != State::kStopped)
    {
        // Signal() only writes to the thread-safe wake socket. Taking the CHIP
        // stack lock here would deadlock a caller that already owns it.
        SystemLayerSelectLoop().Signal();
        mEventQueueStoppedCond.wait(lock, [this]() { return mState.load(std::memory_order_relaxed) == State::kStopped; });
    }

    // Claim the native handle while serialized so concurrent stop calls cannot
    // both wait on or close it.
    HANDLE completedTask = nullptr;
    if (mInternallyManagedChipTask)
    {
        completedTask              = mChipTask;
        mChipTask                  = nullptr;
        mInternallyManagedChipTask = false;
    }

    lock.unlock();
    CHIP_ERROR result = CHIP_NO_ERROR;
    if (completedTask != nullptr)
    {
        if (WaitForSingleObject(completedTask, INFINITE) != WAIT_OBJECT_0)
        {
            result = CHIP_ERROR_INTERNAL;
        }
        if (CloseHandle(completedTask) == FALSE && result == CHIP_NO_ERROR)
        {
            result = CHIP_ERROR_WINDOWS(GetLastError());
        }
    }

    lifecycleLock.unlock();
    if (restoreChipStackLock)
    {
        Impl()->LockChipStack();
    }

    return result;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_LockChipStack()
{
    mChipStackLock.lock();
    sChipStackLockedByCurrentThread = true;
}

template <class ImplClass>
bool GenericPlatformManagerImpl_Windows<ImplClass>::_TryLockChipStack()
{
    const bool locked = mChipStackLock.try_lock();
    if (locked)
    {
        sChipStackLockedByCurrentThread = true;
    }
    return locked;
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_UnlockChipStack()
{
    sChipStackLockedByCurrentThread = false;
    mChipStackLock.unlock();
}

#if CHIP_STACK_LOCK_TRACKING_ENABLED
template <class ImplClass>
bool GenericPlatformManagerImpl_Windows<ImplClass>::_IsChipStackLockedByCurrentThread() const
{
    // If no Matter thread is running, locking is not a concern.
    if (mState.load(std::memory_order_relaxed) == State::kStopped)
    {
        return true;
    }
    return sChipStackLockedByCurrentThread;
}
#endif

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_ScheduleBackgroundWork(AsyncWorkFunct workFunct, intptr_t arg)
{
    // No dedicated background processing in the foundation: run on the CHIP loop.
    return Impl()->ScheduleWork(workFunct, arg);
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_PostBackgroundEvent(const ChipDeviceEvent * event)
{
    return Impl()->PostEvent(event);
}

template <class ImplClass>
void GenericPlatformManagerImpl_Windows<ImplClass>::_RunBackgroundEventLoop()
{
    // No dedicated background event loop in the foundation.
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_StartBackgroundEventLoopTask()
{
    return CHIP_NO_ERROR;
}

template <class ImplClass>
CHIP_ERROR GenericPlatformManagerImpl_Windows<ImplClass>::_StopBackgroundEventLoopTask()
{
    return CHIP_NO_ERROR;
}

// Fully instantiate the generic implementation class in whatever compilation
// unit includes this file. NB: This must come after all templated members.
template class GenericPlatformManagerImpl_Windows<PlatformManagerImpl>;

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // GENERIC_PLATFORM_MANAGER_IMPL_WINDOWS_CPP
