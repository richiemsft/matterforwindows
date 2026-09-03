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
// TCPEndPointImplSockets.cpp. It drives the real Inet TCP endpoint on top of
// the Windows system layer over IPv6 and IPv4 loopback: ephemeral bind, listen,
// non-blocking connect, accept, bidirectional send/receive pumped through
// LayerImplWindows, peer-address reporting, clean shutdown, and bounded
// connection-refusal error reporting.
//
// WinSock2.h / WS2tcpip.h must precede the CHIP headers (which pull in the
// Windows system layer) so the raw sockets used by the free-port probe resolve
// against the same WinSock definitions.
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <inet/IPAddress.h>
#include <inet/InetInterface.h>
#include <inet/TCPEndPointImpl.h>
#include <system/SystemClock.h>
#include <system/SystemPacketBuffer.h>
#include <system/windows/SystemLayerImplWindows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

using chip::Inet::InterfaceId;
using chip::Inet::IPAddress;
using chip::Inet::IPAddressType;
using chip::Inet::TCPEndPoint;
using chip::Inet::TCPEndPointHandle;
using chip::Inet::TCPEndPointManagerImpl;
using chip::System::LayerImplWindows;
using chip::System::PacketBufferHandle;

namespace {

constexpr uint8_t kClientToServer[] = { 'p', 'i', 'n', 'g' };
constexpr uint8_t kServerToClient[] = { 'p', 'o', 'n', 'g' };

struct DataSink
{
    bool received = false;
    size_t length = 0;
    uint8_t payload[16] = {};

    bool Matches(const uint8_t * expected, size_t expectedLen) const
    {
        return received && length == expectedLen && memcmp(payload, expected, expectedLen) == 0;
    }
};

struct ServerState
{
    TCPEndPointHandle accepted;
    bool acceptError = false;
    IPAddress peerAddress;
    uint16_t peerPort = 0;
    DataSink sink;
};

struct ClientState
{
    bool connectComplete = false;
    CHIP_ERROR connectError = CHIP_NO_ERROR;
    DataSink sink;
};

CHIP_ERROR StoreData(const TCPEndPointHandle & endPoint, PacketBufferHandle && data)
{
    auto * sink = static_cast<DataSink *>(endPoint->mAppState);
    if (!data.IsNull())
    {
        sink->length = data->DataLength();
        if (sink->length <= sizeof(sink->payload))
        {
            memcpy(sink->payload, data->Start(), sink->length);
        }
        sink->received = true;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR OnServerDataReceived(const TCPEndPointHandle & endPoint, PacketBufferHandle && data)
{
    return StoreData(endPoint, std::move(data));
}

CHIP_ERROR OnClientDataReceived(const TCPEndPointHandle & endPoint, PacketBufferHandle && data)
{
    return StoreData(endPoint, std::move(data));
}

void OnConnectionReceived(const TCPEndPointHandle & listener, const TCPEndPointHandle & connection, const IPAddress & peerAddr,
                          uint16_t peerPort)
{
    auto * server        = static_cast<ServerState *>(listener->mAppState);
    server->accepted     = connection;
    server->peerAddress  = peerAddr;
    server->peerPort     = peerPort;
    connection->mAppState = &server->sink;
    connection->OnDataReceived = OnServerDataReceived;
}

void OnAcceptError(const TCPEndPointHandle & listener, CHIP_ERROR)
{
    static_cast<ServerState *>(listener->mAppState)->acceptError = true;
}

void OnConnectComplete(const TCPEndPointHandle & endPoint, CHIP_ERROR err)
{
    auto * client           = static_cast<ClientState *>(endPoint->mAppState);
    client->connectComplete = true;
    client->connectError    = err;
}

void Timeout(chip::System::Layer *, void * appState)
{
    *static_cast<bool *>(appState) = true;
}

// Pumps the Windows event loop until the predicate is satisfied or a bounded
// timer fires, guaranteeing the test cannot hang.
template <typename Predicate>
bool PumpUntil(LayerImplWindows & layer, Predicate predicate)
{
    bool timedOut = false;
    if (layer.StartTimer(chip::System::Clock::Seconds16(2), Timeout, &timedOut) != CHIP_NO_ERROR)
    {
        return false;
    }
    while (!predicate() && !timedOut)
    {
        layer.PrepareEvents();
        layer.WaitForEvents();
        layer.HandleEvents();
    }
    layer.CancelTimer(Timeout, &timedOut);
    return predicate();
}

// Uses a throwaway raw WinSock socket to reserve then release an ephemeral
// loopback port, yielding a port number that is momentarily free.
bool ProbeEphemeralPort(IPAddressType type, uint16_t & outPort)
{
    const int family = (type == IPAddressType::kIPv6) ? AF_INET6 : AF_INET;
    SOCKET probe      = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_storage storage = {};
    int addrLen              = 0;
    if (type == IPAddressType::kIPv6)
    {
        auto * addr        = reinterpret_cast<sockaddr_in6 *>(&storage);
        addr->sin6_family  = AF_INET6;
        addr->sin6_addr    = in6addr_loopback;
        addr->sin6_port    = 0;
        addrLen            = static_cast<int>(sizeof(sockaddr_in6));
    }
    else
    {
        auto * addr           = reinterpret_cast<sockaddr_in *>(&storage);
        addr->sin_family      = AF_INET;
        addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr->sin_port        = 0;
        addrLen               = static_cast<int>(sizeof(sockaddr_in));
    }

    bool ok = ::bind(probe, reinterpret_cast<sockaddr *>(&storage), addrLen) == 0;
    if (ok)
    {
        sockaddr_storage bound = {};
        int boundLen           = static_cast<int>(sizeof(bound));
        ok                     = ::getsockname(probe, reinterpret_cast<sockaddr *>(&bound), &boundLen) == 0;
        if (ok)
        {
            const uint16_t netPort =
                (type == IPAddressType::kIPv6) ? reinterpret_cast<sockaddr_in6 *>(&bound)->sin6_port
                                               : reinterpret_cast<sockaddr_in *>(&bound)->sin_port;
            outPort = ntohs(netPort);
            ok      = outPort != 0;
        }
    }

    ::closesocket(probe);
    return ok;
}

// Full connect/accept/bidirectional-exchange cycle for one address family.
bool RunConnectedExchange(LayerImplWindows & layer, TCPEndPointManagerImpl & manager, IPAddressType type,
                          const IPAddress & loopback)
{
    uint16_t listenPort = 0;
    if (!ProbeEphemeralPort(type, listenPort))
    {
        printf("  probe for free port failed\n");
        return false;
    }

    ServerState server;
    TCPEndPointHandle listener;
    if (manager.NewEndPoint(listener) != CHIP_NO_ERROR)
    {
        return false;
    }
    listener->mAppState             = &server;
    listener->OnConnectionReceived  = OnConnectionReceived;
    listener->OnAcceptError         = OnAcceptError;
    if (listener->Bind(type, loopback, listenPort, /* reuseAddr */ true) != CHIP_NO_ERROR)
    {
        printf("  listener bind failed\n");
        return false;
    }
    if (listener->Listen(1) != CHIP_NO_ERROR)
    {
        printf("  listen failed\n");
        return false;
    }

    ClientState client;
    TCPEndPointHandle connector;
    if (manager.NewEndPoint(connector) != CHIP_NO_ERROR)
    {
        return false;
    }
    connector->mAppState        = &client;
    connector->OnConnectComplete = OnConnectComplete;
    connector->OnDataReceived    = OnClientDataReceived;
    if (connector->Connect(loopback, listenPort, InterfaceId::Null()) != CHIP_NO_ERROR)
    {
        printf("  connect initiation failed\n");
        return false;
    }

    if (!PumpUntil(layer, [&] { return client.connectComplete && !server.accepted.IsNull(); }))
    {
        printf("  connect/accept did not complete\n");
        return false;
    }
    if (client.connectError != CHIP_NO_ERROR)
    {
        printf("  connect reported error\n");
        return false;
    }

    // Peer/local reporting: the connector's peer port must equal the listen
    // port and its local (ephemeral) port must be non-zero.
    IPAddress peerAddr;
    uint16_t peerPort = 0;
    if (connector->GetPeerInfo(&peerAddr, &peerPort) != CHIP_NO_ERROR || peerPort != listenPort || peerAddr != loopback)
    {
        printf("  peer info mismatch\n");
        return false;
    }
    IPAddress localAddr;
    uint16_t localPort = 0;
    if (connector->GetLocalInfo(&localAddr, &localPort) != CHIP_NO_ERROR || localPort == 0)
    {
        printf("  local info mismatch\n");
        return false;
    }
    // The listener observed the connector's ephemeral source port on accept.
    if (server.peerPort != localPort)
    {
        printf("  accepted peer port mismatch\n");
        return false;
    }

    // Point the connector's inbound sink at the client sink now that connect
    // reporting no longer needs mAppState.
    connector->mAppState = &client.sink;

    // Client -> server.
    if (connector->Send(PacketBufferHandle::NewWithData(kClientToServer, sizeof(kClientToServer))) != CHIP_NO_ERROR)
    {
        printf("  client send failed\n");
        return false;
    }
    if (!PumpUntil(layer, [&] { return server.sink.received; }) ||
        !server.sink.Matches(kClientToServer, sizeof(kClientToServer)))
    {
        printf("  server did not receive client payload\n");
        return false;
    }

    // Server -> client (bidirectional).
    if (server.accepted->Send(PacketBufferHandle::NewWithData(kServerToClient, sizeof(kServerToClient))) != CHIP_NO_ERROR)
    {
        printf("  server send failed\n");
        return false;
    }
    if (!PumpUntil(layer, [&] { return client.sink.received; }) ||
        !client.sink.Matches(kServerToClient, sizeof(kServerToClient)))
    {
        printf("  client did not receive server payload\n");
        return false;
    }

    if (server.acceptError)
    {
        printf("  unexpected accept error\n");
        return false;
    }

    server.accepted->Close();
    server.accepted.Release();
    connector->Close();
    connector.Release();
    listener->Close();
    listener.Release();
    return true;
}

// Connection-refusal probe. WSAPoll does not reliably surface failed
// non-blocking connects, so configure the endpoint's supported connect timeout
// and require an error completion rather than allowing a pending connection.
bool RunConnectionRefusal(LayerImplWindows & layer, TCPEndPointManagerImpl & manager, IPAddressType type,
                          const IPAddress & loopback)
{
    uint16_t deadPort = 0;
    if (!ProbeEphemeralPort(type, deadPort))
    {
        return false;
    }

    ClientState client;
    TCPEndPointHandle connector;
    if (manager.NewEndPoint(connector) != CHIP_NO_ERROR)
    {
        return false;
    }
    connector->mAppState         = &client;
    connector->OnConnectComplete = OnConnectComplete;
    connector->SetConnectTimeout(500);
    if (connector->Connect(loopback, deadPort, InterfaceId::Null()) != CHIP_NO_ERROR)
    {
        // A synchronous refusal is also acceptable.
        connector.Release();
        return true;
    }

    if (!PumpUntil(layer, [&] { return client.connectComplete; }))
    {
        printf("  refusal did not complete within the endpoint timeout\n");
        connector->Close();
        connector.Release();
        return false;
    }
    if (client.connectError == CHIP_NO_ERROR)
    {
        printf("  refusal reported success unexpectedly\n");
        connector->Close();
        connector.Release();
        return false;
    }

    connector->Close();
    connector.Release();
    return true;
}

bool RunFamily(LayerImplWindows & layer, TCPEndPointManagerImpl & manager, IPAddressType type, const char * literal)
{
    IPAddress loopback;
    if (!IPAddress::FromString(literal, loopback))
    {
        return false;
    }
    if (!RunConnectedExchange(layer, manager, type, loopback))
    {
        return false;
    }
    if (!RunConnectionRefusal(layer, manager, type, loopback))
    {
        return false;
    }
    return true;
}

} // namespace

int main()
{
    LayerImplWindows layer;
    if (layer.Init() != CHIP_NO_ERROR || !layer.IsInitialized())
    {
        printf("layer init failed\n");
        return 1;
    }

    TCPEndPointManagerImpl manager;
    if (manager.Init(layer) != CHIP_NO_ERROR)
    {
        printf("manager init failed\n");
        return 1;
    }

    int status = 0;
    printf("IPv6 loopback...\n");
    if (!RunFamily(layer, manager, IPAddressType::kIPv6, "::1"))
    {
        status = 1;
    }
    printf("IPv4 loopback...\n");
    if (!RunFamily(layer, manager, IPAddressType::kIPv4, "127.0.0.1"))
    {
        status = 1;
    }

    manager.Shutdown();
    layer.Shutdown();
    if (layer.IsInitialized())
    {
        status = 1;
    }

    printf(status == 0 ? "PASS\n" : "FAIL\n");
    return status;
}
