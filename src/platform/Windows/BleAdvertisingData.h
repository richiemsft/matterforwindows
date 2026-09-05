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
 *          Parses and encodes the raw bytes of the BLE "Service Data - 16-bit
 *          UUID" (AD type 0x16) advertising structure used to announce the
 *          Matter/CHIPoBLE service. Reuses the shared, platform-agnostic
 *          chip::Ble::ChipBLEDeviceIdentificationInfo wire-format struct
 *          (src/ble/CHIPBleServiceData.h) for the payload itself; this file
 *          only concerns itself with the raw byte layout WinRT hands back
 *          from/expects for a BluetoothLEAdvertisementDataSection, so it has
 *          no WinRT/COM dependency and can be exercised deterministically
 *          without Bluetooth hardware.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <ble/Ble.h>
#include <lib/support/Span.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

// The 16-bit Bluetooth SIG UUID for the Matter primary service
// (chip::Ble::CHIP_BLE_SERVICE_SHORT_UUID_STR == "fff6"), as it appears
// little-endian-encoded in the two-byte UUID prefix of a BLE "Service Data -
// 16-bit UUID" advertising structure.
inline constexpr uint16_t kChipBleServiceUuid16 = 0xFFF6;

// Total length of a Matter "Service Data - 16-bit UUID" structure payload:
// the two little-endian UUID bytes immediately followed by the CHIPoBLE
// device identification information block.
inline constexpr size_t kChipServiceDataSectionSize = sizeof(uint16_t) + sizeof(Ble::ChipBLEDeviceIdentificationInfo);
inline constexpr size_t kChipServiceDataPayloadSize = sizeof(Ble::ChipBLEDeviceIdentificationInfo);

/**
 * Parses a raw BLE "Service Data - 16-bit UUID" advertising structure payload
 * -- exactly the bytes a BluetoothLEAdvertisementDataSection whose DataType is
 * BluetoothLEAdvertisementDataTypes::ServiceData16BitUuids carries in its
 * Data buffer -- into a ChipBLEDeviceIdentificationInfo.
 *
 * Returns false (leaving outInfo untouched) if the section's UUID does not
 * match the Matter service or the section is not exactly
 * kChipServiceDataSectionSize bytes long. Both are expected, non-error
 * conditions during a scan: most BLE advertisements observed are not Matter
 * devices.
 */
bool ParseChipServiceData(ByteSpan sectionPayload, Ble::ChipBLEDeviceIdentificationInfo & outInfo);

/**
 * Serializes a ChipBLEDeviceIdentificationInfo into a raw "Service Data -
 * 16-bit UUID" advertising structure payload (the inverse of
 * ParseChipServiceData), writing exactly kChipServiceDataSectionSize bytes.
 */
void EncodeChipServiceData(const Ble::ChipBLEDeviceIdentificationInfo & info, uint8_t (&outBuffer)[kChipServiceDataSectionSize]);

/**
 * Serializes only the service-specific payload. Windows adds the service UUID
 * when GattServiceProviderAdvertisingParameters::ServiceData is set.
 */
void EncodeChipServiceDataPayload(const Ble::ChipBLEDeviceIdentificationInfo & info,
                                  uint8_t (&outBuffer)[kChipServiceDataPayloadSize]);

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
