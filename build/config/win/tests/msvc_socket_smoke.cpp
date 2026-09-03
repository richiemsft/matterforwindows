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
#include <system/SocketEvents.h>

#include <WS2tcpip.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

namespace {

using chip::Inet::Internal::WindowsSocket;

bool BindToLoopback(WindowsSocket & socket, sockaddr_in6 & address)
{
    address             = {};
    address.sin6_family = AF_INET6;
    address.sin6_addr   = in6addr_loopback;

    if (bind(socket.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        return false;
    }

    int addressLength = sizeof(address);
    return getsockname(socket.Get(), reinterpret_cast<sockaddr *>(&address), &addressLength) != SOCKET_ERROR;
}

} // namespace

int main()
{
    using chip::Inet::Internal::WindowsSocketSystem;

    static_assert(sizeof(chip::System::SocketHandle) == sizeof(SOCKET), "System socket handles must retain pointer width");
    static_assert(chip::System::kInvalidSocketHandle == INVALID_SOCKET, "System socket invalid value must match WinSock");

    WindowsSocketSystem socketSystem;
    if (!socketSystem.IsInitialized())
    {
        return 1;
    }

    int error = 0;
    auto receiver = WindowsSocket::Create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, error);
    auto sender   = WindowsSocket::Create(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, error);
    if (!receiver.has_value() || !sender.has_value())
    {
        return 1;
    }

    sockaddr_in6 receiverAddress;
    if (!BindToLoopback(receiver.value(), receiverAddress))
    {
        return 1;
    }

    constexpr std::array<uint8_t, 4> kPayload = { 0x4d, 0x41, 0x54, 0x52 };
    const int sent = sendto(sender->Get(), reinterpret_cast<const char *>(kPayload.data()), static_cast<int>(kPayload.size()), 0,
                            reinterpret_cast<const sockaddr *>(&receiverAddress), sizeof(receiverAddress));
    if (sent != static_cast<int>(kPayload.size()))
    {
        return 1;
    }

    WSAPOLLFD pollDescriptor = {};
    pollDescriptor.fd        = receiver->Get();
    pollDescriptor.events    = POLLRDNORM;
    if (WSAPoll(&pollDescriptor, 1, 2000) != 1 || (pollDescriptor.revents & POLLRDNORM) == 0)
    {
        return 1;
    }

    std::array<uint8_t, kPayload.size()> received;
    const int receivedLength =
        recv(receiver->Get(), reinterpret_cast<char *>(received.data()), static_cast<int>(received.size()), 0);

    WindowsSocket moved = std::move(sender.value());
    if (sender->IsValid() || !moved.IsValid())
    {
        return 1;
    }

    receiver->Close();
    moved.Close();
    return receivedLength == static_cast<int>(received.size()) && received == kPayload ? 0 : 1;
}
