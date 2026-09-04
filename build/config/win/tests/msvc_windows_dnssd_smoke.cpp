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

// Focused runtime smoke for the native Windows DNS-SD backend
// (src/platform/Windows/DnssdImpl.cpp). Covers:
//   - deterministic conversion/mapping seams (UTF-8/UTF-16, status mapping,
//     interface-index round trip, service-type construction) that never touch
//     the network;
//   - synchronous validation of every chip::Dnssd entry point;
//   - the init/shutdown/reinit lifecycle;
//   - live but deterministic use of the real Windows DNS-SD APIs: publishing
//     a uniquely named (GUID) service and observing the success callback,
//     and a browse/StopBrowse cycle that must deliver exactly one
//     finalBrowse=true callback.
//
// It deliberately does not assert that browse/resolve discover any published
// service: multicast delivery on the host running this smoke (firewall
// profile, adapter state, self-resolve loopback suppression) is not
// guaranteed, and asserting on it would make this test flaky. See
// docs/guides/windows.md for the known limitations this informs.
//
// Returns 0 on success, 1 on any failure.

#include <lib/dnssd/platform/Dnssd.h>
#include <platform/PlatformManager.h>
#include <platform/Windows/DnssdImpl.h>

#include <inet/InetInterface.h>
#include <inet/IPAddress.h>
#include <lib/support/Span.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>

using namespace chip;
using namespace chip::Dnssd;
using namespace chip::DeviceLayer;
using namespace std::chrono_literals;

namespace {

int gChecks = 0;

#define CHECK(condition)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        ++gChecks;                                                                                                                 \
        if (!(condition))                                                                                                          \
        {                                                                                                                          \
            std::printf("Windows DNS-SD smoke failed at check %d: %s\n", gChecks, #condition);                                    \
            return 1;                                                                                                              \
        }                                                                                                                          \
    } while (0)

struct Signal
{
    std::mutex mutex;
    std::condition_variable cond;
    bool fired = false;

    void Fire()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            fired = true;
        }
        cond.notify_all();
    }

    bool Wait(std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cond.wait_for(lock, timeout, [this]() { return fired; });
    }
};

std::string MakeGuidInstanceName()
{
    GUID guid = {};
    CoCreateGuid(&guid);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%08lX%04X%04X", static_cast<unsigned long>(guid.Data1),
                  static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3));
    return std::string(buffer);
}

std::atomic<int> gInitCalls{ 0 };
std::atomic<bool> gInitSucceeded{ false };

void OnInit(void * context, CHIP_ERROR error)
{
    gInitCalls++;
    gInitSucceeded = (error == CHIP_NO_ERROR);
    reinterpret_cast<Signal *>(context)->Fire();
}

void OnAsyncError(void *, CHIP_ERROR) {}

struct PublishResult
{
    Signal signal;
    std::atomic<int> calls{ 0 };
    CHIP_ERROR error = CHIP_NO_ERROR;
    std::string type;
    std::string instanceName;
};

void OnPublish(void * context, const char * type, const char * instanceName, CHIP_ERROR error)
{
    auto * result  = reinterpret_cast<PublishResult *>(context);
    result->calls++;
    result->error = error;
    if (type != nullptr)
    {
        result->type = type;
    }
    if (instanceName != nullptr)
    {
        result->instanceName = instanceName;
    }
    result->signal.Fire();
}

struct BrowseResult
{
    Signal finalSignal;
    std::atomic<int> nonFinalCalls{ 0 };
    std::atomic<int> finalCalls{ 0 };
    CHIP_ERROR finalError = CHIP_NO_ERROR;
};

void OnBrowse(void * context, DnssdService *, size_t, bool finalBrowse, CHIP_ERROR error)
{
    auto * result = reinterpret_cast<BrowseResult *>(context);
    if (finalBrowse)
    {
        result->finalCalls++;
        result->finalError = error;
        result->finalSignal.Fire();
    }
    else
    {
        result->nonFinalCalls++;
    }
}

struct ResolveResult
{
    std::atomic<int> calls{ 0 };
};

void OnResolve(void * context, DnssdService *, const Span<Inet::IPAddress> &, CHIP_ERROR)
{
    reinterpret_cast<ResolveResult *>(context)->calls++;
}

} // namespace

int main()
{
    // ---- Deterministic conversion/mapping seams; no network involved. ----
    CHECK(Windows::MakeFullServiceType("_matterc", DnssdServiceProtocol::kDnssdProtocolUdp) == "_matterc._udp");
    CHECK(Windows::MakeFullServiceType("_matterc", DnssdServiceProtocol::kDnssdProtocolTcp) == "_matterc._tcp");
    CHECK(Windows::GetBaseServiceType("_matterc") == "_matterc");
    CHECK(Windows::GetBaseServiceType("_L840._sub._matterc") == "_matterc");
    CHECK(Windows::GetBaseServiceType("_S15._sub._matter") == "_matter");

    std::wstring wide;
    CHECK(Windows::Utf8ToWide("hello-01", wide) == CHIP_NO_ERROR);
    CHECK(wide == L"hello-01");
    std::string narrow;
    CHECK(Windows::WideToUtf8(wide.c_str(), narrow) == CHIP_NO_ERROR);
    CHECK(narrow == "hello-01");
    // A lone UTF-8 continuation byte is not well-formed and must be rejected.
    const char invalidUtf8[] = { static_cast<char>(0x80), '\0' };
    CHECK(Windows::Utf8ToWide(invalidUtf8, wide) != CHIP_NO_ERROR);
    CHECK(Windows::Utf8ToWide(nullptr, wide) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(Windows::WideToUtf8(nullptr, narrow) == CHIP_ERROR_INVALID_ARGUMENT);

    CHECK(Windows::MapServiceStatus(ERROR_SUCCESS) == CHIP_NO_ERROR);
    CHECK(Windows::MapServiceStatus(ERROR_CANCELLED) == CHIP_ERROR_CANCELLED);
    CHECK(Windows::MapServiceStatus(ERROR_ACCESS_DENIED) != CHIP_NO_ERROR);
    CHECK(Windows::MapServiceStatus(ERROR_ACCESS_DENIED) != CHIP_ERROR_CANCELLED);

    CHECK(!Windows::InterfaceIdFromIndex(0).IsPresent());
    {
        bool foundLiveInterface = false;
        for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
        {
            const uint32_t index = iterator.GetInterfaceId().GetInterfaceIndex();
            if (index == 0)
            {
                continue;
            }
            foundLiveInterface   = true;
            const Inet::InterfaceId roundTripped = Windows::InterfaceIdFromIndex(index);
            CHECK(roundTripped.IsPresent());
            CHECK(roundTripped.GetInterfaceIndex() == index);
            break;
        }
        (void) foundLiveInterface; // Absence of any live interface is a valid (if unusual) host state.
    }

    // ---- Synchronous validation, before Init(). ----
    CHECK(ChipDnssdReconfirmRecord(nullptr, Inet::IPAddress::Any, Inet::InterfaceId::Null()) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(ChipDnssdReconfirmRecord("host", Inet::IPAddress::Any, Inet::InterfaceId::Null()) == CHIP_ERROR_NOT_IMPLEMENTED);
    CHECK(ChipDnssdFinalizeServiceUpdate() == CHIP_NO_ERROR);
    CHECK(ChipDnssdPublishService(nullptr, nullptr, nullptr) == CHIP_ERROR_INVALID_ARGUMENT);
    intptr_t browseId = 0;
    CHECK(ChipDnssdBrowse(nullptr, DnssdServiceProtocol::kDnssdProtocolUdp, Inet::IPAddressType::kAny, Inet::InterfaceId::Null(),
                          OnBrowse, nullptr, &browseId) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(ChipDnssdResolve(nullptr, Inet::InterfaceId::Null(), OnResolve, nullptr) == CHIP_ERROR_INVALID_ARGUMENT);

    // ---- Init requires the event loop running so async completions can be
    // marshalled via PlatformMgr().ScheduleWork(). ----
    CHECK(PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    CHECK(PlatformMgr().StartEventLoopTask() == CHIP_NO_ERROR);

    Signal initSignal;
    CHECK(ChipDnssdInit(OnInit, OnAsyncError, &initSignal) == CHIP_NO_ERROR);
    CHECK(gInitCalls.load() == 1); // Init completes synchronously today.
    CHECK(gInitSucceeded.load());

    // ---- Operations before a valid Init are rejected; after Init they must
    // validate arguments the same way. ----
    DnssdService badService{};
    CHECK(ChipDnssdPublishService(&badService, nullptr, nullptr) == CHIP_ERROR_INVALID_ARGUMENT); // empty name/type

    DnssdService txtService{};
    std::strncpy(txtService.mName, MakeGuidInstanceName().c_str(), sizeof(txtService.mName) - 1);
    std::strncpy(txtService.mType, "_matterc", sizeof(txtService.mType) - 1);
    txtService.mProtocol = DnssdServiceProtocol::kDnssdProtocolUdp;
    txtService.mPort     = 5540;
    const uint8_t embeddedNul[] = { 'a', 0x00, 'b' };
    TextEntry badEntry{ "CM", embeddedNul, sizeof(embeddedNul) };
    txtService.mTextEntries   = &badEntry;
    txtService.mTextEntrySize = 1;
    CHECK(ChipDnssdPublishService(&txtService, nullptr, nullptr) == CHIP_ERROR_INVALID_ARGUMENT);

    DnssdService unknownProtoService = txtService;
    unknownProtoService.mTextEntrySize = 0;
    unknownProtoService.mProtocol      = DnssdServiceProtocol::kDnssdProtocolUnknown;
    CHECK(ChipDnssdPublishService(&unknownProtoService, nullptr, nullptr) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(ChipDnssdBrowse("_matterc", DnssdServiceProtocol::kDnssdProtocolUnknown, Inet::IPAddressType::kAny,
                          Inet::InterfaceId::Null(), OnBrowse, nullptr, &browseId) == CHIP_ERROR_INVALID_ARGUMENT);

    // ---- Live publish: a unique GUID instance under "_matterc._udp" must
    // report success with the base type and instance name. ----
    const std::string instanceName = MakeGuidInstanceName();
    DnssdService publishService{};
    std::strncpy(publishService.mName, instanceName.c_str(), sizeof(publishService.mName) - 1);
    std::strncpy(publishService.mType, "_matterc", sizeof(publishService.mType) - 1);
    publishService.mProtocol = DnssdServiceProtocol::kDnssdProtocolUdp;
    publishService.mPort     = 5540;
    const uint8_t cmValue[]  = { '1' };
    TextEntry cmEntry{ "CM", cmValue, sizeof(cmValue) };
    publishService.mTextEntries   = &cmEntry;
    publishService.mTextEntrySize = 1;

    PublishResult publishResult;
    CHECK(ChipDnssdPublishService(&publishService, OnPublish, &publishResult) == CHIP_NO_ERROR);
    CHECK(publishResult.signal.Wait(10s));
    CHECK(publishResult.calls.load() == 1);
    CHECK(publishResult.error == CHIP_NO_ERROR);
    CHECK(publishResult.type == "_matterc._udp");
    CHECK(publishResult.instanceName == instanceName);

    // A normal Matter advertisement refresh removes and immediately
    // republishes the same instance. The backend must wait for asynchronous
    // deregistration before issuing the replacement registration.
    CHECK(ChipDnssdRemoveServices() == CHIP_NO_ERROR);
    PublishResult refreshedPublish;
    CHECK(ChipDnssdPublishService(&publishService, OnPublish, &refreshedPublish) == CHIP_NO_ERROR);
    CHECK(refreshedPublish.signal.Wait(10s));
    CHECK(refreshedPublish.calls.load() == 1);
    CHECK(refreshedPublish.error == CHIP_NO_ERROR);

    // ---- Browse + StopBrowse: exactly one finalBrowse=true callback, ever,
    // regardless of whether any PTR records were actually observed. ----
    BrowseResult browseResult;
    intptr_t liveBrowseId = 0;
    CHECK(ChipDnssdBrowse("_matterc", DnssdServiceProtocol::kDnssdProtocolUdp, Inet::IPAddressType::kAny, Inet::InterfaceId::Null(),
                          OnBrowse, &browseResult, &liveBrowseId) == CHIP_NO_ERROR);
    std::this_thread::sleep_for(500ms);
    CHECK(ChipDnssdStopBrowse(liveBrowseId) == CHIP_NO_ERROR);
    CHECK(browseResult.finalSignal.Wait(10s));
    CHECK(browseResult.finalCalls.load() == 1);
    // Give any (unexpected) duplicate delivery a chance to arrive before
    // asserting there was exactly one.
    std::this_thread::sleep_for(200ms);
    CHECK(browseResult.finalCalls.load() == 1);
    // Stopping an already-finished browse identifier is a caller error.
    CHECK(ChipDnssdStopBrowse(liveBrowseId) == CHIP_ERROR_KEY_NOT_FOUND);

    // ---- Resolve started then immediately abandoned via
    // ResolveNoLongerNeeded() must never invoke the resolve callback. ----
    DnssdService browseHint{};
    std::strncpy(browseHint.mName, instanceName.c_str(), sizeof(browseHint.mName) - 1);
    std::strncpy(browseHint.mType, "_matterc", sizeof(browseHint.mType) - 1);
    browseHint.mProtocol    = DnssdServiceProtocol::kDnssdProtocolUdp;
    browseHint.mAddressType = Inet::IPAddressType::kAny;

    ResolveResult resolveResult;
    CHECK(ChipDnssdResolve(&browseHint, Inet::InterfaceId::Null(), OnResolve, &resolveResult) == CHIP_NO_ERROR);
    ChipDnssdResolveNoLongerNeeded(instanceName.c_str());
    std::this_thread::sleep_for(500ms);
    CHECK(resolveResult.calls.load() == 0);

    // ---- Clean up the live publish and confirm removal is accepted. ----
    CHECK(ChipDnssdRemoveServices() == CHIP_NO_ERROR);

    // ---- Shutdown must reject further operations; re-Init must recover. ----
    ChipDnssdShutdown();
    CHECK(ChipDnssdRemoveServices() == CHIP_ERROR_INCORRECT_STATE);
    CHECK(ChipDnssdPublishService(&publishService, nullptr, nullptr) == CHIP_ERROR_INCORRECT_STATE);
    CHECK(ChipDnssdBrowse("_matterc", DnssdServiceProtocol::kDnssdProtocolUdp, Inet::IPAddressType::kAny,
                          Inet::InterfaceId::Null(), OnBrowse, nullptr, &browseId) == CHIP_ERROR_INCORRECT_STATE);
    CHECK(ChipDnssdResolve(&browseHint, Inet::InterfaceId::Null(), OnResolve, nullptr) == CHIP_ERROR_INCORRECT_STATE);

    Signal reinitSignal;
    CHECK(ChipDnssdInit(OnInit, OnAsyncError, &reinitSignal) == CHIP_NO_ERROR);
    CHECK(gInitCalls.load() == 2);
    PublishResult secondPublish;
    CHECK(ChipDnssdPublishService(&publishService, OnPublish, &secondPublish) == CHIP_NO_ERROR);
    CHECK(secondPublish.signal.Wait(10s));
    CHECK(secondPublish.calls.load() == 1);
    CHECK(secondPublish.error == CHIP_NO_ERROR);
    CHECK(ChipDnssdRemoveServices() == CHIP_NO_ERROR);
    ChipDnssdShutdown();

    (void) PlatformMgr().StopEventLoopTask();
    PlatformMgr().Shutdown();

    std::printf("Windows DNS-SD smoke passed (%d checks)\n", gChecks);
    return 0;
}
