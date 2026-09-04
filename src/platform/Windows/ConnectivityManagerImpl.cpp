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
#include <platform/internal/GenericConnectivityManagerImpl_TCP.ipp>
#include <platform/internal/GenericConnectivityManagerImpl_UDP.ipp>

#include <cstring>

namespace chip {
namespace DeviceLayer {

namespace {

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

} // namespace

ConnectivityManagerImpl ConnectivityManagerImpl::sInstance;

CHIP_ERROR ConnectivityManagerImpl::_Init()
{
    return CHIP_NO_ERROR;
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
