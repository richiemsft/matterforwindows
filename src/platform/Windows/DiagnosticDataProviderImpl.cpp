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

#include <platform/Windows/DiagnosticDataProviderImpl.h>

#include <app/data-model/List.h>
#include <lib/support/CodeUtils.h>
#include <platform/ConfigurationManager.h>
#include <platform/PlatformManager.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <new>

namespace chip {
namespace DeviceLayer {

namespace {

app::Clusters::GeneralDiagnostics::InterfaceTypeEnum MapInterfaceType(Inet::InterfaceType type)
{
    using InterfaceTypeEnum = app::Clusters::GeneralDiagnostics::InterfaceTypeEnum;

    switch (type)
    {
    case Inet::InterfaceType::WiFi:
        return InterfaceTypeEnum::kWiFi;
    case Inet::InterfaceType::Ethernet:
        return InterfaceTypeEnum::kEthernet;
    case Inet::InterfaceType::Thread:
        return InterfaceTypeEnum::kThread;
    case Inet::InterfaceType::Cellular:
        return InterfaceTypeEnum::kCellular;
    case Inet::InterfaceType::Unknown:
        return InterfaceTypeEnum::kUnspecified;
    }
    return InterfaceTypeEnum::kUnspecified;
}

NetworkInterface * FindInterface(NetworkInterface * interfaces, const char * name)
{
    for (NetworkInterface * interface = interfaces; interface != nullptr; interface = interface->Next)
    {
        if (std::strcmp(interface->Name, name) == 0)
        {
            return interface;
        }
    }
    return nullptr;
}

void PopulateAddresses(NetworkInterface * interfaces)
{
    for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        char name[Inet::InterfaceId::kMaxIfNameLength] = {};
        if (iterator.GetInterfaceName(name, sizeof(name)) != CHIP_NO_ERROR)
        {
            continue;
        }

        NetworkInterface * interface = FindInterface(interfaces, name);
        if (interface == nullptr)
        {
            continue;
        }

        Inet::IPAddress address;
        if (iterator.GetAddress(address) != CHIP_NO_ERROR)
        {
            continue;
        }

        const size_t ipv4Count = interface->IPv4Addresses.size();
        const size_t ipv6Count = interface->IPv6Addresses.size();
        if (address.IsIPv4() && ipv4Count < kMaxIPv4AddrCount)
        {
            std::memcpy(interface->Ipv4AddressesBuffer[ipv4Count], &address.Addr[3], kMaxIPv4AddrSize);
            interface->Ipv4AddressSpans[ipv4Count] = ByteSpan(interface->Ipv4AddressesBuffer[ipv4Count]);
            interface->IPv4Addresses = app::DataModel::List<const ByteSpan>(interface->Ipv4AddressSpans, ipv4Count + 1);
        }
        else if (address.IsIPv6() && ipv6Count < kMaxIPv6AddrCount)
        {
            std::memcpy(interface->Ipv6AddressesBuffer[ipv6Count], address.Addr, kMaxIPv6AddrSize);
            interface->Ipv6AddressSpans[ipv6Count] = ByteSpan(interface->Ipv6AddressesBuffer[ipv6Count]);
            interface->IPv6Addresses = app::DataModel::List<const ByteSpan>(interface->Ipv6AddressSpans, ipv6Count + 1);
        }
    }
}

} // namespace

DiagnosticDataProviderImpl & DiagnosticDataProviderImpl::GetDefaultInstance()
{
    static DiagnosticDataProviderImpl instance;
    return instance;
}

DiagnosticDataProvider & GetDiagnosticDataProviderImpl()
{
    return DiagnosticDataProviderImpl::GetDefaultInstance();
}

CHIP_ERROR DiagnosticDataProviderImpl::GetRebootCount(uint16_t & rebootCount)
{
    uint32_t count = 0;
    ReturnErrorOnFailure(ConfigurationMgr().GetRebootCount(count));
    VerifyOrReturnError(count <= std::numeric_limits<uint16_t>::max(), CHIP_ERROR_INVALID_INTEGER_VALUE);
    rebootCount = static_cast<uint16_t>(count);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetUpTime(uint64_t & upTime)
{
    const System::Clock::Timestamp currentTime = System::SystemClock().GetMonotonicTimestamp();
    const System::Clock::Timestamp startTime   = PlatformMgrImpl().GetStartTime();
    VerifyOrReturnError(currentTime >= startTime, CHIP_ERROR_INVALID_TIME);

    upTime = static_cast<uint64_t>(std::chrono::duration_cast<System::Clock::Seconds64>(currentTime - startTime).count());
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetTotalOperationalHours(uint32_t & totalOperationalHours)
{
    uint64_t upTime = 0;
    ReturnErrorOnFailure(GetUpTime(upTime));

    uint32_t persistedHours = 0;
    ReturnErrorOnFailure(ConfigurationMgr().GetTotalOperationalHours(persistedHours));

    const uint64_t currentRunHours = upTime / 3600;
    VerifyOrReturnError(currentRunHours <= std::numeric_limits<uint32_t>::max() - persistedHours,
                        CHIP_ERROR_INVALID_INTEGER_VALUE);
    totalOperationalHours = persistedHours + static_cast<uint32_t>(currentRunHours);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetBootReason(BootReasonType & bootReason)
{
    uint32_t reason = 0;
    ReturnErrorOnFailure(ConfigurationMgr().GetBootReason(reason));
    VerifyOrReturnError(reason <= std::numeric_limits<uint8_t>::max(), CHIP_ERROR_INVALID_INTEGER_VALUE);
    bootReason = static_cast<BootReasonType>(reason);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetActiveHardwareFaults(GeneralFaults<kMaxHardwareFaults> & hardwareFaults)
{
    (void) hardwareFaults;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetActiveRadioFaults(GeneralFaults<kMaxRadioFaults> & radioFaults)
{
    (void) radioFaults;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetActiveNetworkFaults(GeneralFaults<kMaxNetworkFaults> & networkFaults)
{
    (void) networkFaults;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetNetworkInterfaces(NetworkInterface ** interfaces)
{
    VerifyOrReturnError(interfaces != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    *interfaces = nullptr;

    NetworkInterface * head = nullptr;
    NetworkInterface * tail = nullptr;
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        NetworkInterface * interface = new (std::nothrow) NetworkInterface();
        if (interface == nullptr)
        {
            ReleaseNetworkInterfaces(head);
            return CHIP_ERROR_NO_MEMORY;
        }

        CHIP_ERROR error = iterator.GetInterfaceName(interface->Name, sizeof(interface->Name));
        if (error != CHIP_NO_ERROR)
        {
            delete interface;
            continue;
        }

        interface->name          = CharSpan::fromCharString(interface->Name);
        interface->isOperational = iterator.IsUp();
        interface->offPremiseServicesReachableIPv4.SetNull();
        interface->offPremiseServicesReachableIPv6.SetNull();

        Inet::InterfaceType type = Inet::InterfaceType::Unknown;
        if (iterator.GetInterfaceType(type) == CHIP_NO_ERROR)
        {
            interface->type = MapInterfaceType(type);
        }
        else
        {
            interface->type = app::Clusters::GeneralDiagnostics::InterfaceTypeEnum::kUnspecified;
        }

        uint8_t hardwareAddressSize = 0;
        if (iterator.GetHardwareAddress(interface->MacAddress, hardwareAddressSize, sizeof(interface->MacAddress)) == CHIP_NO_ERROR)
        {
            interface->hardwareAddress = ByteSpan(interface->MacAddress, hardwareAddressSize);
        }

        if (tail == nullptr)
        {
            head = interface;
        }
        else
        {
            tail->Next = interface;
        }
        tail = interface;
    }

    PopulateAddresses(head);
    *interfaces = head;
    return CHIP_NO_ERROR;
}

void DiagnosticDataProviderImpl::ReleaseNetworkInterfaces(NetworkInterface * interfaces)
{
    while (interfaces != nullptr)
    {
        NetworkInterface * current = interfaces;
        interfaces                 = interfaces->Next;
        delete current;
    }
}

} // namespace DeviceLayer
} // namespace chip
