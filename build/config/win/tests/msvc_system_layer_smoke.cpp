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

#include <WS2tcpip.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

void TimerCallback(chip::System::Layer *, void * context)
{
    ++*static_cast<unsigned int *>(context);
}

void SocketCallback(chip::System::SocketEvents events, intptr_t context)
{
    if (events.Has(chip::System::SocketEventFlags::kRead))
    {
        ++*reinterpret_cast<unsigned int *>(context);
    }
}

struct ReplaceWatchContext
{
    chip::System::LayerImplWindows * layer;
    chip::System::SocketWatchToken * token;
    SOCKET socket;
    unsigned int * callbacks;
    CHIP_ERROR error = CHIP_NO_ERROR;
};

void ReplaceWatch(chip::System::Layer *, void * context)
{
    auto & replacement = *static_cast<ReplaceWatchContext *>(context);
    replacement.error  = replacement.layer->StopWatchingSocket(replacement.token);
    if (replacement.error == CHIP_NO_ERROR)
    {
        replacement.error = replacement.layer->StartWatchingSocket(replacement.socket, replacement.token);
    }
    if (replacement.error == CHIP_NO_ERROR)
    {
        replacement.error =
            replacement.layer->SetCallback(*replacement.token, SocketCallback, reinterpret_cast<intptr_t>(replacement.callbacks));
    }
    if (replacement.error == CHIP_NO_ERROR)
    {
        replacement.error = replacement.layer->RequestCallbackOnPendingRead(*replacement.token);
    }
}

class TestLoopHandler : public chip::System::EventLoopHandler
{
public:
    chip::System::Clock::Timestamp PrepareEvents(chip::System::Clock::Timestamp) override
    {
        return chip::System::Clock::Timestamp::max();
    }

    void HandleEvents() override { ++mCallbacks; }
    unsigned int mCallbacks = 0;
};

} // namespace

int main()
{
    using namespace std::chrono_literals;

    chip::System::LayerImplWindows layer;
    if (layer.Init() != CHIP_NO_ERROR || !layer.IsInitialized())
    {
        return 1;
    }

    unsigned int timerCallbacks = 0;
    if (layer.StartTimer(chip::System::Clock::Milliseconds32(10), TimerCallback, &timerCallbacks) != CHIP_NO_ERROR)
    {
        return 1;
    }
    do
    {
        layer.PrepareEvents();
        layer.WaitForEvents();
        layer.HandleEvents();
    } while (timerCallbacks == 0);
    if (timerCallbacks != 1 || layer.IsTimerActive(TimerCallback, &timerCallbacks))
    {
        return 1;
    }

    layer.PrepareEvents();
    std::atomic<bool> waitCompleted = false;
    std::thread waiter([&layer, &waitCompleted]() {
        layer.WaitForEvents();
        waitCompleted = true;
    });
    std::this_thread::sleep_for(10ms);
    layer.Signal();
    waiter.join();
    layer.HandleEvents();
    if (!waitCompleted)
    {
        return 1;
    }

    SOCKET receiver = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET sender   = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (receiver == INVALID_SOCKET || sender == INVALID_SOCKET)
    {
        return 1;
    }

    sockaddr_in6 receiverAddress = {};
    receiverAddress.sin6_family  = AF_INET6;
    receiverAddress.sin6_addr    = in6addr_loopback;
    if (bind(receiver, reinterpret_cast<const sockaddr *>(&receiverAddress), sizeof(receiverAddress)) == SOCKET_ERROR)
    {
        return 1;
    }
    int receiverAddressLength = sizeof(receiverAddress);
    if (getsockname(receiver, reinterpret_cast<sockaddr *>(&receiverAddress), &receiverAddressLength) == SOCKET_ERROR)
    {
        return 1;
    }

    chip::System::SocketWatchToken watch = layer.InvalidSocketWatchToken();
    unsigned int socketCallbacks         = 0;
    if (layer.StartWatchingSocket(receiver, &watch) != CHIP_NO_ERROR ||
        layer.SetCallback(watch, SocketCallback, reinterpret_cast<intptr_t>(&socketCallbacks)) != CHIP_NO_ERROR ||
        layer.RequestCallbackOnPendingRead(watch) != CHIP_NO_ERROR)
    {
        return 1;
    }

    constexpr char payload = 1;
    if (sendto(sender, &payload, sizeof(payload), 0, reinterpret_cast<const sockaddr *>(&receiverAddress),
               sizeof(receiverAddress)) == SOCKET_ERROR)
    {
        return 1;
    }

    ReplaceWatchContext replacement = { &layer, &watch, receiver, &socketCallbacks };
    if (layer.ScheduleWork(ReplaceWatch, &replacement) != CHIP_NO_ERROR)
    {
        return 1;
    }
    layer.PrepareEvents();
    layer.WaitForEvents();
    layer.HandleEvents();
    if (replacement.error != CHIP_NO_ERROR || socketCallbacks != 0)
    {
        return 1;
    }

    layer.PrepareEvents();
    layer.WaitForEvents();
    layer.HandleEvents();
    if (socketCallbacks != 1 || layer.StopWatchingSocket(&watch) != CHIP_NO_ERROR ||
        watch != layer.InvalidSocketWatchToken())
    {
        return 1;
    }

    closesocket(sender);
    closesocket(receiver);

    TestLoopHandler loopHandler;
    layer.AddLoopHandler(loopHandler);
    layer.PrepareEvents();
    layer.WaitForEvents();
    layer.HandleEvents();
    if (loopHandler.mCallbacks != 1)
    {
        return 1;
    }

    layer.Shutdown();
    if (layer.IsInitialized() || layer.Init() != CHIP_NO_ERROR)
    {
        return 1;
    }
    layer.AddLoopHandler(loopHandler);
    layer.PrepareEvents();
    layer.WaitForEvents();
    layer.HandleEvents();
    if (loopHandler.mCallbacks != 2)
    {
        return 1;
    }
    layer.RemoveLoopHandler(loopHandler);
    layer.Shutdown();
    return 0;
}
