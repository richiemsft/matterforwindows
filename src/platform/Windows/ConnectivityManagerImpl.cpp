/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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

#include <platform/ConnectivityManager.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/PlatformManager.h>
#include <platform/internal/GenericConnectivityManagerImpl_TCP.ipp>
#include <platform/internal/GenericConnectivityManagerImpl_UDP.ipp>
#include <system/SystemError.h>

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>

namespace chip {
namespace DeviceLayer {

namespace {

struct ConnectivityState
{
    bool haveIPv4 = false;
    bool haveIPv6 = false;
};

bool HasUsableAddress(Inet::InterfaceId id)
{
    for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        if (iterator.GetInterfaceId() == id && iterator.IsUp() && !iterator.IsLoopback() && iterator.SupportsMulticast())
        {
            return true;
        }
    }
    return false;
}

CHIP_ERROR FindInterface(Inet::InterfaceType requestedType, bool requireUsableAddress, char * name, size_t nameSize,
                         Inet::InterfaceId * id)
{
    VerifyOrReturnError(name != nullptr && nameSize > 0, CHIP_ERROR_INVALID_ARGUMENT);

    Inet::InterfaceId fallback = Inet::InterfaceId::Null();
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceType type;
        if (iterator.GetInterfaceType(type) != CHIP_NO_ERROR || type != requestedType || iterator.IsLoopback())
        {
            continue;
        }

        const Inet::InterfaceId current = iterator.GetInterfaceId();
        if (!fallback.IsPresent())
        {
            fallback = current;
        }
        if (iterator.IsUp() && (!requireUsableAddress || HasUsableAddress(current)))
        {
            fallback = current;
            break;
        }
    }

    VerifyOrReturnError(fallback.IsPresent() && (!requireUsableAddress || HasUsableAddress(fallback)), CHIP_ERROR_KEY_NOT_FOUND);
    ReturnErrorOnFailure(fallback.GetInterfaceName(name, nameSize));
    if (id != nullptr)
    {
        *id = fallback;
    }
    return CHIP_NO_ERROR;
}

ConnectivityState ReadConnectivityState()
{
    ConnectivityState state;
    for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        if (!iterator.IsUp() || iterator.IsLoopback() || !iterator.SupportsMulticast())
        {
            continue;
        }

        Inet::IPAddress address;
        if (iterator.GetAddress(address) != CHIP_NO_ERROR)
        {
            continue;
        }
        state.haveIPv4 = state.haveIPv4 || address.IsIPv4();
        state.haveIPv6 = state.haveIPv6 || address.IsIPv6();
    }
    return state;
}

void WINAPI HandleInterfaceChange(void * context, MIB_IPINTERFACE_ROW *, MIB_NOTIFICATION_TYPE)
{
    static_cast<ConnectivityManagerImpl *>(context)->NotifyNetworkChange();
}

void WINAPI HandleAddressChange(void * context, MIB_UNICASTIPADDRESS_ROW *, MIB_NOTIFICATION_TYPE)
{
    static_cast<ConnectivityManagerImpl *>(context)->NotifyNetworkChange();
}

} // namespace

ConnectivityManagerImpl ConnectivityManagerImpl::sInstance;

CHIP_ERROR ConnectivityManagerImpl::_Init()
{
    VerifyOrReturnError(!mInitialized.load(std::memory_order_acquire), CHIP_ERROR_INCORRECT_STATE);

    const ConnectivityState state = ReadConnectivityState();
    mHaveIPv4Connectivity         = state.haveIPv4;
    mHaveIPv6Connectivity         = state.haveIPv6;
    mRefreshScheduled.store(false, std::memory_order_release);
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mInitialized.store(true, std::memory_order_release);

    HANDLE interfaceHandle = nullptr;
    DWORD result = NotifyIpInterfaceChange(AF_UNSPEC, HandleInterfaceChange, this, FALSE, &interfaceHandle);
    if (result != NO_ERROR)
    {
        mInitialized.store(false, std::memory_order_release);
        return CHIP_ERROR_WINDOWS(result);
    }
    mInterfaceChangeHandle = interfaceHandle;

    HANDLE addressHandle = nullptr;
    result = NotifyUnicastIpAddressChange(AF_UNSPEC, HandleAddressChange, this, FALSE, &addressHandle);
    if (result != NO_ERROR)
    {
        mInitialized.store(false, std::memory_order_release);
        mInterfaceChangeHandle = nullptr;
        const DWORD cancelResult = CancelMibChangeNotify2(interfaceHandle);
        if (cancelResult != NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to cancel Windows interface notifications: %lu",
                         static_cast<unsigned long>(cancelResult));
        }
        return CHIP_ERROR_WINDOWS(result);
    }
    mAddressChangeHandle = addressHandle;

    RefreshConnectivityState();
    return CHIP_NO_ERROR;
}

void ConnectivityManagerImpl::Shutdown()
{
    if (!mInitialized.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }

    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mRefreshScheduled.store(false, std::memory_order_release);

    HANDLE interfaceHandle = static_cast<HANDLE>(mInterfaceChangeHandle);
    HANDLE addressHandle   = static_cast<HANDLE>(mAddressChangeHandle);
    mInterfaceChangeHandle = nullptr;
    mAddressChangeHandle   = nullptr;

    if (interfaceHandle != nullptr)
    {
        const DWORD result = CancelMibChangeNotify2(interfaceHandle);
        if (result != NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to cancel Windows interface notifications: %lu",
                         static_cast<unsigned long>(result));
        }
    }
    if (addressHandle != nullptr)
    {
        const DWORD result = CancelMibChangeNotify2(addressHandle);
        if (result != NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to cancel Windows address notifications: %lu", static_cast<unsigned long>(result));
        }
    }
}

void ConnectivityManagerImpl::NotifyNetworkChange()
{
    if (!mInitialized.load(std::memory_order_acquire))
    {
        return;
    }

    bool expected = false;
    if (!mRefreshScheduled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    const uint32_t generation = mGeneration.load(std::memory_order_acquire);
    const CHIP_ERROR error =
        PlatformMgr().ScheduleWork(static_cast<AsyncWorkFunct>(RefreshConnectivityState), static_cast<intptr_t>(generation));
    if (error != CHIP_NO_ERROR && mGeneration.load(std::memory_order_acquire) == generation)
    {
        mRefreshScheduled.store(false, std::memory_order_release);
    }
}

void ConnectivityManagerImpl::RefreshConnectivityState(intptr_t generation)
{
    ConnectivityManagerImpl & manager = ConnectivityMgrImpl();
    if (static_cast<uint32_t>(generation) != manager.mGeneration.load(std::memory_order_acquire))
    {
        return;
    }

    manager.mRefreshScheduled.store(false, std::memory_order_release);
    if (manager.mInitialized.load(std::memory_order_acquire))
    {
        manager.RefreshConnectivityState();
    }
}

void ConnectivityManagerImpl::RefreshConnectivityState()
{
    VerifyOrReturn(mInitialized.load(std::memory_order_acquire));

    const ConnectivityState state = ReadConnectivityState();
    const ConnectivityChange ipv4 = GetConnectivityChange(mHaveIPv4Connectivity, state.haveIPv4);
    const ConnectivityChange ipv6 = GetConnectivityChange(mHaveIPv6Connectivity, state.haveIPv6);
    mHaveIPv4Connectivity          = state.haveIPv4;
    mHaveIPv6Connectivity          = state.haveIPv6;

    ConnectivityManagerDelegate * delegate = GetDelegate();
    if (delegate != nullptr)
    {
        delegate->OnNetworkInfoChanged();
    }

    if (ipv4 != kConnectivity_NoChange || ipv6 != kConnectivity_NoChange)
    {
        ChipDeviceEvent internetEvent{};
        internetEvent.Type                            = DeviceEventType::kInternetConnectivityChange;
        internetEvent.InternetConnectivityChange.IPv4 = ipv4;
        internetEvent.InternetConnectivityChange.IPv6 = ipv6;
        PlatformMgr().PostEventOrDie(&internetEvent);
    }

    if (ipv4 != kConnectivity_NoChange)
    {
        ChipDeviceEvent addressEvent{};
        addressEvent.Type = DeviceEventType::kInterfaceIpAddressChanged;
        addressEvent.InterfaceIpAddressChanged.Type =
            state.haveIPv4 ? InterfaceIpChangeType::kIpV4_Assigned : InterfaceIpChangeType::kIpV4_Lost;
        PlatformMgr().PostEventOrDie(&addressEvent);
    }
    if (ipv6 != kConnectivity_NoChange)
    {
        ChipDeviceEvent addressEvent{};
        addressEvent.Type = DeviceEventType::kInterfaceIpAddressChanged;
        addressEvent.InterfaceIpAddressChanged.Type =
            state.haveIPv6 ? InterfaceIpChangeType::kIpV6_Assigned : InterfaceIpChangeType::kIpV6_Lost;
        PlatformMgr().PostEventOrDie(&addressEvent);
    }
}

void ConnectivityManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
    (void) event;
}

CHIP_ERROR ConnectivityManagerImpl::GetEthernetInterfaceName(char * name, size_t nameSize)
{
    return FindInterface(Inet::InterfaceType::Ethernet, false, name, nameSize, nullptr);
}

CHIP_ERROR ConnectivityManagerImpl::GetWiFiInterfaceName(char * name, size_t nameSize)
{
    return FindInterface(Inet::InterfaceType::WiFi, false, name, nameSize, nullptr);
}

CHIP_ERROR ConnectivityManagerImpl::GetInterfaceStatus(const char * name, bool & isUp)
{
    VerifyOrReturnError(name != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    Inet::InterfaceId requested;
    ReturnErrorOnFailure(Inet::InterfaceId::InterfaceNameToId(name, requested));
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        if (iterator.GetInterfaceId() == requested)
        {
            isUp = iterator.IsUp();
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_KEY_NOT_FOUND;
}

Inet::InterfaceId ConnectivityManagerImpl::_GetExternalInterface()
{
    char name[Inet::InterfaceId::kMaxIfNameLength] = {};
    Inet::InterfaceId id;
    if (FindInterface(Inet::InterfaceType::Ethernet, true, name, sizeof(name), &id) == CHIP_NO_ERROR)
    {
        return id;
    }
    if (FindInterface(Inet::InterfaceType::WiFi, true, name, sizeof(name), &id) == CHIP_NO_ERROR)
    {
        return id;
    }

    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        if (iterator.IsUp() && !iterator.IsLoopback() && iterator.SupportsMulticast() &&
            HasUsableAddress(iterator.GetInterfaceId()))
        {
            return iterator.GetInterfaceId();
        }
    }
    return Inet::InterfaceId::Null();
}

} // namespace DeviceLayer
} // namespace chip
