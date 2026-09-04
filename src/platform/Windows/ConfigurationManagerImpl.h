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

#pragma once

#include <string>

#include <platform/Windows/WindowsConfig.h>
#include <platform/internal/GenericConfigurationManagerImpl.h>

namespace chip {
namespace DeviceLayer {

class ConfigurationManagerImpl final : public Internal::GenericConfigurationManagerImpl<Internal::WindowsConfig>
{
public:
    static ConfigurationManagerImpl & GetDefaultInstance();

    CHIP_ERROR ConfigureStorageRoot(const char * storageRoot);
    void Shutdown();

    CHIP_ERROR StoreVendorId(uint16_t vendorId);
    CHIP_ERROR StoreProductId(uint16_t productId);

    CHIP_ERROR GetPrimaryMACAddress(MutableByteSpan & buffer) override;
    CHIP_ERROR GetRebootCount(uint32_t & rebootCount) override;
    CHIP_ERROR StoreRebootCount(uint32_t rebootCount) override;
    CHIP_ERROR GetTotalOperationalHours(uint32_t & totalOperationalHours) override;
    CHIP_ERROR StoreTotalOperationalHours(uint32_t totalOperationalHours) override;
    CHIP_ERROR GetBootReason(uint32_t & bootReason) override;
    CHIP_ERROR StoreBootReason(uint32_t bootReason) override;
    CHIP_ERROR GetRegulatoryLocation(uint8_t & location) override;
    CHIP_ERROR GetLocationCapability(uint8_t & location) override;
    CHIP_ERROR GetConfigurationVersion(uint32_t & configurationVersion) override;
    CHIP_ERROR StoreConfigurationVersion(uint32_t configurationVersion) override;

private:
    CHIP_ERROR Init() override;
    CHIP_ERROR Initialize(const char * storageRoot);
    CHIP_ERROR InitializeManagerState();
    CHIP_ERROR GetPrimaryWiFiMACAddress(uint8_t * buffer) override;
    bool CanFactoryReset() override;
    void InitiateFactoryReset() override;
    CHIP_ERROR ReadPersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t & value) override;
    CHIP_ERROR WritePersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t value) override;

    CHIP_ERROR WriteConfigValue(Key key, uint16_t value);
    CHIP_ERROR ReadConfigValue(Key key, uint16_t & value);
    CHIP_ERROR ReadConfigValue(Key key, bool & value) override;
    CHIP_ERROR ReadConfigValue(Key key, uint32_t & value) override;
    CHIP_ERROR ReadConfigValue(Key key, uint64_t & value) override;
    CHIP_ERROR ReadConfigValueStr(Key key, char * buffer, size_t bufferSize, size_t & outLength) override;
    CHIP_ERROR ReadConfigValueBin(Key key, uint8_t * buffer, size_t bufferSize, size_t & outLength) override;
    CHIP_ERROR WriteConfigValue(Key key, bool value) override;
    CHIP_ERROR WriteConfigValue(Key key, uint32_t value) override;
    CHIP_ERROR WriteConfigValue(Key key, uint64_t value) override;
    CHIP_ERROR WriteConfigValueStr(Key key, const char * value) override;
    CHIP_ERROR WriteConfigValueStr(Key key, const char * value, size_t valueLength) override;
    CHIP_ERROR WriteConfigValueBin(Key key, const uint8_t * value, size_t valueLength) override;
    void RunConfigUnitTest() override;

    static void DoFactoryReset(intptr_t argument);

    std::string mStorageRoot;
    bool mInitialized = false;
};

ConfigurationManager & ConfigurationMgrImpl();

} // namespace DeviceLayer
} // namespace chip
