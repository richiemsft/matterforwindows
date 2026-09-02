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

#include <inet/windows/WindowsSocket.h>

#include <utility>

namespace chip {
namespace Inet {
namespace Internal {

WindowsSocketSystem::WindowsSocketSystem()
{
    WSADATA data;
    mError = WSAStartup(MAKEWORD(2, 2), &data);
}

WindowsSocketSystem::~WindowsSocketSystem()
{
    if (IsInitialized())
    {
        WSACleanup();
    }
}

WindowsSocket::~WindowsSocket()
{
    Close();
}

WindowsSocket::WindowsSocket(WindowsSocket && other) noexcept : mSocket(std::exchange(other.mSocket, INVALID_SOCKET)) {}

WindowsSocket & WindowsSocket::operator=(WindowsSocket && other) noexcept
{
    if (this != &other)
    {
        Close();
        mSocket = std::exchange(other.mSocket, INVALID_SOCKET);
    }
    return *this;
}

std::optional<WindowsSocket> WindowsSocket::Create(int addressFamily, int type, int protocol, int & error)
{
    SOCKET socket = WSASocketW(addressFamily, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET)
    {
        error = WSAGetLastError();
        return std::nullopt;
    }

    error = 0;
    return WindowsSocket(socket);
}

void WindowsSocket::Close()
{
    if (mSocket != INVALID_SOCKET)
    {
        closesocket(mSocket);
        mSocket = INVALID_SOCKET;
    }
}

} // namespace Internal
} // namespace Inet
} // namespace chip
