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
 *          Defines platform-specific event types and data for the chip
 *          Device Layer on native Windows hosts.
 */

#pragma once

#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceEvent.h>

namespace chip {
namespace DeviceLayer {

namespace DeviceEventType {

/**
 * Enumerates Windows platform-specific event types that are visible to the application.
 */
enum PublicPlatformSpecificEventTypes
{
    /* None currently defined */
};

/**
 * Enumerates Windows platform-specific event types that are internal to the chip Device Layer.
 */
enum InternalPlatformSpecificEventTypes
{
    kPlatformWindowsEvent = kRange_InternalPlatformSpecific,
};

} // namespace DeviceEventType

/**
 * Represents platform-specific event information for native Windows hosts.
 *
 * No connectivity, BLE, or discovery events are defined for the Phase 3
 * foundation. A placeholder member keeps the aggregate a complete type for the
 * shared ChipDeviceEvent union without implying any feature.
 */
struct ChipDevicePlatformEvent
{
    uint32_t Unused;
};

} // namespace DeviceLayer
} // namespace chip
