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

#include <platform/Windows/ConfigurationManagerImpl.h>

#include <lib/core/CHIPVendorIdentifiers.hpp>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/ConnectivityManager.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/PlatformManager.h>
#include <platform/internal/GenericConfigurationManagerImpl.ipp>

#include <cstring>

namespace chip {
namespace DeviceLayer {

using Internal::WindowsConfig;

ConfigurationManagerImpl & ConfigurationManagerImpl::GetDefaultInstance()
{
    static ConfigurationManagerImpl instance;
    return instance;
}

CHIP_ERROR ConfigurationManagerImpl::Init()
{
    return Initialize(nullptr);
}

CHIP_ERROR ConfigurationManagerImpl::ConfigureStorageRoot(const char * storageRoot)
{
    VerifyOrReturnError(!mInitialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(storageRoot != nullptr && storageRoot[0] != '\0', CHIP_ERROR_INVALID_ARGUMENT);
    mStorageRoot = storageRoot;
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::Initialize(const char * storageRoot)
{
    VerifyOrReturnError(!mInitialized, CHIP_ERROR_INCORRECT_STATE);
    const char * effectiveRoot = storageRoot;
    if (effectiveRoot == nullptr && !mStorageRoot.empty())
    {
        effectiveRoot = mStorageRoot.c_str();
    }
    ReturnErrorOnFailure(WindowsConfig::Init(effectiveRoot));
    if (storageRoot != nullptr)
    {
        mStorageRoot = storageRoot;
    }

    const CHIP_ERROR error = InitializeManagerState();
    if (error != CHIP_NO_ERROR)
    {
        WindowsConfig::Shutdown();
    }
    else
    {
        mInitialized = true;
    }
    return error;
}

CHIP_ERROR ConfigurationManagerImpl::InitializeManagerState()
{
    ReturnErrorOnFailure(Internal::GenericConfigurationManagerImpl<WindowsConfig>::Init());

    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_VendorId))
    {
        ReturnErrorOnFailure(StoreVendorId(CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID));
    }
    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_ProductId))
    {
        ReturnErrorOnFailure(StoreProductId(CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID));
    }

    uint32_t rebootCount = 0;
    if (WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_RebootCount))
    {
        ReturnErrorOnFailure(GetRebootCount(rebootCount));
        VerifyOrReturnError(rebootCount != UINT32_MAX, CHIP_ERROR_INVALID_INTEGER_VALUE);
        ReturnErrorOnFailure(StoreRebootCount(rebootCount + 1));
    }
    else
    {
        ReturnErrorOnFailure(StoreRebootCount(1));
    }

    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_TotalOperationalHours))
    {
        ReturnErrorOnFailure(StoreTotalOperationalHours(0));
    }
    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_BootReason))
    {
        ReturnErrorOnFailure(StoreBootReason(to_underlying(BootReasonType::kUnspecified)));
    }
    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_RegulatoryLocation))
    {
        ReturnErrorOnFailure(
            StoreRegulatoryLocation(to_underlying(app::Clusters::GeneralCommissioning::RegulatoryLocationTypeEnum::kIndoor)));
    }
    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_LocationCapability))
    {
        ReturnErrorOnFailure(WriteConfigValue(
            WindowsConfig::kConfigKey_LocationCapability,
            static_cast<uint32_t>(
                to_underlying(app::Clusters::GeneralCommissioning::RegulatoryLocationTypeEnum::kIndoorOutdoor))));
    }
    if (!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_ConfigurationVersion))
    {
        ReturnErrorOnFailure(StoreConfigurationVersion(1));
    }

    return CHIP_NO_ERROR;
}

void ConfigurationManagerImpl::Shutdown()
{
    WindowsConfig::Shutdown();
    mInitialized = false;
}

CHIP_ERROR ConfigurationManagerImpl::GetPrimaryWiFiMACAddress(uint8_t * buffer)
{
    VerifyOrReturnError(buffer != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    std::memset(buffer, 0, kEthernetMACAddressLength);

    const Inet::InterfaceId external = ConnectivityMgr().GetExternalInterface();
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        const Inet::InterfaceId id = iterator.GetInterfaceId();
        Inet::InterfaceType type;
        if (iterator.IsLoopback() || iterator.GetInterfaceType(type) != CHIP_NO_ERROR || type != Inet::InterfaceType::WiFi)
        {
            continue;
        }
        if (external.IsPresent() && id != external)
        {
            continue;
        }

        uint8_t addressLength = 0;
        if (iterator.GetHardwareAddress(buffer, addressLength, kEthernetMACAddressLength) == CHIP_NO_ERROR &&
            addressLength == kEthernetMACAddressLength)
        {
            return CHIP_NO_ERROR;
        }
    }

    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceType type;
        if (iterator.IsLoopback() || iterator.GetInterfaceType(type) != CHIP_NO_ERROR || type != Inet::InterfaceType::WiFi)
        {
            continue;
        }

        uint8_t addressLength = 0;
        if (iterator.GetHardwareAddress(buffer, addressLength, kEthernetMACAddressLength) == CHIP_NO_ERROR &&
            addressLength == kEthernetMACAddressLength)
        {
            return CHIP_NO_ERROR;
        }
    }

    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR ConfigurationManagerImpl::GetPrimaryMACAddress(MutableByteSpan & buffer)
{
    VerifyOrReturnError(buffer.size() == kPrimaryMACAddressLength, CHIP_ERROR_INVALID_ARGUMENT);
    std::memset(buffer.data(), 0, buffer.size());

    const Inet::InterfaceId external = ConnectivityMgr().GetExternalInterface();
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceType type;
        if (iterator.GetInterfaceId() != external || iterator.GetInterfaceType(type) != CHIP_NO_ERROR ||
            (type != Inet::InterfaceType::Ethernet && type != Inet::InterfaceType::WiFi))
        {
            continue;
        }

        uint8_t addressLength = 0;
        if (iterator.GetHardwareAddress(buffer.data(), addressLength, static_cast<uint8_t>(buffer.size())) == CHIP_NO_ERROR &&
            addressLength == kEthernetMACAddressLength)
        {
            buffer.reduce_size(kEthernetMACAddressLength);
            return CHIP_NO_ERROR;
        }
    }

    if (GetPrimaryWiFiMACAddress(buffer.data()) == CHIP_NO_ERROR)
    {
        buffer.reduce_size(kEthernetMACAddressLength);
        return CHIP_NO_ERROR;
    }

    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceType type;
        if (iterator.IsLoopback() || iterator.GetInterfaceType(type) != CHIP_NO_ERROR || type != Inet::InterfaceType::Ethernet)
        {
            continue;
        }

        uint8_t addressLength = 0;
        if (iterator.GetHardwareAddress(buffer.data(), addressLength, static_cast<uint8_t>(buffer.size())) == CHIP_NO_ERROR &&
            addressLength == kEthernetMACAddressLength)
        {
            buffer.reduce_size(kEthernetMACAddressLength);
            return CHIP_NO_ERROR;
        }
    }

    return CHIP_ERROR_NOT_FOUND;
}

bool ConfigurationManagerImpl::CanFactoryReset()
{
    return true;
}

void ConfigurationManagerImpl::InitiateFactoryReset()
{
    CHIP_ERROR error = PlatformMgr().ScheduleWork(DoFactoryReset);
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to schedule factory reset: %" CHIP_ERROR_FORMAT, error.Format());
    }
}

void ConfigurationManagerImpl::DoFactoryReset(intptr_t argument)
{
    (void) argument;
    const CHIP_ERROR error = PersistedStorage::KeyValueStoreMgrImpl().ClearAllExceptPrefix("factory/");
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to reset Windows Matter state: %" CHIP_ERROR_FORMAT, error.Format());
    }
    ChipLogProgress(DeviceLayer, "Factory reset complete; application restart is required");
}

CHIP_ERROR ConfigurationManagerImpl::StoreVendorId(uint16_t vendorId)
{
    return WriteConfigValue(WindowsConfig::kConfigKey_VendorId, vendorId);
}

CHIP_ERROR ConfigurationManagerImpl::StoreProductId(uint16_t productId)
{
    return WriteConfigValue(WindowsConfig::kConfigKey_ProductId, productId);
}

CHIP_ERROR ConfigurationManagerImpl::GetRebootCount(uint32_t & rebootCount)
{
    return ReadConfigValue(WindowsConfig::kCounterKey_RebootCount, rebootCount);
}

CHIP_ERROR ConfigurationManagerImpl::StoreRebootCount(uint32_t rebootCount)
{
    return WriteConfigValue(WindowsConfig::kCounterKey_RebootCount, rebootCount);
}

CHIP_ERROR ConfigurationManagerImpl::GetTotalOperationalHours(uint32_t & totalOperationalHours)
{
    return ReadConfigValue(WindowsConfig::kCounterKey_TotalOperationalHours, totalOperationalHours);
}

CHIP_ERROR ConfigurationManagerImpl::StoreTotalOperationalHours(uint32_t totalOperationalHours)
{
    return WriteConfigValue(WindowsConfig::kCounterKey_TotalOperationalHours, totalOperationalHours);
}

CHIP_ERROR ConfigurationManagerImpl::GetBootReason(uint32_t & bootReason)
{
    return ReadConfigValue(WindowsConfig::kCounterKey_BootReason, bootReason);
}

CHIP_ERROR ConfigurationManagerImpl::StoreBootReason(uint32_t bootReason)
{
    return WriteConfigValue(WindowsConfig::kCounterKey_BootReason, bootReason);
}

CHIP_ERROR ConfigurationManagerImpl::GetRegulatoryLocation(uint8_t & location)
{
    uint32_t value = 0;
    ReturnErrorOnFailure(ReadConfigValue(WindowsConfig::kConfigKey_RegulatoryLocation, value));
    VerifyOrReturnError(value <= UINT8_MAX, CHIP_ERROR_INVALID_INTEGER_VALUE);
    location = static_cast<uint8_t>(value);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::GetLocationCapability(uint8_t & location)
{
    uint32_t value = 0;
    ReturnErrorOnFailure(ReadConfigValue(WindowsConfig::kConfigKey_LocationCapability, value));
    VerifyOrReturnError(value <= UINT8_MAX, CHIP_ERROR_INVALID_INTEGER_VALUE);
    location = static_cast<uint8_t>(value);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::GetConfigurationVersion(uint32_t & configurationVersion)
{
    return ReadConfigValue(WindowsConfig::kConfigKey_ConfigurationVersion, configurationVersion);
}

CHIP_ERROR ConfigurationManagerImpl::StoreConfigurationVersion(uint32_t configurationVersion)
{
    return WriteConfigValue(WindowsConfig::kConfigKey_ConfigurationVersion, configurationVersion);
}

CHIP_ERROR ConfigurationManagerImpl::ReadPersistedStorageValue(Platform::PersistedStorage::Key key, uint32_t & value)
{
    WindowsConfig::Key configKey{ WindowsConfig::kConfigNamespace_ChipCounters, key };
    CHIP_ERROR error = ReadConfigValue(configKey, value);
    return error == CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND ? CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND : error;
}

CHIP_ERROR ConfigurationManagerImpl::WritePersistedStorageValue(Platform::PersistedStorage::Key key, uint32_t value)
{
    return WriteConfigValue({ WindowsConfig::kConfigNamespace_ChipCounters, key }, value);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, bool & value)
{
    return WindowsConfig::ReadConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint16_t & value)
{
    return WindowsConfig::ReadConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint32_t & value)
{
    return WindowsConfig::ReadConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint64_t & value)
{
    return WindowsConfig::ReadConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueStr(Key key, char * buffer, size_t bufferSize, size_t & outLength)
{
    return WindowsConfig::ReadConfigValueStr(key, buffer, bufferSize, outLength);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueBin(Key key, uint8_t * buffer, size_t bufferSize, size_t & outLength)
{
    return WindowsConfig::ReadConfigValueBin(key, buffer, bufferSize, outLength);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, bool value)
{
    return WindowsConfig::WriteConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint16_t value)
{
    return WindowsConfig::WriteConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint32_t value)
{
    return WindowsConfig::WriteConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint64_t value)
{
    return WindowsConfig::WriteConfigValue(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * value)
{
    return WindowsConfig::WriteConfigValueStr(key, value);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * value, size_t valueLength)
{
    return WindowsConfig::WriteConfigValueStr(key, value, valueLength);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueBin(Key key, const uint8_t * value, size_t valueLength)
{
    return WindowsConfig::WriteConfigValueBin(key, value, valueLength);
}

void ConfigurationManagerImpl::RunConfigUnitTest() {}

ConfigurationManager & ConfigurationMgrImpl()
{
    return ConfigurationManagerImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip
