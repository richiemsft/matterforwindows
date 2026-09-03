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

// Runtime smoke test for the native WinSock branches of the shared
// UDPEndPointImplSockets.cpp. It drives the real Inet UDP endpoint on top of
// the Windows system layer: ephemeral bind, packet-info send/receive over IPv6
// and IPv4 loopback, multicast loopback + join/leave, and clean shutdown.

#include <inet/IPAddress.h>
#include <inet/IPPacketInfo.h>
#include <inet/InetInterface.h>
#include <inet/UDPEndPointImpl.h>
#include <system/SystemPacketBuffer.h>
#include <system/windows/SystemLayerImplWindows.h>

#include <cstdint>
#include <cstring>
#include <utility>

using chip::Inet::InterfaceId;
using chip::Inet::IPAddress;
using chip::Inet::IPAddressType;
using chip::Inet::IPPacketInfo;
using chip::Inet::kIPVersion_6;
using chip::Inet::UDPEndPoint;
using chip::Inet::UDPEndPointHandle;
using chip::Inet::UDPEndPointManagerImpl;
using chip::System::LayerImplWindows;
using chip::System::PacketBufferHandle;

namespace {

constexpr uint8_t kPayload[]   = { 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72 }; // "Matter"

struct ReceiveState
{
    bool received = false;
    bool errored  = false;
    IPAddress srcAddress;
    IPAddress destAddress;
    uint16_t srcPort  = 0;
    uint16_t destPort = 0;
    InterfaceId interface;
    size_t length      = 0;
    uint8_t payload[8] = {};
};

void OnMessageReceived(UDPEndPoint * endPoint, PacketBufferHandle && buffer, const IPPacketInfo * pktInfo)
{
    auto * state       = static_cast<ReceiveState *>(endPoint->mAppState);
    state->srcAddress  = pktInfo->SrcAddress;
    state->destAddress = pktInfo->DestAddress;
    state->srcPort     = pktInfo->SrcPort;
    state->destPort    = pktInfo->DestPort;
    state->interface   = pktInfo->Interface;
    state->length      = buffer->DataLength();
    if (state->length <= sizeof(state->payload))
    {
        memcpy(state->payload, buffer->Start(), state->length);
    }
    state->received = true;
}

void OnReceiveError(UDPEndPoint * endPoint, CHIP_ERROR, const IPPacketInfo *)
{
    static_cast<ReceiveState *>(endPoint->mAppState)->errored = true;
}

PacketBufferHandle MakePayloadBuffer()
{
    return PacketBufferHandle::NewWithData(kPayload, sizeof(kPayload));
}

void Timeout(chip::System::Layer *, void * appState)
{
    *static_cast<bool *>(appState) = true;
}

// Pumps the Windows event loop until the receiver reports a delivered datagram
// or a bounded timer expires.
bool PumpUntilReceived(LayerImplWindows & layer, ReceiveState & state)
{
    bool timedOut = false;
    if (layer.StartTimer(chip::System::Clock::Seconds16(2), Timeout, &timedOut) != CHIP_NO_ERROR)
    {
        return false;
    }

    while (!state.received && !timedOut)
    {
        layer.PrepareEvents();
        layer.WaitForEvents();
        layer.HandleEvents();
    }
    layer.CancelTimer(Timeout, &timedOut);
    return state.received;
}

// Exercises ephemeral bind + packet-info send/receive for one address family.
bool RunLoopbackExchange(LayerImplWindows & layer, UDPEndPointManagerImpl & manager, IPAddressType type,
                         const IPAddress & loopback)
{
    ReceiveState state;

    UDPEndPointHandle receiver;
    if (manager.NewEndPoint(receiver) != CHIP_NO_ERROR)
    {
        return false;
    }
    if (receiver->Bind(type, loopback, 0) != CHIP_NO_ERROR)
    {
        return false;
    }
    const uint16_t boundPort = receiver->GetBoundPort();
    if (boundPort == 0)
    {
        return false;
    }
    if (receiver->Listen(OnMessageReceived, OnReceiveError, &state) != CHIP_NO_ERROR)
    {
        return false;
    }

    UDPEndPointHandle sender;
    if (manager.NewEndPoint(sender) != CHIP_NO_ERROR)
    {
        return false;
    }
    if (sender->Bind(type, loopback, 0) != CHIP_NO_ERROR)
    {
        return false;
    }

    if (sender->SendTo(loopback, boundPort, MakePayloadBuffer()) != CHIP_NO_ERROR)
    {
        return false;
    }

    const bool received = PumpUntilReceived(layer, state);

    receiver.Release();
    sender.Release();

    if (!received || state.errored)
    {
        return false;
    }
    if (state.length != sizeof(kPayload) || memcmp(state.payload, kPayload, sizeof(kPayload)) != 0)
    {
        return false;
    }
    if (state.destPort != boundPort)
    {
        return false;
    }
    if (state.srcAddress != loopback || state.destAddress != loopback)
    {
        return false;
    }
    // The packet-info control message must identify the receiving interface.
    if (!state.interface.IsPresent())
    {
        return false;
    }
    return true;
}

// Exercises multicast loopback control and IPv6 group join/leave.
bool RunMulticast(UDPEndPointManagerImpl & manager)
{
    IPAddress loopback;
    if (!IPAddress::FromString("::1", loopback))
    {
        return false;
    }
    IPAddress multicastGroup;
    if (!IPAddress::FromString("ff02::1", multicastGroup))
    {
        return false;
    }

    UDPEndPointHandle endPoint;
    if (manager.NewEndPoint(endPoint) != CHIP_NO_ERROR)
    {
        return false;
    }
    if (endPoint->Bind(IPAddressType::kIPv6, loopback, 0) != CHIP_NO_ERROR)
    {
        return false;
    }

    bool ok = endPoint->SetMulticastLoopback(kIPVersion_6, true) == CHIP_NO_ERROR &&
        endPoint->SetMulticastLoopback(kIPVersion_6, false) == CHIP_NO_ERROR &&
        endPoint->JoinMulticastGroup(InterfaceId::Null(), multicastGroup) == CHIP_NO_ERROR &&
        endPoint->LeaveMulticastGroup(InterfaceId::Null(), multicastGroup) == CHIP_NO_ERROR;

    endPoint.Release();
    return ok;
}

} // namespace

int main()
{
    LayerImplWindows layer;
    if (layer.Init() != CHIP_NO_ERROR || !layer.IsInitialized())
    {
        return 1;
    }

    UDPEndPointManagerImpl manager;
    if (manager.Init(layer) != CHIP_NO_ERROR)
    {
        return 1;
    }

    IPAddress loopback6;
    if (!IPAddress::FromString("::1", loopback6))
    {
        return 1;
    }
    if (!RunLoopbackExchange(layer, manager, IPAddressType::kIPv6, loopback6))
    {
        return 1;
    }

    IPAddress loopback4;
    if (!IPAddress::FromString("127.0.0.1", loopback4))
    {
        return 1;
    }
    if (!RunLoopbackExchange(layer, manager, IPAddressType::kIPv4, loopback4))
    {
        return 1;
    }

    if (!RunMulticast(manager))
    {
        return 1;
    }

    manager.Shutdown();
    layer.Shutdown();
    if (layer.IsInitialized())
    {
        return 1;
    }
    return 0;
}
