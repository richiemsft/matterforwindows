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

/**
 *    @file
 *      Native Windows DNS-SD backend built on the Win32 windns.h service
 *      discovery APIs (DnsServiceRegister/DnsServiceBrowse/DnsServiceResolve
 *      and their DeRegister/Cancel counterparts). This uses the native OS
 *      mDNS responder exclusively: it never binds UDP 5353 and never
 *      implements any part of the mDNS wire protocol itself.
 *
 *      Every windns.h entry point used here is asynchronous: a successful
 *      call returns DNS_REQUEST_PENDING and the outcome is delivered later,
 *      on an arbitrary OS thread, to the completion callback supplied in the
 *      request. Every such callback is marshalled onto PlatformMgr() before
 *      any chip::Dnssd callback is invoked, so no Matter callback ever runs
 *      on a Windows worker thread. Native results are deep-copied (and any
 *      Windows-owned memory freed) before the hop to the event loop.
 *
 *      Publish/Browse/Resolve operations are individually reference-counted
 *      (std::shared_ptr) and additionally kept alive, for exactly as long as
 *      the OS may still call back into them, by a private copy of that
 *      shared_ptr heap-allocated alongside the native request ("the box").
 *      Registration/backend generation counters and registry membership
 *      checks let late or superseded callbacks be recognized as stale and
 *      cleaned up without ever invoking a Matter callback for them.
 */

#include "DnssdImpl.h"

#include <lib/dnssd/ServiceNaming.h>
#include <lib/dnssd/platform/Dnssd.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/Span.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/PlatformManager.h>
#include <system/SystemError.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>

#pragma comment(lib, "dnsapi.lib")

using namespace chip::DeviceLayer;

namespace chip {
namespace Dnssd {
namespace Windows {

std::string MakeFullServiceType(const char * type, DnssdServiceProtocol protocol)
{
    std::string result = (type != nullptr) ? type : "";
    result += ".";
    result += (protocol == DnssdServiceProtocol::kDnssdProtocolTcp) ? kOperationalProtocol : kCommissionProtocol;
    return result;
}

std::string GetBaseServiceType(const char * type)
{
    constexpr char kSubtypeSeparator[] = "._sub.";

    const std::string fullType = (type != nullptr) ? type : "";
    const size_t separator     = fullType.find(kSubtypeSeparator);
    return (separator == std::string::npos) ? fullType : fullType.substr(separator + sizeof(kSubtypeSeparator) - 1);
}

CHIP_ERROR Utf8ToWide(const char * utf8, std::wstring & out)
{
    VerifyOrReturnError(utf8 != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    VerifyOrReturnError(needed > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<wchar_t> buffer(static_cast<size_t>(needed));
    VerifyOrReturnError(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, buffer.data(), needed) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));
    out.assign(buffer.data(), static_cast<size_t>(needed - 1));
    return CHIP_NO_ERROR;
}

CHIP_ERROR WideToUtf8(const wchar_t * wide, std::string & out)
{
    VerifyOrReturnError(wide != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    VerifyOrReturnError(needed > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<char> buffer(static_cast<size_t>(needed));
    VerifyOrReturnError(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, buffer.data(), needed, nullptr, nullptr) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));
    out.assign(buffer.data(), static_cast<size_t>(needed - 1));
    return CHIP_NO_ERROR;
}

CHIP_ERROR MapServiceStatus(unsigned long status)
{
    if (status == ERROR_SUCCESS)
    {
        return CHIP_NO_ERROR;
    }
    if (status == ERROR_CANCELLED)
    {
        return CHIP_ERROR_CANCELLED;
    }
    return CHIP_ERROR_WINDOWS(status);
}

Inet::InterfaceId InterfaceIdFromIndex(uint32_t index)
{
    if (index == 0)
    {
        return Inet::InterfaceId::Null();
    }
    NET_LUID luid;
    if (ConvertInterfaceIndexToLuid(static_cast<NET_IFINDEX>(index), &luid) != NO_ERROR)
    {
        return Inet::InterfaceId::Null();
    }
    return Inet::InterfaceId(luid.Value);
}

} // namespace Windows

namespace {

using Windows::InterfaceIdFromIndex;
using Windows::MakeFullServiceType;
using Windows::GetBaseServiceType;
using Windows::MapServiceStatus;
using Windows::Utf8ToWide;
using Windows::WideToUtf8;

uint32_t IndexFromInterface(Inet::InterfaceId interface)
{
    return interface.IsPresent() ? interface.GetInterfaceIndex() : 0;
}

// ---------------------------------------------------------------------------
// Operation contexts. Each is owned via std::shared_ptr from the backend's
// registry *and* from a private heap-allocated copy ("the box") handed to
// Windows as pQueryContext, so the object outlives whichever owner drops it
// first: registry teardown (Shutdown/supersede) or the native completion.
// ---------------------------------------------------------------------------

struct PublishOp
{
    std::string mKey;          // DNS-SD identity: name|type|protocol|interfaceIndex
    std::string mBaseType;     // "<type>.<protocol>", reported on publish success
    std::string mInstanceName; // reported on publish success

    DnssdPublishCallback mCallback = nullptr;
    void * mContext                = nullptr;
    uint32_t mGeneration            = 0;

    PDNS_SERVICE_INSTANCE mInstance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST mRequest{};
    DNS_SERVICE_CANCEL mCancel{};
    bool mRegisterComplete = false;
    bool mDeregistering    = false;

    ~PublishOp()
    {
        if (mInstance != nullptr)
        {
            DnsServiceFreeInstance(mInstance);
        }
    }
};

struct BrowseOp
{
    intptr_t mId = 0;
    std::string mType;                                              // as requested, e.g. "_matterc"
    std::string mQuerySuffix;                                       // ".<type>.<protocol>.local"
    DnssdServiceProtocol mProtocol    = DnssdServiceProtocol::kDnssdProtocolUnknown;
    Inet::IPAddressType mAddressType  = Inet::IPAddressType::kAny;
    Inet::InterfaceId mInterface      = Inet::InterfaceId::Null();

    DnssdBrowseCallback mCallback = nullptr;
    void * mContext                = nullptr;
    uint32_t mGeneration            = 0;

    std::vector<wchar_t> mQueryNameStorage;
    DNS_SERVICE_BROWSE_REQUEST mRequest{};
    DNS_SERVICE_CANCEL mCancel{};
    std::atomic<bool> mFinalDelivered{ false };
};

struct ResolveOp
{
    std::string mInstanceName;
    std::string mType;
    DnssdServiceProtocol mProtocol = DnssdServiceProtocol::kDnssdProtocolUnknown;
    Inet::IPAddressType mRequestedAddressType = Inet::IPAddressType::kAny;

    DnssdResolveCallback mCallback = nullptr;
    void * mContext                 = nullptr;
    uint32_t mGeneration             = 0;

    std::vector<wchar_t> mQueryNameStorage;
    DNS_SERVICE_RESOLVE_REQUEST mRequest{};
    DNS_SERVICE_CANCEL mCancel{};
};

std::string MakePublishKey(const DnssdService & service, uint32_t interfaceIndex)
{
    return std::string(service.mName) + "|" + std::string(service.mType) + "|" +
        std::to_string(static_cast<int>(service.mProtocol)) + "|" + std::to_string(interfaceIndex);
}

void WINAPI NativeRegisterCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);
void WINAPI NativeBrowseCallback(DWORD status, PVOID context, PDNS_RECORD records);
void WINAPI NativeResolveCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);

// ---------------------------------------------------------------------------
// Delivery payloads. Native callbacks deep-copy everything they need out of
// Windows-owned memory into one of these, free the Windows memory, then post
// the payload's Deliver() function to PlatformMgr().ScheduleWork(). Nothing
// below this point touches a Windows API; it is safe to run entirely on the
// Matter event-loop thread.
// ---------------------------------------------------------------------------

class Backend;

struct PublishDelivery
{
    std::shared_ptr<PublishOp> op;
    CHIP_ERROR error = CHIP_NO_ERROR;

    static void Deliver(intptr_t arg);
};

struct BrowseDelivery
{
    std::shared_ptr<BrowseOp> op;
    std::vector<DnssdService> services; // self-contained (no TextEntries/SubTypes set)
    bool finalBrowse = false;
    CHIP_ERROR error  = CHIP_NO_ERROR;

    static void Deliver(intptr_t arg);
};

struct ResolveDelivery
{
    struct TextItem
    {
        std::string key;
        std::vector<uint8_t> value;
    };

    std::shared_ptr<ResolveOp> op;
    CHIP_ERROR error = CHIP_NO_ERROR;

    std::string hostName;
    uint16_t port            = 0;
    uint32_t interfaceIndex  = 0;
    std::vector<Inet::IPAddress> addresses;
    std::vector<TextItem> textItems;

    static void Deliver(intptr_t arg);
};

class Backend
{
public:
    static Backend & Instance()
    {
        static Backend backend;
        return backend;
    }

    CHIP_ERROR Init(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback errorCallback, void * context);
    void Shutdown();
    CHIP_ERROR RemoveServices();
    CHIP_ERROR PublishService(const DnssdService * service, DnssdPublishCallback callback, void * context);
    CHIP_ERROR FinalizeServiceUpdate();
    CHIP_ERROR Browse(const char * type, DnssdServiceProtocol protocol, Inet::IPAddressType addressType,
                       Inet::InterfaceId interface, DnssdBrowseCallback callback, void * context, intptr_t * browseIdentifier);
    CHIP_ERROR StopBrowse(intptr_t browseIdentifier);
    CHIP_ERROR Resolve(DnssdService * browseResult, Inet::InterfaceId interface, DnssdResolveCallback callback, void * context);
    void ResolveNoLongerNeeded(const char * instanceName);
    CHIP_ERROR ReconfirmRecord(const char * hostname, Inet::IPAddress address, Inet::InterfaceId interface);

    void DeliverPublish(std::unique_ptr<PublishDelivery> delivery);
    void DeliverBrowse(std::unique_ptr<BrowseDelivery> delivery);
    void DeliverResolve(std::unique_ptr<ResolveDelivery> delivery);

private:
    Backend() = default;

    bool IsCurrentLocked(uint32_t generation) const { return mInitialized && generation == mGeneration; }
    void SupersedePublish(const std::shared_ptr<PublishOp> & op);
    CHIP_ERROR IssueRegister(const std::shared_ptr<PublishOp> & op);
    void IssueDeregister(const std::shared_ptr<PublishOp> & op);
    void CompletePublishRetirement(const std::shared_ptr<PublishOp> & op, CHIP_ERROR error);
    void FinalizeBrowseNow(const std::shared_ptr<BrowseOp> & op, CHIP_ERROR error);

    std::mutex mMutex;
    bool mInitialized        = false;
    uint32_t mGeneration     = 0;
    intptr_t mNextBrowseId   = 1;

    std::unordered_map<std::string, std::shared_ptr<PublishOp>> mPublishOps;
    std::unordered_map<std::string, std::shared_ptr<PublishOp>> mRetiringPublishOps;
    std::unordered_map<std::string, std::shared_ptr<PublishOp>> mPendingPublishOps;
    std::unordered_map<intptr_t, std::shared_ptr<BrowseOp>> mBrowseOps;
    std::vector<std::shared_ptr<ResolveOp>> mResolveOps;
};

void PublishDelivery::Deliver(intptr_t arg)
{
    std::unique_ptr<PublishDelivery> self(reinterpret_cast<PublishDelivery *>(arg));
    Backend::Instance().DeliverPublish(std::move(self));
}

void BrowseDelivery::Deliver(intptr_t arg)
{
    std::unique_ptr<BrowseDelivery> self(reinterpret_cast<BrowseDelivery *>(arg));
    Backend::Instance().DeliverBrowse(std::move(self));
}

void ResolveDelivery::Deliver(intptr_t arg)
{
    std::unique_ptr<ResolveDelivery> self(reinterpret_cast<ResolveDelivery *>(arg));
    Backend::Instance().DeliverResolve(std::move(self));
}

template <typename Payload>
void PostOrDrop(void (*deliver)(intptr_t), std::unique_ptr<Payload> payload)
{
    Payload * raw = payload.release();
    if (PlatformMgr().ScheduleWork(deliver, reinterpret_cast<intptr_t>(raw)) != CHIP_NO_ERROR)
    {
        // The event loop cannot accept work (e.g. shutting down); dropping the
        // delivery is safe because Shutdown() already invalidates the
        // originating operation's generation, so it would have been treated as
        // stale and discarded without a Matter callback in any case.
        delete raw;
    }
}

// ---------------------------------------------------------------------------
// Native (arbitrary-thread) trampolines.
// ---------------------------------------------------------------------------

void WINAPI NativeRegisterCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    std::unique_ptr<std::shared_ptr<PublishOp>> box(static_cast<std::shared_ptr<PublishOp> *>(context));
    auto delivery  = std::make_unique<PublishDelivery>();
    delivery->op    = *box;
    delivery->error = MapServiceStatus(status);

    if (instance != nullptr)
    {
        DnsServiceFreeInstance(instance);
    }

    PostOrDrop(&PublishDelivery::Deliver, std::move(delivery));
}

void WINAPI NativeBrowseCallback(DWORD status, PVOID context, PDNS_RECORD records)
{
    auto * box                   = static_cast<std::shared_ptr<BrowseOp> *>(context);
    std::shared_ptr<BrowseOp> op = *box;

    auto delivery         = std::make_unique<BrowseDelivery>();
    delivery->op           = op;
    delivery->error        = MapServiceStatus(status);
    delivery->finalBrowse  = (delivery->error != CHIP_NO_ERROR);

    if (records != nullptr)
    {
        for (PDNS_RECORD record = records; record != nullptr; record = record->pNext)
        {
            if (record->wType != DNS_TYPE_PTR || record->dwTtl == 0)
            {
                // Only PTR records describe discovered instances; a TTL of 0 is
                // a goodbye/removal record, which this backend does not surface
                // as a distinct browse event.
                continue;
            }
            if (record->Data.PTR.pNameHost == nullptr)
            {
                continue;
            }
            std::string target;
            if (WideToUtf8(record->Data.PTR.pNameHost, target) != CHIP_NO_ERROR)
            {
                continue;
            }
            const std::string & suffix = op->mQuerySuffix;
            if (target.size() <= suffix.size() ||
                _stricmp(target.c_str() + (target.size() - suffix.size()), suffix.c_str()) != 0)
            {
                continue; // not a PTR under the type/protocol/domain we queried for
            }

            DnssdService service{};
            const std::string instanceName = target.substr(0, target.size() - suffix.size());
            Platform::CopyString(service.mName, instanceName.c_str());
            Platform::CopyString(service.mType, op->mType.c_str());
            service.mProtocol      = op->mProtocol;
            service.mAddressType   = op->mAddressType;
            service.mTransportType = op->mAddressType;
            service.mInterface     = op->mInterface;
            delivery->services.push_back(service);
        }
        DnsRecordListFree(records, DnsFreeRecordList);
    }

    const bool final = delivery->finalBrowse;
    PostOrDrop(&BrowseDelivery::Deliver, std::move(delivery));

    if (final)
    {
        // No further callbacks will arrive for this browse; safe to free the box.
        delete box;
    }
}

void WINAPI NativeResolveCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    std::unique_ptr<std::shared_ptr<ResolveOp>> box(static_cast<std::shared_ptr<ResolveOp> *>(context));
    std::shared_ptr<ResolveOp> & op = *box;

    auto delivery  = std::make_unique<ResolveDelivery>();
    delivery->op    = op;
    delivery->error = MapServiceStatus(status);

    if (delivery->error == CHIP_NO_ERROR && instance == nullptr)
    {
        delivery->error = CHIP_ERROR_INTERNAL;
    }
    else if (delivery->error == CHIP_NO_ERROR)
    {
        std::string host;
        if (instance->pszHostName != nullptr && WideToUtf8(instance->pszHostName, host) == CHIP_NO_ERROR)
        {
            const size_t dot   = host.find('.');
            delivery->hostName = (dot == std::string::npos) ? host : host.substr(0, dot);
        }
        delivery->port            = instance->wPort;
        delivery->interfaceIndex  = instance->dwInterfaceIndex;

        const Inet::IPAddressType requested = op->mRequestedAddressType;
        const bool wantV4 = requested == Inet::IPAddressType::kIPv4 || requested == Inet::IPAddressType::kAny ||
            requested == Inet::IPAddressType::kUnknown;
        const bool wantV6 = requested == Inet::IPAddressType::kIPv6 || requested == Inet::IPAddressType::kAny ||
            requested == Inet::IPAddressType::kUnknown;

        if (wantV4 && instance->ip4Address != nullptr)
        {
            struct in_addr addr;
            addr.s_addr = *instance->ip4Address;
            delivery->addresses.push_back(Inet::IPAddress(addr));
        }
        if (wantV6 && instance->ip6Address != nullptr)
        {
            // IP6_ADDRESS is a union of raw byte/word/qword views; the typed
            // `In6` alternative is only compiled in when the (never-defined-as-
            // a-macro) IN6_ADDR guard is set, so copy the 16 address bytes
            // directly instead of relying on that member.
            struct in6_addr addr6;
            static_assert(sizeof(addr6) == sizeof(instance->ip6Address->IP6Byte), "unexpected IPv6 address size");
            memcpy(&addr6, instance->ip6Address->IP6Byte, sizeof(addr6));
            delivery->addresses.push_back(Inet::IPAddress(addr6));
        }

        for (DWORD i = 0; i < instance->dwPropertyCount; ++i)
        {
            if (instance->keys == nullptr || instance->keys[i] == nullptr)
            {
                continue;
            }
            std::string key;
            if (WideToUtf8(instance->keys[i], key) != CHIP_NO_ERROR)
            {
                continue;
            }
            ResolveDelivery::TextItem item;
            item.key = std::move(key);
            if (instance->values != nullptr && instance->values[i] != nullptr)
            {
                std::string value;
                if (WideToUtf8(instance->values[i], value) == CHIP_NO_ERROR)
                {
                    item.value.assign(value.begin(), value.end());
                }
            }
            delivery->textItems.push_back(std::move(item));
        }
    }

    if (instance != nullptr)
    {
        DnsServiceFreeInstance(instance);
    }

    PostOrDrop(&ResolveDelivery::Deliver, std::move(delivery));
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

CHIP_ERROR Backend::Init(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback /* errorCallback */, void * context)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mInitialized = true;
        ++mGeneration;
    }
    if (initCallback != nullptr)
    {
        initCallback(context, CHIP_NO_ERROR);
    }
    return CHIP_NO_ERROR;
}

void Backend::Shutdown()
{
    std::vector<std::shared_ptr<PublishOp>> publishOps;
    std::vector<std::shared_ptr<BrowseOp>> browseOps;
    std::vector<std::shared_ptr<ResolveOp>> resolveOps;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mInitialized)
        {
            return;
        }
        mInitialized = false;
        ++mGeneration;

        for (auto & entry : mPublishOps)
        {
            publishOps.push_back(entry.second);
            mRetiringPublishOps[entry.first] = entry.second;
        }
        mPublishOps.clear();
        mPendingPublishOps.clear();

        for (auto & entry : mBrowseOps)
        {
            browseOps.push_back(entry.second);
        }
        mBrowseOps.clear();

        resolveOps = std::move(mResolveOps);
        mResolveOps.clear();
    }

    for (auto & op : publishOps)
    {
        SupersedePublish(op);
    }
    for (auto & op : browseOps)
    {
        DnsServiceBrowseCancel(&op->mCancel);
    }
    for (auto & op : resolveOps)
    {
        DnsServiceResolveCancel(&op->mCancel);
    }
}

CHIP_ERROR Backend::RemoveServices()
{
    std::vector<std::shared_ptr<PublishOp>> ops;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        VerifyOrReturnError(mInitialized, CHIP_ERROR_INCORRECT_STATE);
        for (auto & entry : mPublishOps)
        {
            ops.push_back(entry.second);
            mRetiringPublishOps[entry.first] = entry.second;
        }
        mPublishOps.clear();
        mPendingPublishOps.clear();
    }
    for (auto & op : ops)
    {
        SupersedePublish(op);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR Backend::FinalizeServiceUpdate()
{
    // Publish/RemoveServices apply immediately (each issues its native call as
    // soon as it is invoked), so there is nothing to finalize.
    return CHIP_NO_ERROR;
}

void Backend::IssueDeregister(const std::shared_ptr<PublishOp> & op)
{
    op->mDeregistering                       = true;
    op->mRequest.pRegisterCompletionCallback = &NativeRegisterCallback;
    auto * box                               = new std::shared_ptr<PublishOp>(op);
    op->mRequest.pQueryContext               = box;

    const DWORD status = DnsServiceDeRegister(&op->mRequest, nullptr);
    if (status != DNS_REQUEST_PENDING)
    {
        delete box;
        CompletePublishRetirement(op, MapServiceStatus(status));
    }
}

void Backend::SupersedePublish(const std::shared_ptr<PublishOp> & op)
{
    if (op->mRegisterComplete)
    {
        IssueDeregister(op);
    }
    else
    {
        // Still pending: cancel it. Windows may still deliver a completion
        // (success or ERROR_CANCELLED); DeliverPublish() will find it is no
        // longer the current op for its key and clean it up without invoking
        // the Matter publish callback.
        DnsServiceRegisterCancel(&op->mCancel);
    }
}

CHIP_ERROR Backend::IssueRegister(const std::shared_ptr<PublishOp> & op)
{
    auto * box                 = new std::shared_ptr<PublishOp>(op);
    op->mRequest.pQueryContext = box;

    const DWORD status = DnsServiceRegister(&op->mRequest, &op->mCancel);
    if (status == DNS_REQUEST_PENDING)
    {
        return CHIP_NO_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mPublishOps.find(op->mKey);
        if (it != mPublishOps.end() && it->second == op)
        {
            mPublishOps.erase(it);
        }
    }
    delete box;
    return MapServiceStatus(status);
}

void Backend::CompletePublishRetirement(const std::shared_ptr<PublishOp> & op, CHIP_ERROR error)
{
    std::shared_ptr<PublishOp> pending;
    bool startPending = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto retiring = mRetiringPublishOps.find(op->mKey);
        if (retiring == mRetiringPublishOps.end() || retiring->second != op)
        {
            return;
        }
        mRetiringPublishOps.erase(retiring);

        auto waiting = mPendingPublishOps.find(op->mKey);
        if (waiting != mPendingPublishOps.end())
        {
            pending = waiting->second;
            mPendingPublishOps.erase(waiting);
            startPending = error == CHIP_NO_ERROR && IsCurrentLocked(pending->mGeneration);
            if (startPending)
            {
                mPublishOps[pending->mKey] = pending;
            }
        }
    }

    if (pending == nullptr)
    {
        return;
    }

    CHIP_ERROR registerError = startPending ? IssueRegister(pending) : error;
    if (!startPending && registerError == CHIP_NO_ERROR)
    {
        registerError = CHIP_ERROR_INCORRECT_STATE;
    }
    if (registerError != CHIP_NO_ERROR && pending->mCallback != nullptr)
    {
        pending->mCallback(pending->mContext, nullptr, nullptr, registerError);
    }
}

CHIP_ERROR Backend::PublishService(const DnssdService * service, DnssdPublishCallback callback, void * context)
{
    VerifyOrReturnError(service != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(service->mName[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(service->mType[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(service->mProtocol == DnssdServiceProtocol::kDnssdProtocolUdp ||
                            service->mProtocol == DnssdServiceProtocol::kDnssdProtocolTcp,
                        CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(service->mTextEntrySize == 0 || service->mTextEntries != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    for (size_t i = 0; i < service->mTextEntrySize; ++i)
    {
        const TextEntry & entry = service->mTextEntries[i];
        VerifyOrReturnError(entry.mKey != nullptr && entry.mKey[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
        VerifyOrReturnError(entry.mDataSize == 0 || entry.mData != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
        // DNS_SERVICE_INSTANCE TXT values are NUL-terminated wide strings; a
        // binary value with an embedded NUL cannot be represented and must be
        // rejected rather than silently truncated.
        VerifyOrReturnError(entry.mDataSize == 0 || memchr(entry.mData, 0, entry.mDataSize) == nullptr,
                            CHIP_ERROR_INVALID_ARGUMENT);
    }

    bool initialized;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        initialized = mInitialized;
        generation  = mGeneration;
    }
    VerifyOrReturnError(initialized, CHIP_ERROR_INCORRECT_STATE);

    const uint32_t interfaceIndex      = IndexFromInterface(service->mInterface);
    const std::string baseType         = MakeFullServiceType(service->mType, service->mProtocol);
    const std::string fullInstanceName = std::string(service->mName) + "." + baseType + "." + kLocalDomain;
    const std::string hostLabel        = (service->mHostName[0] != '\0') ? std::string(service->mHostName) : std::string(service->mName);
    const std::string fullHostName     = hostLabel + "." + kLocalDomain;

    std::wstring instanceNameWide;
    std::wstring hostNameWide;
    ReturnErrorOnFailure(Utf8ToWide(fullInstanceName.c_str(), instanceNameWide));
    ReturnErrorOnFailure(Utf8ToWide(fullHostName.c_str(), hostNameWide));

    std::vector<std::wstring> keyStorage;
    std::vector<std::wstring> valueStorage;
    keyStorage.reserve(service->mTextEntrySize);
    valueStorage.reserve(service->mTextEntrySize);
    for (size_t i = 0; i < service->mTextEntrySize; ++i)
    {
        const TextEntry & entry = service->mTextEntries[i];
        std::wstring key;
        ReturnErrorOnFailure(Utf8ToWide(entry.mKey, key));
        const std::string value(reinterpret_cast<const char *>(entry.mData), entry.mDataSize);
        std::wstring wideValue;
        ReturnErrorOnFailure(Utf8ToWide(value.c_str(), wideValue));
        keyStorage.push_back(std::move(key));
        valueStorage.push_back(std::move(wideValue));
    }
    std::vector<PCWSTR> keyPointers(keyStorage.size());
    std::vector<PCWSTR> valuePointers(valueStorage.size());
    for (size_t i = 0; i < keyStorage.size(); ++i)
    {
        keyPointers[i]   = keyStorage[i].c_str();
        valuePointers[i] = valueStorage[i].c_str();
    }

    IP4_ADDRESS ip4Value = 0;
    IP6_ADDRESS ip6Value{};
    PIP4_ADDRESS ip4Ptr = nullptr;
    PIP6_ADDRESS ip6Ptr = nullptr;
    if (service->mAddress.has_value())
    {
        if (service->mAddress->IsIPv4())
        {
            const struct in_addr addr = service->mAddress->ToIPv4();
            ip4Value                  = addr.s_addr;
            ip4Ptr                    = &ip4Value;
        }
        else if (service->mAddress->IsIPv6())
        {
            const struct in6_addr addr6 = service->mAddress->ToIPv6();
            static_assert(sizeof(addr6) == sizeof(ip6Value.IP6Byte), "unexpected IPv6 address size");
            memcpy(ip6Value.IP6Byte, &addr6, sizeof(addr6));
            ip6Ptr = &ip6Value;
        }
    }

    PDNS_SERVICE_INSTANCE instance =
        DnsServiceConstructInstance(instanceNameWide.c_str(), hostNameWide.c_str(), ip4Ptr, ip6Ptr,
                                    static_cast<WORD>(service->mPort), 0, 0, static_cast<DWORD>(keyPointers.size()),
                                    keyPointers.empty() ? nullptr : keyPointers.data(),
                                    valuePointers.empty() ? nullptr : valuePointers.data());
    VerifyOrReturnError(instance != nullptr, CHIP_ERROR_NO_MEMORY);
    instance->dwInterfaceIndex = interfaceIndex;

    auto op           = std::make_shared<PublishOp>();
    op->mKey          = MakePublishKey(*service, interfaceIndex);
    op->mBaseType     = baseType;
    op->mInstanceName = service->mName;
    op->mCallback     = callback;
    op->mContext      = context;
    op->mGeneration   = generation;
    op->mInstance     = instance;

    op->mRequest.Version                    = DNS_QUERY_REQUEST_VERSION1;
    op->mRequest.InterfaceIndex              = interfaceIndex;
    op->mRequest.pServiceInstance            = instance;
    op->mRequest.pRegisterCompletionCallback = &NativeRegisterCallback;
    op->mRequest.unicastEnabled              = FALSE;

    std::shared_ptr<PublishOp> superseded;
    bool registerNow = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!IsCurrentLocked(generation))
        {
            return CHIP_ERROR_INCORRECT_STATE; // `op` destructs here, freeing `instance`.
        }
        auto it = mPublishOps.find(op->mKey);
        if (it != mPublishOps.end())
        {
            superseded = it->second;
            mPublishOps.erase(it);
            mRetiringPublishOps[op->mKey] = superseded;
            mPendingPublishOps[op->mKey]  = op;
        }
        else if (mRetiringPublishOps.find(op->mKey) != mRetiringPublishOps.end())
        {
            mPendingPublishOps[op->mKey] = op;
        }
        else
        {
            mPublishOps[op->mKey] = op;
            registerNow            = true;
        }
    }
    if (superseded)
    {
        SupersedePublish(superseded);
    }
    return registerNow ? IssueRegister(op) : CHIP_NO_ERROR;
}

void Backend::DeliverPublish(std::unique_ptr<PublishDelivery> delivery)
{
    std::shared_ptr<PublishOp> & op = delivery->op;
    if (op->mDeregistering)
    {
        CompletePublishRetirement(op, delivery->error);
        return;
    }

    bool retiring = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it  = mRetiringPublishOps.find(op->mKey);
        retiring = it != mRetiringPublishOps.end() && it->second == op;
    }
    if (retiring)
    {
        if (delivery->error == CHIP_NO_ERROR)
        {
            op->mRegisterComplete = true;
            IssueDeregister(op);
        }
        else
        {
            CompletePublishRetirement(op, CHIP_NO_ERROR);
        }
        return;
    }

    DnssdPublishCallback callback = nullptr;
    void * context                = nullptr;
    bool stale                    = true;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mPublishOps.find(op->mKey);
        stale   = !(it != mPublishOps.end() && it->second == op && IsCurrentLocked(op->mGeneration));
        if (!stale)
        {
            callback = op->mCallback;
            context  = op->mContext;
            if (delivery->error == CHIP_NO_ERROR)
            {
                op->mRegisterComplete = true;
            }
            else
            {
                mPublishOps.erase(it);
            }
        }
    }

    if (stale)
    {
        // The backend lifecycle changed after registration completed.
        if (delivery->error == CHIP_NO_ERROR)
        {
            op->mRegisterComplete = true;
            IssueDeregister(op);
        }
        return;
    }

    if (callback == nullptr)
    {
        return;
    }
    if (delivery->error == CHIP_NO_ERROR)
    {
        callback(context, op->mBaseType.c_str(), op->mInstanceName.c_str(), CHIP_NO_ERROR);
    }
    else
    {
        callback(context, nullptr, nullptr, delivery->error);
    }
}

CHIP_ERROR Backend::Browse(const char * type, DnssdServiceProtocol protocol, Inet::IPAddressType addressType,
                           Inet::InterfaceId interface, DnssdBrowseCallback callback, void * context,
                           intptr_t * browseIdentifier)
{
    VerifyOrReturnError(type != nullptr && type[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(callback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(browseIdentifier != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(protocol == DnssdServiceProtocol::kDnssdProtocolUdp || protocol == DnssdServiceProtocol::kDnssdProtocolTcp,
                        CHIP_ERROR_INVALID_ARGUMENT);

    bool initialized;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        initialized = mInitialized;
        generation  = mGeneration;
    }
    VerifyOrReturnError(initialized, CHIP_ERROR_INCORRECT_STATE);

    const std::string fullType    = MakeFullServiceType(type, protocol);
    const std::string baseType    = GetBaseServiceType(type);
    const std::string queryName   = fullType + "." + kLocalDomain;
    const std::string baseName    = MakeFullServiceType(baseType.c_str(), protocol) + "." + kLocalDomain;
    std::wstring wideQueryName;
    ReturnErrorOnFailure(Utf8ToWide(queryName.c_str(), wideQueryName));

    auto op          = std::make_shared<BrowseOp>();
    op->mType        = baseType;
    op->mQuerySuffix = "." + baseName;
    op->mProtocol    = protocol;
    op->mAddressType = addressType;
    op->mInterface   = interface;
    op->mCallback    = callback;
    op->mContext     = context;
    op->mGeneration  = generation;
    op->mQueryNameStorage.assign(wideQueryName.begin(), wideQueryName.end());
    op->mQueryNameStorage.push_back(L'\0');

    op->mRequest.Version        = DNS_QUERY_REQUEST_VERSION1;
    op->mRequest.InterfaceIndex = IndexFromInterface(interface);
    op->mRequest.QueryName      = op->mQueryNameStorage.data();
    op->mRequest.pBrowseCallback = &NativeBrowseCallback;

    intptr_t id;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!IsCurrentLocked(generation))
        {
            return CHIP_ERROR_INCORRECT_STATE;
        }
        id             = mNextBrowseId++;
        op->mId        = id;
        mBrowseOps[id] = op;
    }

    auto * box                  = new std::shared_ptr<BrowseOp>(op);
    op->mRequest.pQueryContext  = box;

    const DNS_STATUS status = DnsServiceBrowse(&op->mRequest, &op->mCancel);
    if (status != DNS_REQUEST_PENDING)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mBrowseOps.erase(id);
        }
        delete box;
        return MapServiceStatus(static_cast<unsigned long>(status));
    }

    *browseIdentifier = id;
    return CHIP_NO_ERROR;
}

void Backend::FinalizeBrowseNow(const std::shared_ptr<BrowseOp> & op, CHIP_ERROR error)
{
    bool expected = false;
    if (!op->mFinalDelivered.compare_exchange_strong(expected, true))
    {
        return; // already delivered (raced with the native completion)
    }

    DnssdBrowseCallback callback = nullptr;
    void * context                = nullptr;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mBrowseOps.find(op->mId);
        if (it != mBrowseOps.end() && it->second == op)
        {
            callback = op->mCallback;
            context  = op->mContext;
            mBrowseOps.erase(it);
        }
    }
    if (callback != nullptr)
    {
        callback(context, nullptr, 0, true, error);
    }
}

CHIP_ERROR Backend::StopBrowse(intptr_t browseIdentifier)
{
    std::shared_ptr<BrowseOp> op;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mBrowseOps.find(browseIdentifier);
        VerifyOrReturnError(it != mBrowseOps.end(), CHIP_ERROR_KEY_NOT_FOUND);
        op = it->second;
    }

    const DNS_STATUS status = DnsServiceBrowseCancel(&op->mCancel);
    if (status == ERROR_SUCCESS)
    {
        // Windows will invoke the browse callback once more with a
        // cancellation status; DeliverBrowse() performs the single required
        // final callback from that delivery (guarded by mFinalDelivered).
        return CHIP_NO_ERROR;
    }

    // The cancel call itself failed (operation already completed or otherwise
    // invalid); no cancellation callback will arrive, so synthesize the
    // required final callback ourselves.
    FinalizeBrowseNow(op, MapServiceStatus(static_cast<unsigned long>(status)));
    return CHIP_NO_ERROR;
}

void Backend::DeliverBrowse(std::unique_ptr<BrowseDelivery> delivery)
{
    std::shared_ptr<BrowseOp> & op = delivery->op;
    if (delivery->finalBrowse)
    {
        bool expected = false;
        if (!op->mFinalDelivered.compare_exchange_strong(expected, true))
        {
            return; // already finalized via StopBrowse()'s synchronous fallback
        }
    }

    DnssdBrowseCallback callback = nullptr;
    void * context                = nullptr;
    bool stale;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mBrowseOps.find(op->mId);
        stale   = !(it != mBrowseOps.end() && it->second == op && IsCurrentLocked(op->mGeneration));
        if (!stale)
        {
            callback = op->mCallback;
            context  = op->mContext;
            if (delivery->finalBrowse)
            {
                mBrowseOps.erase(it);
            }
        }
    }
    if (stale || callback == nullptr)
    {
        return;
    }

    if (delivery->services.empty())
    {
        callback(context, nullptr, 0, delivery->finalBrowse, delivery->error);
    }
    else
    {
        callback(context, delivery->services.data(), delivery->services.size(), delivery->finalBrowse, delivery->error);
    }
}

CHIP_ERROR Backend::Resolve(DnssdService * browseResult, Inet::InterfaceId interface, DnssdResolveCallback callback,
                            void * context)
{
    VerifyOrReturnError(browseResult != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(callback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(browseResult->mName[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(browseResult->mType[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(browseResult->mProtocol == DnssdServiceProtocol::kDnssdProtocolUdp ||
                            browseResult->mProtocol == DnssdServiceProtocol::kDnssdProtocolTcp,
                        CHIP_ERROR_INVALID_ARGUMENT);

    bool initialized;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        initialized = mInitialized;
        generation  = mGeneration;
    }
    VerifyOrReturnError(initialized, CHIP_ERROR_INCORRECT_STATE);

    const std::string fullName =
        std::string(browseResult->mName) + "." + MakeFullServiceType(browseResult->mType, browseResult->mProtocol) + "." +
        kLocalDomain;
    std::wstring wideName;
    ReturnErrorOnFailure(Utf8ToWide(fullName.c_str(), wideName));

    auto op                   = std::make_shared<ResolveOp>();
    op->mInstanceName         = browseResult->mName;
    op->mType                 = browseResult->mType;
    op->mProtocol             = browseResult->mProtocol;
    op->mRequestedAddressType = browseResult->mAddressType;
    op->mCallback             = callback;
    op->mContext              = context;
    op->mGeneration           = generation;
    op->mQueryNameStorage.assign(wideName.begin(), wideName.end());
    op->mQueryNameStorage.push_back(L'\0');

    op->mRequest.Version                   = DNS_QUERY_REQUEST_VERSION1;
    op->mRequest.InterfaceIndex             = IndexFromInterface(interface);
    op->mRequest.QueryName                  = op->mQueryNameStorage.data();
    op->mRequest.pResolveCompletionCallback = &NativeResolveCallback;

    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!IsCurrentLocked(generation))
        {
            return CHIP_ERROR_INCORRECT_STATE;
        }
        mResolveOps.push_back(op);
    }

    auto * box                 = new std::shared_ptr<ResolveOp>(op);
    op->mRequest.pQueryContext = box;

    const DNS_STATUS status = DnsServiceResolve(&op->mRequest, &op->mCancel);
    if (status != DNS_REQUEST_PENDING)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto it = std::find(mResolveOps.begin(), mResolveOps.end(), op);
            if (it != mResolveOps.end())
            {
                mResolveOps.erase(it);
            }
        }
        delete box;
        return MapServiceStatus(static_cast<unsigned long>(status));
    }
    return CHIP_NO_ERROR;
}

void Backend::DeliverResolve(std::unique_ptr<ResolveDelivery> delivery)
{
    std::shared_ptr<ResolveOp> & op = delivery->op;
    DnssdResolveCallback callback    = nullptr;
    void * context                  = nullptr;
    bool stale;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = std::find(mResolveOps.begin(), mResolveOps.end(), op);
        stale   = !(it != mResolveOps.end() && IsCurrentLocked(op->mGeneration));
        if (!stale)
        {
            callback = op->mCallback;
            context  = op->mContext;
            mResolveOps.erase(it); // resolve is single-shot: this delivery is always terminal
        }
    }
    if (stale || callback == nullptr)
    {
        return;
    }

    if (delivery->error != CHIP_NO_ERROR)
    {
        callback(context, nullptr, Span<Inet::IPAddress>(), delivery->error);
        return;
    }

    DnssdService result{};
    Platform::CopyString(result.mName, op->mInstanceName.c_str());
    Platform::CopyString(result.mType, op->mType.c_str());
    Platform::CopyString(result.mHostName, delivery->hostName.c_str());
    result.mProtocol      = op->mProtocol;
    result.mPort          = delivery->port;
    result.mInterface     = InterfaceIdFromIndex(delivery->interfaceIndex);
    result.mAddressType   = op->mRequestedAddressType;
    result.mTransportType = op->mRequestedAddressType;

    std::vector<TextEntry> textEntries;
    textEntries.reserve(delivery->textItems.size());
    for (auto & item : delivery->textItems)
    {
        TextEntry entry;
        entry.mKey      = item.key.c_str();
        entry.mData     = item.value.empty() ? nullptr : item.value.data();
        entry.mDataSize = item.value.size();
        textEntries.push_back(entry);
    }
    if (!textEntries.empty())
    {
        result.mTextEntries = textEntries.data();
    }
    result.mTextEntrySize = textEntries.size();

    callback(context, &result, Span<Inet::IPAddress>(delivery->addresses.data(), delivery->addresses.size()), CHIP_NO_ERROR);
}

void Backend::ResolveNoLongerNeeded(const char * instanceName)
{
    if (instanceName == nullptr)
    {
        return;
    }
    std::vector<std::shared_ptr<ResolveOp>> matches;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto it = mResolveOps.begin(); it != mResolveOps.end();)
        {
            if ((*it)->mInstanceName == instanceName)
            {
                matches.push_back(*it);
                it = mResolveOps.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (auto & op : matches)
    {
        DnsServiceResolveCancel(&op->mCancel);
    }
}

CHIP_ERROR Backend::ReconfirmRecord(const char * hostname, Inet::IPAddress /* address */, Inet::InterfaceId /* interface */)
{
    VerifyOrReturnError(hostname != nullptr && hostname[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    // windns.h exposes no direct equivalent to the mDNSResponder "reconfirm
    // record" hook this API models; there is no safe native call to make here.
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

} // namespace

CHIP_ERROR ChipDnssdInit(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback errorCallback, void * context)
{
    return Backend::Instance().Init(initCallback, errorCallback, context);
}

void ChipDnssdShutdown()
{
    Backend::Instance().Shutdown();
}

CHIP_ERROR ChipDnssdRemoveServices()
{
    return Backend::Instance().RemoveServices();
}

CHIP_ERROR ChipDnssdPublishService(const DnssdService * service, DnssdPublishCallback callback, void * context)
{
    return Backend::Instance().PublishService(service, callback, context);
}

CHIP_ERROR ChipDnssdFinalizeServiceUpdate()
{
    return Backend::Instance().FinalizeServiceUpdate();
}

CHIP_ERROR ChipDnssdBrowse(const char * type, DnssdServiceProtocol protocol, chip::Inet::IPAddressType addressType,
                           chip::Inet::InterfaceId interface, DnssdBrowseCallback callback, void * context,
                           intptr_t * browseIdentifier)
{
    return Backend::Instance().Browse(type, protocol, addressType, interface, callback, context, browseIdentifier);
}

CHIP_ERROR ChipDnssdStopBrowse(intptr_t browseIdentifier)
{
    return Backend::Instance().StopBrowse(browseIdentifier);
}

CHIP_ERROR ChipDnssdResolve(DnssdService * browseResult, chip::Inet::InterfaceId interface, DnssdResolveCallback callback,
                            void * context)
{
    return Backend::Instance().Resolve(browseResult, interface, callback, context);
}

void ChipDnssdResolveNoLongerNeeded(const char * instanceName)
{
    Backend::Instance().ResolveNoLongerNeeded(instanceName);
}

CHIP_ERROR ChipDnssdReconfirmRecord(const char * hostname, chip::Inet::IPAddress address, chip::Inet::InterfaceId interface)
{
    return Backend::Instance().ReconfirmRecord(hostname, address, interface);
}

} // namespace Dnssd
} // namespace chip
