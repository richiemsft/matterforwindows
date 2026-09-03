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

#include <system/SystemError.h>

#include <WS2tcpip.h>

namespace chip {
namespace System {
namespace Internal {

namespace {

CHIP_ERROR SetNonBlocking(SOCKET socket)
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR ? CHIP_ERROR_WINDOWS(WSAGetLastError()) : CHIP_NO_ERROR;
}

} // namespace

WindowsWakeEvent::~WindowsWakeEvent()
{
    Close();
}

CHIP_ERROR WindowsWakeEvent::Open()
{
    if (IsOpen())
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    WSADATA socketData;
    int result = WSAStartup(MAKEWORD(2, 2), &socketData);
    if (result != 0)
    {
        return CHIP_ERROR_WINDOWS(result);
    }
    mWinsockInitialized = true;
    if (LOBYTE(socketData.wVersion) != 2 || HIBYTE(socketData.wVersion) != 2)
    {
        Close();
        return CHIP_ERROR_VERSION_MISMATCH;
    }

    mReceiver = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (mReceiver == INVALID_SOCKET)
    {
        const CHIP_ERROR error = CHIP_ERROR_WINDOWS(WSAGetLastError());
        Close();
        return error;
    }

    sockaddr_in6 address = {};
    address.sin6_family  = AF_INET6;
    address.sin6_addr    = in6addr_loopback;
    if (bind(mReceiver, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const CHIP_ERROR error = CHIP_ERROR_WINDOWS(WSAGetLastError());
        Close();
        return error;
    }

    int addressLength = sizeof(address);
    if (getsockname(mReceiver, reinterpret_cast<sockaddr *>(&address), &addressLength) == SOCKET_ERROR)
    {
        const CHIP_ERROR error = CHIP_ERROR_WINDOWS(WSAGetLastError());
        Close();
        return error;
    }

    mSender = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (mSender == INVALID_SOCKET)
    {
        const CHIP_ERROR error = CHIP_ERROR_WINDOWS(WSAGetLastError());
        Close();
        return error;
    }

    if (connect(mSender, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const CHIP_ERROR error = CHIP_ERROR_WINDOWS(WSAGetLastError());
        Close();
        return error;
    }

    CHIP_ERROR error = SetNonBlocking(mReceiver);
    if (error == CHIP_NO_ERROR)
    {
        error = SetNonBlocking(mSender);
    }
    if (error != CHIP_NO_ERROR)
    {
        Close();
        return error;
    }

    return CHIP_NO_ERROR;
}

void WindowsWakeEvent::Close()
{
    if (mReceiver != INVALID_SOCKET)
    {
        closesocket(mReceiver);
        mReceiver = INVALID_SOCKET;
    }
    if (mSender != INVALID_SOCKET)
    {
        closesocket(mSender);
        mSender = INVALID_SOCKET;
    }
    if (mWinsockInitialized)
    {
        WSACleanup();
        mWinsockInitialized = false;
    }
}

CHIP_ERROR WindowsWakeEvent::Notify() const
{
    if (!IsOpen())
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    constexpr char notification = 1;
    if (send(mSender, &notification, sizeof(notification), 0) != SOCKET_ERROR)
    {
        return CHIP_NO_ERROR;
    }

    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? CHIP_NO_ERROR : CHIP_ERROR_WINDOWS(error);
}

CHIP_ERROR WindowsWakeEvent::Confirm() const
{
    if (!IsOpen())
    {
        return CHIP_ERROR_INCORRECT_STATE;
    }

    char notifications[128];
    while (recv(mReceiver, notifications, sizeof(notifications), 0) != SOCKET_ERROR)
    {}

    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? CHIP_NO_ERROR : CHIP_ERROR_WINDOWS(error);
}

} // namespace Internal
} // namespace System
} // namespace chip
