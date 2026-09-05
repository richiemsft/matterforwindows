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

#include <platform/Windows/BleAdvertisingData.h>

#include <cstring>

#include <lib/core/CHIPEncoding.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

bool ParseChipServiceData(ByteSpan sectionPayload, Ble::ChipBLEDeviceIdentificationInfo & outInfo)
{
    if (sectionPayload.size() != kChipServiceDataSectionSize)
    {
        return false;
    }

    const uint8_t * bytes = sectionPayload.data();
    uint16_t uuid16       = chip::Encoding::LittleEndian::Get16(bytes);
    if (uuid16 != kChipBleServiceUuid16)
    {
        return false;
    }

    memcpy(&outInfo, bytes + sizeof(uint16_t), sizeof(outInfo));
    return true;
}

void EncodeChipServiceData(const Ble::ChipBLEDeviceIdentificationInfo & info, uint8_t (&outBuffer)[kChipServiceDataSectionSize])
{
    chip::Encoding::LittleEndian::Put16(outBuffer, kChipBleServiceUuid16);
    uint8_t payload[kChipServiceDataPayloadSize];
    EncodeChipServiceDataPayload(info, payload);
    memcpy(outBuffer + sizeof(uint16_t), payload, sizeof(payload));
}

void EncodeChipServiceDataPayload(const Ble::ChipBLEDeviceIdentificationInfo & info,
                                  uint8_t (&outBuffer)[kChipServiceDataPayloadSize])
{
    memcpy(outBuffer, &info, sizeof(info));
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
