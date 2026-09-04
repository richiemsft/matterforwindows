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

#include <lib/dnssd/ServiceNaming.h>
#include <lib/dnssd/platform/Dnssd.h>
#include <platform/PlatformManager.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace chip;
using namespace chip::DeviceLayer;
using namespace chip::Dnssd;

namespace {

struct DiscoveryState
{
    std::mutex mutex;
    std::condition_variable condition;
    intptr_t browseId      = 0;
    size_t resolvedCount   = 0;
    bool browseFinal       = false;
};

void PrintTxtValue(const uint8_t * value, size_t size)
{
    bool printable = value != nullptr;
    for (size_t i = 0; i < size && printable; ++i)
    {
        printable = value[i] >= 0x20 && value[i] <= 0x7e;
    }

    if (printable)
    {
        std::printf("\"%.*s\"", static_cast<int>(size), reinterpret_cast<const char *>(value));
        return;
    }

    std::printf("hex:");
    for (size_t i = 0; i < size; ++i)
    {
        std::printf("%02X", value[i]);
    }
}

void PrintAddress(const Inet::IPAddress & address)
{
    char text[INET6_ADDRSTRLEN];
    const char * result = nullptr;
    if (address.IsIPv4())
    {
        in_addr nativeAddress = address.ToIPv4();
        result                = InetNtopA(AF_INET, &nativeAddress, text, sizeof(text));
    }
    else if (address.IsIPv6())
    {
        in6_addr nativeAddress = address.ToIPv6();
        result                 = InetNtopA(AF_INET6, &nativeAddress, text, sizeof(text));
    }
    std::printf("  Address:   %s\n", result != nullptr ? result : "<conversion failed>");
}

void OnResolve(void * context, DnssdService * service, const Span<Inet::IPAddress> & addresses, CHIP_ERROR error)
{
    auto * state = static_cast<DiscoveryState *>(context);
    if (error != CHIP_NO_ERROR || service == nullptr)
    {
        std::printf("Resolve failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return;
    }

    std::printf("\nResolved Matter service\n");
    std::printf("  Instance:  %s\n", service->mName);
    std::printf("  Host:      %s.local\n", service->mHostName);
    std::printf("  Port:      %u\n", static_cast<unsigned>(service->mPort));
    std::printf("  Interface: %lu\n", static_cast<unsigned long>(service->mInterface.GetInterfaceIndex()));

    for (size_t i = 0; i < addresses.size(); ++i)
    {
        PrintAddress(addresses[i]);
    }
    for (size_t i = 0; i < service->mTextEntrySize; ++i)
    {
        const TextEntry & entry = service->mTextEntries[i];
        std::printf("  TXT %s=", entry.mKey);
        PrintTxtValue(entry.mData, entry.mDataSize);
        std::printf("\n");
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->resolvedCount;
    }
    state->condition.notify_all();
}

void OnBrowse(void * context, DnssdService * services, size_t serviceCount, bool finalBrowse, CHIP_ERROR error)
{
    auto * state = static_cast<DiscoveryState *>(context);

    if (error == CHIP_NO_ERROR)
    {
        for (size_t i = 0; i < serviceCount; ++i)
        {
            std::printf("Found %s.%s on interface %lu; resolving...\n", services[i].mName, services[i].mType,
                        static_cast<unsigned long>(services[i].mInterface.GetInterfaceIndex()));
            CHIP_ERROR resolveError = ChipDnssdResolve(&services[i], services[i].mInterface, OnResolve, state);
            if (resolveError != CHIP_NO_ERROR)
            {
                std::printf("Unable to start resolve: %" CHIP_ERROR_FORMAT "\n", resolveError.Format());
            }
        }
    }

    if (finalBrowse || error != CHIP_NO_ERROR)
    {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->browseFinal = true;
        }
        state->condition.notify_all();
    }
}

void OnDnssdInit(void * context, CHIP_ERROR error)
{
    *static_cast<CHIP_ERROR *>(context) = error;
}

bool ParseTimeout(const char * argument, unsigned & timeoutSeconds)
{
    char * end        = nullptr;
    const long parsed = std::strtol(argument, &end, 10);
    if (end == argument || *end != '\0' || parsed < 1 || parsed > 300)
    {
        return false;
    }
    timeoutSeconds = static_cast<unsigned>(parsed);
    return true;
}

} // namespace

int main(int argc, char * argv[])
{
    unsigned timeoutSeconds = 15;
    if (argc > 2 || (argc == 2 && !ParseTimeout(argv[1], timeoutSeconds)))
    {
        std::fprintf(stderr, "Usage: %s [timeout-seconds: 1-300]\n", argv[0]);
        return 1;
    }

    CHIP_ERROR error = PlatformMgr().InitChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Platform initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }

    error = PlatformMgr().StartEventLoopTask();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Event-loop startup failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().Shutdown();
        return 1;
    }

    CHIP_ERROR initError = CHIP_ERROR_INTERNAL;
    error                = ChipDnssdInit(OnDnssdInit, nullptr, &initError);
    if (error == CHIP_NO_ERROR)
    {
        error = initError;
    }
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "DNS-SD initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }

    DiscoveryState state;
    error = ChipDnssdBrowse(kCommissionableServiceName, DnssdServiceProtocol::kDnssdProtocolUdp, Inet::IPAddressType::kAny,
                            Inet::InterfaceId::Null(), OnBrowse, &state, &state.browseId);
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Commissionable-node browse failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        ChipDnssdShutdown();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }

    std::printf("Browsing for Matter commissionable devices for %u seconds...\n", timeoutSeconds);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait_for(lock, std::chrono::seconds(timeoutSeconds),
                                 [&state]() { return state.resolvedCount > 0 || state.browseFinal; });
    }

    CHIP_ERROR stopError = ChipDnssdStopBrowse(state.browseId);
    if (stopError != CHIP_NO_ERROR && stopError != CHIP_ERROR_KEY_NOT_FOUND)
    {
        std::fprintf(stderr, "Unable to stop browse: %" CHIP_ERROR_FORMAT "\n", stopError.Format());
    }

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait_for(lock, std::chrono::seconds(5), [&state]() { return state.browseFinal; });
    }

    ChipDnssdShutdown();
    (void) PlatformMgr().StopEventLoopTask();
    PlatformMgr().Shutdown();

    if (state.resolvedCount == 0)
    {
        std::printf("No commissionable Matter device resolved. Open the device's commissioning window and check the LAN/firewall.\n");
        return 2;
    }

    std::printf("\nResolved %zu commissionable Matter service(s).\n", state.resolvedCount);
    return 0;
}
