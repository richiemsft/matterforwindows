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

#include <inet/IPAddress.h>
#include <inet/InetInterface.h>

#include <array>
#include <cstdio>
#include <cstring>

int main()
{
    size_t interfaceCount = 0;
    bool foundLoopback    = false;
    for (chip::Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        const chip::Inet::InterfaceId id = iterator.GetInterfaceId();
        if (!iterator.HasCurrent() || !id.IsPresent() || id.GetInterfaceIndex() == 0)
        {
            return 1;
        }

        char name[chip::Inet::InterfaceId::kMaxIfNameLength];
        char numericName[16];
        chip::Inet::InterfaceId roundTrip;
        if (iterator.GetInterfaceName(name, sizeof(name)) != CHIP_NO_ERROR ||
            chip::Inet::InterfaceId::InterfaceNameToId(name, roundTrip) != CHIP_NO_ERROR || roundTrip != id ||
            std::snprintf(numericName, sizeof(numericName), "%lu", static_cast<unsigned long>(id.GetInterfaceIndex())) <= 0 ||
            chip::Inet::InterfaceId::InterfaceNameToId(numericName, roundTrip) != CHIP_NO_ERROR || roundTrip != id)
        {
            return 1;
        }

        std::array<uint8_t, 32> hardwareAddress;
        uint8_t hardwareAddressSize = 0;
        chip::Inet::InterfaceType type;
        if (iterator.GetHardwareAddress(hardwareAddress.data(), hardwareAddressSize,
                                        static_cast<uint8_t>(hardwareAddress.size())) != CHIP_NO_ERROR ||
            iterator.GetInterfaceType(type) != CHIP_NO_ERROR)
        {
            return 1;
        }

        foundLoopback |= iterator.IsLoopback();
        ++interfaceCount;
    }

    size_t addressCount = 0;
    bool foundIPv6      = false;
    for (chip::Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        chip::Inet::IPAddress address;
        const chip::Inet::InterfaceId id = iterator.GetInterfaceId();
        char name[chip::Inet::InterfaceId::kMaxIfNameLength];
        chip::Inet::InterfaceId roundTrip;
        if (!iterator.HasCurrent() || iterator.GetAddress(address) != CHIP_NO_ERROR || !id.IsPresent() ||
            iterator.GetInterfaceName(name, sizeof(name)) != CHIP_NO_ERROR ||
            chip::Inet::InterfaceId::InterfaceNameToId(name, roundTrip) != CHIP_NO_ERROR || roundTrip != id ||
            (address.IsIPv6() ? iterator.GetPrefixLength() > 128 : iterator.GetPrefixLength() > 32))
        {
            return 1;
        }

        if (address.IsIPv6LinkLocal())
        {
            chip::Inet::IPAddress linkLocal;
            if (id.GetLinkLocalAddr(&linkLocal) != CHIP_NO_ERROR || !linkLocal.IsIPv6LinkLocal())
            {
                return 1;
            }
        }
        foundIPv6 |= address.IsIPv6();
        ++addressCount;
    }

    char nullName[1] = { 'x' };
    if (chip::Inet::InterfaceId::Null().GetInterfaceName(nullName, sizeof(nullName)) != CHIP_NO_ERROR || nullName[0] != '\0')
    {
        return 1;
    }

    chip::Inet::InterfaceId missing;
    return interfaceCount > 0 && addressCount > 0 && foundLoopback && foundIPv6 &&
            chip::Inet::InterfaceId::InterfaceNameToId("not-a-windows-interface", missing) ==
            INET_ERROR_UNKNOWN_INTERFACE
        ? 0
        : 1;
}
