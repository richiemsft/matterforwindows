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

#include <cstddef>
#include <cstdint>

#include <lib/core/CHIPError.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

class WindowsConfig
{
public:
    struct Key
    {
        const char * Namespace;
        const char * Name;

        bool operator==(const Key & other) const;
    };

    static const char kConfigNamespace_ChipFactory[];
    static const char kConfigNamespace_ChipConfig[];
    static const char kConfigNamespace_ChipCounters[];

    static const Key kConfigKey_SerialNum;
    static const Key kConfigKey_UniqueId;
    static const Key kConfigKey_MfrDeviceId;
    static const Key kConfigKey_MfrDeviceCert;
    static const Key kConfigKey_MfrDeviceICACerts;
    static const Key kConfigKey_MfrDevicePrivateKey;
    static const Key kConfigKey_HardwareVersion;
    static const Key kConfigKey_ManufacturingDate;
    static const Key kConfigKey_SetupPinCode;
    static const Key kConfigKey_ServiceConfig;
    static const Key kConfigKey_PairedAccountId;
    static const Key kConfigKey_ServiceId;
    static const Key kConfigKey_LastUsedEpochKeyId;
    static const Key kConfigKey_FailSafeArmed;
    static const Key kConfigKey_SetupDiscriminator;
    static const Key kConfigKey_RegulatoryLocation;
    static const Key kConfigKey_CountryCode;
    static const Key kConfigKey_LocationCapability;
    static const Key kConfigKey_ConfigurationVersion;
    static const Key kConfigKey_Spake2pIterationCount;
    static const Key kConfigKey_Spake2pSalt;
    static const Key kConfigKey_Spake2pVerifier;
    static const Key kConfigKey_VendorId;
    static const Key kConfigKey_ProductId;

    static const Key kCounterKey_RebootCount;
    static const Key kCounterKey_UpTime;
    static const Key kCounterKey_TotalOperationalHours;
    static const Key kCounterKey_BootReason;

    static CHIP_ERROR Init(const char * storageRoot = nullptr);
    static void Shutdown();

    static CHIP_ERROR ReadConfigValue(Key key, bool & value);
    static CHIP_ERROR ReadConfigValue(Key key, uint16_t & value);
    static CHIP_ERROR ReadConfigValue(Key key, uint32_t & value);
    static CHIP_ERROR ReadConfigValue(Key key, uint64_t & value);
    static CHIP_ERROR ReadConfigValueStr(Key key, char * buffer, size_t bufferSize, size_t & outLength);
    static CHIP_ERROR ReadConfigValueBin(Key key, uint8_t * buffer, size_t bufferSize, size_t & outLength);
    static CHIP_ERROR WriteConfigValue(Key key, bool value);
    static CHIP_ERROR WriteConfigValue(Key key, uint16_t value);
    static CHIP_ERROR WriteConfigValue(Key key, uint32_t value);
    static CHIP_ERROR WriteConfigValue(Key key, uint64_t value);
    static CHIP_ERROR WriteConfigValueStr(Key key, const char * value);
    static CHIP_ERROR WriteConfigValueStr(Key key, const char * value, size_t valueLength);
    static CHIP_ERROR WriteConfigValueBin(Key key, const uint8_t * value, size_t valueLength);
    static CHIP_ERROR ClearConfigValue(Key key);
    static bool ConfigValueExists(Key key);
    static CHIP_ERROR EnsureNamespace(const char * configNamespace);
    static CHIP_ERROR ClearNamespace(const char * configNamespace);
    static CHIP_ERROR FactoryResetConfig();
    static CHIP_ERROR FactoryResetCounters();
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
