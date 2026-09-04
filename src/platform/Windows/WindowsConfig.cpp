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

#include <platform/Windows/WindowsConfig.h>

#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceError.h>
#include <platform/KeyValueStoreManager.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

enum class ValueType : uint8_t
{
    kBoolean = 1,
    kUInt16  = 2,
    kUInt32  = 3,
    kUInt64  = 4,
    kString  = 5,
    kBinary  = 6,
};

constexpr size_t kTypeSize = 1;

CHIP_ERROR BuildStorageKey(WindowsConfig::Key key, std::string & storageKey)
{
    VerifyOrReturnError(key.Namespace != nullptr && key.Name != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    storageKey.assign(key.Namespace);
    storageKey.push_back('/');
    storageKey.append(key.Name);
    VerifyOrReturnError(storageKey.size() <= 64, CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR MapStorageError(CHIP_ERROR error)
{
    return error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND ? CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND : error;
}

CHIP_ERROR ReadRecord(WindowsConfig::Key key, ValueType expectedType, std::vector<uint8_t> & payload)
{
    std::string storageKey;
    ReturnErrorOnFailure(BuildStorageKey(key, storageKey));

    std::vector<uint8_t> record;
    CHIP_ERROR error = PersistedStorage::KeyValueStoreMgrImpl().GetValue(storageKey.c_str(), record);
    ReturnErrorOnFailure(MapStorageError(error));
    VerifyOrReturnError(record.size() >= kTypeSize && record[0] == static_cast<uint8_t>(expectedType),
                        CHIP_ERROR_INTEGRITY_CHECK_FAILED);

    payload.assign(record.begin() + kTypeSize, record.end());
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteRecord(WindowsConfig::Key key, ValueType type, const uint8_t * payload, size_t payloadLength)
{
    VerifyOrReturnError(payload != nullptr || payloadLength == 0, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(payloadLength <= PersistedStorage::KeyValueStoreManagerImpl::kMaxValueLength - kTypeSize,
                        CHIP_ERROR_INVALID_ARGUMENT);

    std::string storageKey;
    ReturnErrorOnFailure(BuildStorageKey(key, storageKey));
    std::vector<uint8_t> record(kTypeSize + payloadLength);
    record[0] = static_cast<uint8_t>(type);
    if (payloadLength != 0)
    {
        std::memcpy(record.data() + kTypeSize, payload, payloadLength);
    }
    return PersistedStorage::KeyValueStoreMgr().Put(storageKey.c_str(), record.data(), record.size());
}

template <typename Integer>
CHIP_ERROR ReadUnsigned(WindowsConfig::Key key, ValueType type, Integer & value)
{
    std::vector<uint8_t> payload;
    ReturnErrorOnFailure(ReadRecord(key, type, payload));
    VerifyOrReturnError(payload.size() == sizeof(Integer), CHIP_ERROR_INTEGRITY_CHECK_FAILED);

    value = 0;
    for (size_t index = 0; index < sizeof(Integer); ++index)
    {
        value |= static_cast<Integer>(payload[index]) << (index * 8);
    }
    return CHIP_NO_ERROR;
}

template <typename Integer>
CHIP_ERROR WriteUnsigned(WindowsConfig::Key key, ValueType type, Integer value)
{
    std::array<uint8_t, sizeof(Integer)> payload{};
    for (size_t index = 0; index < payload.size(); ++index)
    {
        payload[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xFFu);
    }
    return WriteRecord(key, type, payload.data(), payload.size());
}

} // namespace

const char WindowsConfig::kConfigNamespace_ChipFactory[]  = "factory";
const char WindowsConfig::kConfigNamespace_ChipConfig[]   = "config";
const char WindowsConfig::kConfigNamespace_ChipCounters[] = "counter";

#define WINDOWS_CONFIG_KEY(name, configNamespace, keyName) const WindowsConfig::Key WindowsConfig::name = { configNamespace, keyName }

WINDOWS_CONFIG_KEY(kConfigKey_SerialNum, kConfigNamespace_ChipFactory, "serial-num");
WINDOWS_CONFIG_KEY(kConfigKey_MfrDeviceId, kConfigNamespace_ChipFactory, "device-id");
WINDOWS_CONFIG_KEY(kConfigKey_MfrDeviceCert, kConfigNamespace_ChipFactory, "device-cert");
WINDOWS_CONFIG_KEY(kConfigKey_MfrDeviceICACerts, kConfigNamespace_ChipFactory, "device-ca-certs");
WINDOWS_CONFIG_KEY(kConfigKey_MfrDevicePrivateKey, kConfigNamespace_ChipFactory, "device-key");
WINDOWS_CONFIG_KEY(kConfigKey_HardwareVersion, kConfigNamespace_ChipFactory, "hardware-ver");
WINDOWS_CONFIG_KEY(kConfigKey_ManufacturingDate, kConfigNamespace_ChipFactory, "mfg-date");
WINDOWS_CONFIG_KEY(kConfigKey_SetupPinCode, kConfigNamespace_ChipFactory, "pin-code");
WINDOWS_CONFIG_KEY(kConfigKey_SetupDiscriminator, kConfigNamespace_ChipFactory, "discriminator");
WINDOWS_CONFIG_KEY(kConfigKey_Spake2pIterationCount, kConfigNamespace_ChipFactory, "iteration-count");
WINDOWS_CONFIG_KEY(kConfigKey_Spake2pSalt, kConfigNamespace_ChipFactory, "salt");
WINDOWS_CONFIG_KEY(kConfigKey_Spake2pVerifier, kConfigNamespace_ChipFactory, "verifier");
WINDOWS_CONFIG_KEY(kConfigKey_VendorId, kConfigNamespace_ChipFactory, "vendor-id");
WINDOWS_CONFIG_KEY(kConfigKey_ProductId, kConfigNamespace_ChipFactory, "product-id");
WINDOWS_CONFIG_KEY(kConfigKey_UniqueId, kConfigNamespace_ChipConfig, "unique-id");
WINDOWS_CONFIG_KEY(kConfigKey_ServiceConfig, kConfigNamespace_ChipConfig, "service-config");
WINDOWS_CONFIG_KEY(kConfigKey_PairedAccountId, kConfigNamespace_ChipConfig, "account-id");
WINDOWS_CONFIG_KEY(kConfigKey_ServiceId, kConfigNamespace_ChipConfig, "service-id");
WINDOWS_CONFIG_KEY(kConfigKey_LastUsedEpochKeyId, kConfigNamespace_ChipConfig, "last-ek-id");
WINDOWS_CONFIG_KEY(kConfigKey_FailSafeArmed, kConfigNamespace_ChipConfig, "fail-safe-armed");
WINDOWS_CONFIG_KEY(kConfigKey_RegulatoryLocation, kConfigNamespace_ChipConfig, "regulatory-location");
WINDOWS_CONFIG_KEY(kConfigKey_CountryCode, kConfigNamespace_ChipConfig, "country-code");
WINDOWS_CONFIG_KEY(kConfigKey_LocationCapability, kConfigNamespace_ChipConfig, "location-capability");
WINDOWS_CONFIG_KEY(kConfigKey_ConfigurationVersion, kConfigNamespace_ChipConfig, "configuration-version");
WINDOWS_CONFIG_KEY(kCounterKey_RebootCount, kConfigNamespace_ChipCounters, "reboot-count");
WINDOWS_CONFIG_KEY(kCounterKey_UpTime, kConfigNamespace_ChipCounters, "up-time");
WINDOWS_CONFIG_KEY(kCounterKey_TotalOperationalHours, kConfigNamespace_ChipCounters, "total-operational-hours");
WINDOWS_CONFIG_KEY(kCounterKey_BootReason, kConfigNamespace_ChipCounters, "boot-reason");

#undef WINDOWS_CONFIG_KEY

bool WindowsConfig::Key::operator==(const Key & other) const
{
    return std::strcmp(Namespace, other.Namespace) == 0 && std::strcmp(Name, other.Name) == 0;
}

CHIP_ERROR WindowsConfig::Init(const char * storageRoot)
{
    return PersistedStorage::KeyValueStoreMgrImpl().Init(storageRoot);
}

void WindowsConfig::Shutdown()
{
    PersistedStorage::KeyValueStoreMgrImpl().Shutdown();
}

CHIP_ERROR WindowsConfig::ReadConfigValue(Key key, bool & value)
{
    std::vector<uint8_t> payload;
    ReturnErrorOnFailure(ReadRecord(key, ValueType::kBoolean, payload));
    VerifyOrReturnError(payload.size() == 1 && payload[0] <= 1, CHIP_ERROR_INTEGRITY_CHECK_FAILED);
    value = payload[0] != 0;
    return CHIP_NO_ERROR;
}

CHIP_ERROR WindowsConfig::ReadConfigValue(Key key, uint16_t & value)
{
    return ReadUnsigned(key, ValueType::kUInt16, value);
}

CHIP_ERROR WindowsConfig::ReadConfigValue(Key key, uint32_t & value)
{
    return ReadUnsigned(key, ValueType::kUInt32, value);
}

CHIP_ERROR WindowsConfig::ReadConfigValue(Key key, uint64_t & value)
{
    return ReadUnsigned(key, ValueType::kUInt64, value);
}

CHIP_ERROR WindowsConfig::ReadConfigValueStr(Key key, char * buffer, size_t bufferSize, size_t & outLength)
{
    std::vector<uint8_t> payload;
    CHIP_ERROR error = ReadRecord(key, ValueType::kString, payload);
    if (error != CHIP_NO_ERROR)
    {
        outLength = 0;
        return error;
    }
    VerifyOrReturnError(std::find(payload.begin(), payload.end(), 0) == payload.end(), CHIP_ERROR_INTEGRITY_CHECK_FAILED);
    outLength = payload.size();
    if (buffer == nullptr)
    {
        return CHIP_NO_ERROR;
    }
    VerifyOrReturnError(bufferSize > payload.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
    std::memcpy(buffer, payload.data(), payload.size());
    buffer[payload.size()] = '\0';
    return CHIP_NO_ERROR;
}

CHIP_ERROR WindowsConfig::ReadConfigValueBin(Key key, uint8_t * buffer, size_t bufferSize, size_t & outLength)
{
    std::vector<uint8_t> payload;
    CHIP_ERROR error = ReadRecord(key, ValueType::kBinary, payload);
    if (error != CHIP_NO_ERROR)
    {
        outLength = 0;
        return error;
    }
    outLength = payload.size();
    if (buffer == nullptr)
    {
        return CHIP_NO_ERROR;
    }
    const size_t copyLength = std::min(bufferSize, payload.size());
    std::memcpy(buffer, payload.data(), copyLength);
    return bufferSize < payload.size() ? CHIP_ERROR_BUFFER_TOO_SMALL : CHIP_NO_ERROR;
}

CHIP_ERROR WindowsConfig::WriteConfigValue(Key key, bool value)
{
    const uint8_t encoded = value ? 1 : 0;
    return WriteRecord(key, ValueType::kBoolean, &encoded, sizeof(encoded));
}

CHIP_ERROR WindowsConfig::WriteConfigValue(Key key, uint16_t value)
{
    return WriteUnsigned(key, ValueType::kUInt16, value);
}

CHIP_ERROR WindowsConfig::WriteConfigValue(Key key, uint32_t value)
{
    return WriteUnsigned(key, ValueType::kUInt32, value);
}

CHIP_ERROR WindowsConfig::WriteConfigValue(Key key, uint64_t value)
{
    return WriteUnsigned(key, ValueType::kUInt64, value);
}

CHIP_ERROR WindowsConfig::WriteConfigValueStr(Key key, const char * value)
{
    return value == nullptr ? ClearConfigValue(key) : WriteConfigValueStr(key, value, std::strlen(value));
}

CHIP_ERROR WindowsConfig::WriteConfigValueStr(Key key, const char * value, size_t valueLength)
{
    if (value == nullptr)
    {
        return ClearConfigValue(key);
    }
    VerifyOrReturnError(valueLength <= PersistedStorage::KeyValueStoreManagerImpl::kMaxValueLength - kTypeSize,
                        CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(std::memchr(value, '\0', valueLength) == nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    return WriteRecord(key, ValueType::kString, reinterpret_cast<const uint8_t *>(value), valueLength);
}

CHIP_ERROR WindowsConfig::WriteConfigValueBin(Key key, const uint8_t * value, size_t valueLength)
{
    return value == nullptr ? ClearConfigValue(key) : WriteRecord(key, ValueType::kBinary, value, valueLength);
}

CHIP_ERROR WindowsConfig::ClearConfigValue(Key key)
{
    std::string storageKey;
    ReturnErrorOnFailure(BuildStorageKey(key, storageKey));
    const CHIP_ERROR error = PersistedStorage::KeyValueStoreMgr().Delete(storageKey.c_str());
    return error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND ? CHIP_NO_ERROR : error;
}

bool WindowsConfig::ConfigValueExists(Key key)
{
    std::string storageKey;
    if (BuildStorageKey(key, storageKey) != CHIP_NO_ERROR)
    {
        return false;
    }
    size_t ignored = 0;
    return PersistedStorage::KeyValueStoreMgrImpl().GetValueSize(storageKey.c_str(), ignored) == CHIP_NO_ERROR;
}

CHIP_ERROR WindowsConfig::EnsureNamespace(const char * configNamespace)
{
    VerifyOrReturnError(configNamespace != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    return std::strcmp(configNamespace, kConfigNamespace_ChipFactory) == 0 ||
            std::strcmp(configNamespace, kConfigNamespace_ChipConfig) == 0 ||
            std::strcmp(configNamespace, kConfigNamespace_ChipCounters) == 0
        ? CHIP_NO_ERROR
        : CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR WindowsConfig::ClearNamespace(const char * configNamespace)
{
    VerifyOrReturnError(configNamespace != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    if (std::strcmp(configNamespace, kConfigNamespace_ChipConfig) == 0)
    {
        return PersistedStorage::KeyValueStoreMgrImpl().ClearPrefix("config/");
    }
    if (std::strcmp(configNamespace, kConfigNamespace_ChipCounters) == 0)
    {
        return PersistedStorage::KeyValueStoreMgrImpl().ClearPrefix("counter/");
    }
    return CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR WindowsConfig::FactoryResetConfig()
{
    return ClearNamespace(kConfigNamespace_ChipConfig);
}

CHIP_ERROR WindowsConfig::FactoryResetCounters()
{
    return ClearNamespace(kConfigNamespace_ChipCounters);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
