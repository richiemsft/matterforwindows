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

// Phase 3 native Windows controller-side discovery acceptance tool.
//
// This is not chip-tool and does not commission a real device (BLE is out of
// scope for this milestone and no real Matter accessory is available on this
// host). It proves, deterministically and against real upstream controller
// code, the part of the Phase 3 "Device Layer and IP" exit criterion that is
// achievable without a peer device:
//
//   1. Discovery start/stop through the *real* controller-facing contract:
//      chip::Dnssd::DiscoveryImplPlatform (implementing chip::Dnssd::Resolver
//      and chip::Dnssd::ServiceAdvertiser), the exact class every other
//      chip_mdns_platform platform's controller links, layered on this port's
//      native DnssdImpl.cpp Win32 windns.h backend. It is not a hand-rolled
//      substitute for the controller's DNS-SD path.
// Exit codes: 0 if a commissionable node was resolved, 2 if discovery
// start/stop passed but no node was resolved within the timeout, and 1 for
// any other failure.

#include <lib/dnssd/Discovery_ImplPlatform.h>
#include <platform/PlatformManager.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace chip;
using namespace chip::DeviceLayer;
using namespace chip::Dnssd;

namespace {

struct DiscoveryState
{
    std::mutex mutex;
    std::condition_variable condition;
    size_t discoveredCount = 0;
};

class Delegate : public DiscoverNodeDelegate
{
public:
    explicit Delegate(DiscoveryState & state) : mState(state) {}

    void OnNodeDiscovered(const DiscoveredNodeData & nodeData) override
    {
        if (!nodeData.Is<CommissionNodeData>())
        {
            return;
        }
        const CommissionNodeData & data = nodeData.Get<CommissionNodeData>();
        std::printf("\nDiscovered commissionable node\n");
        std::printf("  Instance:   %s\n", data.instanceName);
        std::printf("  Vendor ID:  %u\n", data.vendorId);
        std::printf("  Product ID: %u\n", data.productId);
        std::printf("  Port:       %u\n", static_cast<unsigned>(data.port));

        {
            std::lock_guard<std::mutex> lock(mState.mutex);
            ++mState.discoveredCount;
        }
        mState.condition.notify_all();
    }

private:
    DiscoveryState & mState;
};

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

    // Real controller-facing DNS-SD discovery start/stop, via
    // chip::Dnssd::DiscoveryImplPlatform on top of the native DnssdImpl.cpp
    // backend.
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

    DiscoveryImplPlatform & discovery = DiscoveryImplPlatform::GetInstance();
    PlatformMgr().LockChipStack();
    error = discovery.Init(nullptr);
    PlatformMgr().UnlockChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "DNS-SD initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }

    // Init() only starts the asynchronous native windns.h initialization;
    // IsInitialized() reflects when HandleDnssdInit has actually completed it.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            PlatformMgr().LockChipStack();
            const bool initialized = discovery.IsInitialized();
            PlatformMgr().UnlockChipStack();
            if (initialized)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    PlatformMgr().LockChipStack();
    const bool initialized = discovery.IsInitialized();
    PlatformMgr().UnlockChipStack();
    if (!initialized)
    {
        std::fprintf(stderr, "DNS-SD initialization did not complete in time\n");
        PlatformMgr().LockChipStack();
        discovery.Shutdown();
        PlatformMgr().UnlockChipStack();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }
    std::printf("DNS-SD resolver initialized.\n");

    DiscoveryState state;
    Delegate delegate(state);
    DiscoveryContext context;
    context.SetDiscoveryDelegate(&delegate);

    PlatformMgr().LockChipStack();
    error = discovery.StartDiscovery(DiscoveryType::kCommissionableNode, DiscoveryFilter(), context);
    PlatformMgr().UnlockChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Commissionable-node discovery failed to start: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().LockChipStack();
        discovery.Shutdown();
        PlatformMgr().UnlockChipStack();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }
    std::printf("Browsing for Matter commissionable nodes for %u seconds...\n", timeoutSeconds);

    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait_for(lock, std::chrono::seconds(timeoutSeconds), [&state]() { return state.discoveredCount > 0; });
    }

    PlatformMgr().LockChipStack();
    const bool browseActive = context.GetBrowseIdentifier().has_value();
    PlatformMgr().UnlockChipStack();
    if (!browseActive)
    {
        std::fprintf(stderr, "Commissionable-node browse terminated before it was stopped\n");
        PlatformMgr().LockChipStack();
        discovery.Shutdown();
        PlatformMgr().UnlockChipStack();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }

    PlatformMgr().LockChipStack();
    error = discovery.StopDiscovery(context);
    PlatformMgr().UnlockChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Unable to stop discovery: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().LockChipStack();
        discovery.Shutdown();
        PlatformMgr().UnlockChipStack();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }

    // StartDiscovery retains the context until the final browse callback.
    // Wait for that asynchronous callback to release its reference before
    // destroying the stack-owned context.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            PlatformMgr().LockChipStack();
            const bool released = context.GetReferenceCount() == 1;
            PlatformMgr().UnlockChipStack();
            if (released)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    PlatformMgr().LockChipStack();
    const bool contextReleased = context.GetReferenceCount() == 1;
    PlatformMgr().UnlockChipStack();
    if (!contextReleased)
    {
        std::fprintf(stderr, "Discovery cancellation did not complete in time\n");
        PlatformMgr().LockChipStack();
        discovery.Shutdown();
        PlatformMgr().UnlockChipStack();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().Shutdown();
        return 1;
    }
    std::printf("Discovery stopped.\n");

    PlatformMgr().LockChipStack();
    discovery.Shutdown();
    PlatformMgr().UnlockChipStack();
    (void) PlatformMgr().StopEventLoopTask();
    PlatformMgr().Shutdown();

    const size_t discoveredCount = state.discoveredCount;
    if (discoveredCount == 0)
    {
        std::printf("\nController discovery start/stop passed. No commissionable node was resolved -- open a device's "
                    "commissioning window on this LAN and re-run to exercise a real resolve.\n");
        return 2;
    }

    std::printf("\nController discovery passed. Resolved %zu commissionable node(s).\n", discoveredCount);
    return 0;
}
