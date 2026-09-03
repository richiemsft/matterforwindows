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

#include <lib/core/CHIPError.h>

namespace chip {
namespace System {
namespace Internal {

class WindowsWakeEvent
{
public:
    WindowsWakeEvent() = default;
    ~WindowsWakeEvent();

    WindowsWakeEvent(const WindowsWakeEvent &)             = delete;
    WindowsWakeEvent & operator=(const WindowsWakeEvent &) = delete;

    CHIP_ERROR Open();
    void Close();
    CHIP_ERROR Notify() const;
    CHIP_ERROR Confirm() const;

    SOCKET GetSocket() const { return mReceiver; }
    bool IsOpen() const { return mReceiver != INVALID_SOCKET; }

private:
    SOCKET mReceiver = INVALID_SOCKET;
    SOCKET mSender   = INVALID_SOCKET;
    bool mWinsockInitialized = false;
};

} // namespace Internal
} // namespace System
} // namespace chip
