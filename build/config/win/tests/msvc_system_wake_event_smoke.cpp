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

#include <system/windows/WindowsWakeEvent.h>

#include <Windows.h>

#include <atomic>
#include <thread>

namespace {

int Poll(chip::System::Internal::WindowsWakeEvent & wakeEvent, int timeoutMilliseconds)
{
    WSAPOLLFD descriptor = {};
    descriptor.fd        = wakeEvent.GetSocket();
    descriptor.events    = POLLRDNORM;
    return WSAPoll(&descriptor, 1, timeoutMilliseconds);
}

} // namespace

int main()
{
    static_assert(sizeof(SOCKET) == sizeof(uintptr_t), "SOCKET must retain pointer width");

    chip::System::Internal::WindowsWakeEvent wakeEvent;
    if (wakeEvent.Open() != CHIP_NO_ERROR || !wakeEvent.IsOpen() || Poll(wakeEvent, 0) != 0)
    {
        return 1;
    }

    if (wakeEvent.Notify() != CHIP_NO_ERROR || wakeEvent.Notify() != CHIP_NO_ERROR || Poll(wakeEvent, 0) != 1 ||
        wakeEvent.Confirm() != CHIP_NO_ERROR || Poll(wakeEvent, 0) != 0)
    {
        return 1;
    }

    std::atomic<int> pollResult = SOCKET_ERROR;
    std::thread waiter([&wakeEvent, &pollResult]() { pollResult = Poll(wakeEvent, 5000); });
    Sleep(10);
    const CHIP_ERROR notifyError = wakeEvent.Notify();
    waiter.join();
    if (notifyError != CHIP_NO_ERROR || pollResult != 1 || wakeEvent.Confirm() != CHIP_NO_ERROR)
    {
        return 1;
    }

    wakeEvent.Close();
    if (wakeEvent.IsOpen() || wakeEvent.Notify() != CHIP_ERROR_INCORRECT_STATE ||
        wakeEvent.Confirm() != CHIP_ERROR_INCORRECT_STATE || wakeEvent.Open() != CHIP_NO_ERROR)
    {
        return 1;
    }

    return 0;
}
