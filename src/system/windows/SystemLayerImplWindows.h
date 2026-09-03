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

#pragma once

#include <WinSock2.h>

#include <lib/support/IntrusiveList.h>
#include <lib/support/ObjectLifeCycle.h>
#include <system/SystemLayer.h>
#include <system/SystemTimer.h>
#include <system/windows/WindowsWakeEvent.h>

namespace chip {
namespace System {

static_assert(CHIP_SYSTEM_CONFIG_USE_SOCKETS, "The Windows System Layer requires WinSock");

class LayerImplWindows : public LayerSelectLoop
{
public:
    LayerImplWindows() = default;
    ~LayerImplWindows() override;

    CriticalFailure Init() override;
    void Shutdown() override;
    bool IsInitialized() const override { return mLayerState.IsInitialized(); }
    CriticalFailure StartTimer(Clock::Timeout delay, TimerCompleteCallback onComplete, void * appState) override;
    CHIP_ERROR ExtendTimerTo(Clock::Timeout delay, TimerCompleteCallback onComplete, void * appState) override;
    bool IsTimerActive(TimerCompleteCallback onComplete, void * appState) override;
    Clock::Timeout GetRemainingTime(TimerCompleteCallback onComplete, void * appState) override;
    void CancelTimer(TimerCompleteCallback onComplete, void * appState) override;
    CriticalFailure ScheduleWork(TimerCompleteCallback onComplete, void * appState) override;

    CHIP_ERROR StartWatchingSocket(SocketHandle socket, SocketWatchToken * tokenOut) override;
    CHIP_ERROR SetCallback(SocketWatchToken token, SocketWatchCallback callback, intptr_t data) override;
    CHIP_ERROR RequestCallbackOnPendingRead(SocketWatchToken token) override;
    CHIP_ERROR RequestCallbackOnPendingWrite(SocketWatchToken token) override;
    CHIP_ERROR ClearCallbackOnPendingRead(SocketWatchToken token) override;
    CHIP_ERROR ClearCallbackOnPendingWrite(SocketWatchToken token) override;
    CHIP_ERROR StopWatchingSocket(SocketWatchToken * tokenInOut) override;
    SocketWatchToken InvalidSocketWatchToken() override { return 0; }

    void Signal() override;
    void EventLoopBegins() override {}
    void PrepareEvents() override;
    void WaitForEvents() override;
    void HandleEvents() override;
    void EventLoopEnds() override {}

    void AddLoopHandler(EventLoopHandler & handler) override;
    void RemoveLoopHandler(EventLoopHandler & handler) override;

private:
    struct SocketWatch
    {
        void Clear();

        SocketHandle mSocket = kInvalidSocketHandle;
        SocketEvents mPendingIO;
        SocketWatchCallback mCallback = nullptr;
        intptr_t mCallbackData        = 0;
        uint64_t mGeneration          = 0;
    };

    static constexpr size_t kSocketWatchMax = (INET_CONFIG_ENABLE_TCP_ENDPOINT ? INET_CONFIG_NUM_TCP_ENDPOINTS : 0) +
        (INET_CONFIG_ENABLE_UDP_ENDPOINT ? INET_CONFIG_NUM_UDP_ENDPOINTS : 0);
    static constexpr size_t kPollEntryMax = kSocketWatchMax + 1;

    static SocketEvents SocketEventsFromPoll(short events);
    SocketWatch * GetWatch(SocketWatchToken token);

    TimerPool<TimerList::Node> mTimerPool;
    TimerList mTimerList;
    TimerList mExpiredTimers;
    IntrusiveList<EventLoopHandler> mLoopHandlers;
    SocketWatch mSocketWatchPool[kSocketWatchMax];
    WSAPOLLFD mPollDescriptors[kPollEntryMax];
    SocketWatch * mPollWatches[kPollEntryMax];
    uint64_t mPollGenerations[kPollEntryMax];
    ULONG mPollEntryCount = 0;
    int mPollTimeoutMilliseconds = 0;
    int mPollResult              = SOCKET_ERROR;
    Internal::WindowsWakeEvent mWakeEvent;
    ObjectLifeCycle mLayerState;
};

using LayerImpl = LayerImplWindows;

} // namespace System
} // namespace chip
