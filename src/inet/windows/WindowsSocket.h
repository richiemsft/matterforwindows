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

#include <optional>

namespace chip {
namespace Inet {
namespace Internal {

class WindowsSocketSystem
{
public:
    WindowsSocketSystem();
    ~WindowsSocketSystem();

    WindowsSocketSystem(const WindowsSocketSystem &)             = delete;
    WindowsSocketSystem & operator=(const WindowsSocketSystem &) = delete;

    bool IsInitialized() const { return mError == 0; }
    int Error() const { return mError; }

private:
    int mError;
};

class WindowsSocket
{
public:
    WindowsSocket() = default;
    ~WindowsSocket();

    WindowsSocket(WindowsSocket && other) noexcept;
    WindowsSocket & operator=(WindowsSocket && other) noexcept;

    WindowsSocket(const WindowsSocket &)             = delete;
    WindowsSocket & operator=(const WindowsSocket &) = delete;

    static std::optional<WindowsSocket> Create(int addressFamily, int type, int protocol, int & error);

    SOCKET Get() const { return mSocket; }
    bool IsValid() const { return mSocket != INVALID_SOCKET; }
    void Close();

private:
    explicit WindowsSocket(SOCKET socket) : mSocket(socket) {}

    SOCKET mSocket = INVALID_SOCKET;
};

} // namespace Internal
} // namespace Inet
} // namespace chip
