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

#include <system/windows/SystemLayerImplWindows.h>

#include <lib/support/CodeUtils.h>
#include <platform/LockTracker.h>
#include <system/SystemError.h>
#include <system/SystemFaultInjection.h>

#include <algorithm>
#include <limits>

namespace chip {
namespace System {

namespace {

constexpr Clock::Seconds64 kDefaultMinSleepPeriod = Clock::Seconds64(60 * 60 * 24 * 30);

enum : intptr_t
{
    kLoopHandlerInactive = 0,
    kLoopHandlerPending,
    kLoopHandlerActive,
};

} // namespace

LayerImplWindows::~LayerImplWindows()
{
    VerifyOrDie(mLayerState.Destroy());
}

CriticalFailure LayerImplWindows::Init()
{
    VerifyOrReturnError(mLayerState.GetState() == ObjectLifeCycle::State::Uninitialized, CHIP_ERROR_INCORRECT_STATE);

    RegisterWindowsErrorFormatter();
    for (auto & watch : mSocketWatchPool)
    {
        watch.Clear();
    }
    CHIP_ERROR error = mWakeEvent.Open();
    if (error != CHIP_NO_ERROR)
    {
        return error;
    }
    if (!mLayerState.SetInitializing())
    {
        mWakeEvent.Close();
        return CHIP_ERROR_INCORRECT_STATE;
    }

    VerifyOrReturnError(mLayerState.SetInitialized(), CHIP_ERROR_INCORRECT_STATE);
    return CHIP_NO_ERROR;
}

void LayerImplWindows::Shutdown()
{
    VerifyOrReturn(mLayerState.SetShuttingDown());

    for (auto & handler : mLoopHandlers)
    {
        LoopHandlerState(handler) = kLoopHandlerInactive;
    }
    mLoopHandlers.Clear();
    mTimerList.Clear();
    mTimerPool.ReleaseAll();
    for (auto & watch : mSocketWatchPool)
    {
        watch.Clear();
    }
    mWakeEvent.Close();
    mLayerState.ResetFromShuttingDown();
}

void LayerImplWindows::Signal()
{
    const CHIP_ERROR error = mWakeEvent.Notify();
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(chipSystemLayer, "Windows event-loop wake failed: %" CHIP_ERROR_FORMAT, error.Format());
    }
}

CriticalFailure LayerImplWindows::StartTimer(Clock::Timeout delay, TimerCompleteCallback onComplete, void * appState)
{
    assertChipStackLockedByCurrentThread();
    VerifyOrReturnError(mLayerState.IsInitialized(), CHIP_ERROR_INCORRECT_STATE);

    CHIP_SYSTEM_FAULT_INJECT(FaultInjection::kFault_TimeoutImmediate, delay = Clock::kZero);
    CancelTimer(onComplete, appState);

    TimerList::Node * timer = mTimerPool.Create(*this, SystemClock().GetMonotonicTimestamp() + delay, onComplete, appState);
    VerifyOrReturnError(timer != nullptr, CHIP_ERROR_NO_MEMORY);
    if (mTimerList.Add(timer) == timer)
    {
        Signal();
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::ExtendTimerTo(Clock::Timeout delay, TimerCompleteCallback onComplete, void * appState)
{
    VerifyOrReturnError(delay > Clock::kZero, CHIP_ERROR_INVALID_ARGUMENT);
    assertChipStackLockedByCurrentThread();

    if (mTimerList.GetRemainingTime(onComplete, appState) < delay)
    {
        return StartTimer(delay, onComplete, appState);
    }
    return CHIP_NO_ERROR;
}

bool LayerImplWindows::IsTimerActive(TimerCompleteCallback onComplete, void * appState)
{
    if (mTimerList.GetRemainingTime(onComplete, appState) > Clock::kZero)
    {
        return true;
    }

    for (TimerList::Node * timer = mExpiredTimers.Earliest(); timer != nullptr; timer = timer->mNextTimer)
    {
        if (timer->GetCallback().GetOnComplete() == onComplete && timer->GetCallback().GetAppState() == appState)
        {
            return true;
        }
    }
    return false;
}

Clock::Timeout LayerImplWindows::GetRemainingTime(TimerCompleteCallback onComplete, void * appState)
{
    return mTimerList.GetRemainingTime(onComplete, appState);
}

void LayerImplWindows::CancelTimer(TimerCompleteCallback onComplete, void * appState)
{
    assertChipStackLockedByCurrentThread();
    VerifyOrReturn(mLayerState.IsInitialized());

    TimerList::Node * timer = mTimerList.Remove(onComplete, appState);
    if (timer == nullptr)
    {
        timer = mExpiredTimers.Remove(onComplete, appState);
    }
    VerifyOrReturn(timer != nullptr);

    mTimerPool.Release(timer);
    Signal();
}

CriticalFailure LayerImplWindows::ScheduleWork(TimerCompleteCallback onComplete, void * appState)
{
    assertChipStackLockedByCurrentThread();
    VerifyOrReturnError(mLayerState.IsInitialized(), CHIP_ERROR_INCORRECT_STATE);

    TimerList::Node * timer = mTimerPool.Create(*this, SystemClock().GetMonotonicTimestamp(), onComplete, appState);
    VerifyOrReturnError(timer != nullptr, CHIP_ERROR_NO_MEMORY);
    if (mTimerList.Add(timer) == timer)
    {
        Signal();
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::StartWatchingSocket(SocketHandle socket, SocketWatchToken * tokenOut)
{
    VerifyOrReturnError(socket != kInvalidSocketHandle && tokenOut != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    SocketWatch * available = nullptr;
    for (auto & watch : mSocketWatchPool)
    {
        if (watch.mSocket == socket)
        {
            *tokenOut = reinterpret_cast<SocketWatchToken>(&watch);
            return CHIP_NO_ERROR;
        }
        if (watch.mSocket == kInvalidSocketHandle && available == nullptr)
        {
            available = &watch;
        }
    }
    VerifyOrReturnError(available != nullptr, CHIP_ERROR_ENDPOINT_POOL_FULL);

    available->mSocket = socket;
    *tokenOut          = reinterpret_cast<SocketWatchToken>(available);
    Signal();
    return CHIP_NO_ERROR;
}

LayerImplWindows::SocketWatch * LayerImplWindows::GetWatch(SocketWatchToken token)
{
    SocketWatch * watch = reinterpret_cast<SocketWatch *>(token);
    for (auto & registeredWatch : mSocketWatchPool)
    {
        if (watch == &registeredWatch)
        {
            return registeredWatch.mSocket == kInvalidSocketHandle ? nullptr : watch;
        }
    }
    return nullptr;
}

CHIP_ERROR LayerImplWindows::SetCallback(SocketWatchToken token, SocketWatchCallback callback, intptr_t data)
{
    SocketWatch * watch = GetWatch(token);
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->mCallback     = callback;
    watch->mCallbackData = data;
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::RequestCallbackOnPendingRead(SocketWatchToken token)
{
    SocketWatch * watch = GetWatch(token);
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->mPendingIO.Set(SocketEventFlags::kRead);
    Signal();
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::RequestCallbackOnPendingWrite(SocketWatchToken token)
{
    SocketWatch * watch = GetWatch(token);
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->mPendingIO.Set(SocketEventFlags::kWrite);
    Signal();
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::ClearCallbackOnPendingRead(SocketWatchToken token)
{
    SocketWatch * watch = GetWatch(token);
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->mPendingIO.Clear(SocketEventFlags::kRead);
    Signal();
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::ClearCallbackOnPendingWrite(SocketWatchToken token)
{
    SocketWatch * watch = GetWatch(token);
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->mPendingIO.Clear(SocketEventFlags::kWrite);
    Signal();
    return CHIP_NO_ERROR;
}

CHIP_ERROR LayerImplWindows::StopWatchingSocket(SocketWatchToken * tokenInOut)
{
    VerifyOrReturnError(tokenInOut != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    SocketWatch * watch = GetWatch(*tokenInOut);
    *tokenInOut         = InvalidSocketWatchToken();
    VerifyOrReturnError(watch != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    watch->Clear();
    Signal();
    return CHIP_NO_ERROR;
}

void LayerImplWindows::AddLoopHandler(EventLoopHandler & handler)
{
    auto & state = LoopHandlerState(handler);
    VerifyOrReturn(state == kLoopHandlerInactive);
    state = kLoopHandlerPending;
    mLoopHandlers.PushBack(&handler);
    Signal();
}

void LayerImplWindows::RemoveLoopHandler(EventLoopHandler & handler)
{
    mLoopHandlers.Remove(&handler);
    LoopHandlerState(handler) = kLoopHandlerInactive;
}

void LayerImplWindows::PrepareEvents()
{
    assertChipStackLockedByCurrentThread();

    const Clock::Timestamp currentTime = SystemClock().GetMonotonicTimestamp();
    Clock::Timestamp awakenTime        = currentTime + kDefaultMinSleepPeriod;
    if (TimerList::Node * timer = mTimerList.Earliest())
    {
        awakenTime = std::min(awakenTime, timer->AwakenTime());
    }

    auto loopIterator = mLoopHandlers.begin();
    while (loopIterator != mLoopHandlers.end())
    {
        auto & loop = *loopIterator++;
        switch (auto & state = LoopHandlerState(loop))
        {
        case kLoopHandlerPending:
            state = kLoopHandlerActive;
            [[fallthrough]];
        case kLoopHandlerActive:
            awakenTime = std::min(awakenTime, loop.PrepareEvents(currentTime));
            break;
        }
    }

    const Clock::Timeout sleepTime = awakenTime > currentTime ? awakenTime - currentTime : Clock::kZero;
    const auto timeoutMilliseconds = Clock::Milliseconds64(sleepTime).count();
    mPollTimeoutMilliseconds       = static_cast<int>(
        std::min<int64_t>(timeoutMilliseconds, std::numeric_limits<int>::max()));

    mPollEntryCount             = 1;
    mPollDescriptors[0].fd      = mWakeEvent.GetSocket();
    mPollDescriptors[0].events  = POLLRDNORM;
    mPollDescriptors[0].revents = 0;
    mPollWatches[0]             = nullptr;
    mPollGenerations[0]         = 0;

    for (auto & watch : mSocketWatchPool)
    {
        if (watch.mSocket == kInvalidSocketHandle || !watch.mPendingIO.HasAny())
        {
            continue;
        }

        WSAPOLLFD & descriptor             = mPollDescriptors[mPollEntryCount];
        descriptor.fd                      = watch.mSocket;
        descriptor.events                  = 0;
        descriptor.revents                 = 0;
        mPollWatches[mPollEntryCount]       = &watch;
        mPollGenerations[mPollEntryCount] = watch.mGeneration;
        ++mPollEntryCount;
        if (watch.mPendingIO.Has(SocketEventFlags::kRead))
        {
            descriptor.events |= POLLRDNORM;
        }
        if (watch.mPendingIO.Has(SocketEventFlags::kWrite))
        {
            descriptor.events |= POLLWRNORM;
        }
    }
}

void LayerImplWindows::WaitForEvents()
{
    mPollResult = WSAPoll(mPollDescriptors, mPollEntryCount, mPollTimeoutMilliseconds);
}

SocketEvents LayerImplWindows::SocketEventsFromPoll(short events)
{
    SocketEvents result;
    if ((events & (POLLIN | POLLRDNORM)) != 0)
    {
        result.Set(SocketEventFlags::kRead);
    }
    if ((events & (POLLOUT | POLLWRNORM)) != 0)
    {
        result.Set(SocketEventFlags::kWrite);
    }
    if ((events & POLLPRI) != 0)
    {
        result.Set(SocketEventFlags::kExcept);
    }
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        result.Set(SocketEventFlags::kError);
    }
    return result;
}

void LayerImplWindows::HandleEvents()
{
    assertChipStackLockedByCurrentThread();

    if (mPollResult == SOCKET_ERROR)
    {
        ChipLogError(chipSystemLayer, "WSAPoll failed: %" CHIP_ERROR_FORMAT, CHIP_ERROR_WINDOWS(WSAGetLastError()).Format());
        return;
    }

    if (mPollResult > 0 && mPollDescriptors[0].revents != 0)
    {
        const CHIP_ERROR error = mWakeEvent.Confirm();
        if (error != CHIP_NO_ERROR)
        {
            ChipLogError(chipSystemLayer, "Windows event-loop confirm failed: %" CHIP_ERROR_FORMAT, error.Format());
        }
    }

    VerifyOrDieWithMsg(mExpiredTimers.Empty(), chipSystemLayer, "Re-entry into HandleEvents from a timer callback?");
    mExpiredTimers          = mTimerList.ExtractEarlier(Clock::Timeout(1) + SystemClock().GetMonotonicTimestamp());
    TimerList::Node * timer = nullptr;
    while ((timer = mExpiredTimers.PopEarliest()) != nullptr)
    {
        mTimerPool.Invoke(timer);
    }

    if (mPollResult > 0)
    {
        for (ULONG index = 1; index < mPollEntryCount; ++index)
        {
            SocketWatch * watch = mPollWatches[index];
            if (mPollDescriptors[index].revents != 0 && watch->mGeneration == mPollGenerations[index] &&
                watch->mSocket == mPollDescriptors[index].fd && watch->mCallback != nullptr)
            {
                watch->mCallback(SocketEventsFromPoll(mPollDescriptors[index].revents), watch->mCallbackData);
            }
        }
    }

    auto loopIterator = mLoopHandlers.begin();
    while (loopIterator != mLoopHandlers.end())
    {
        auto & loop = *loopIterator++;
        if (LoopHandlerState(loop) == kLoopHandlerActive)
        {
            loop.HandleEvents();
        }
    }
}

void LayerImplWindows::SocketWatch::Clear()
{
    ++mGeneration;
    mSocket = kInvalidSocketHandle;
    mPendingIO.ClearAll();
    mCallback     = nullptr;
    mCallbackData = 0;
}

} // namespace System
} // namespace chip
