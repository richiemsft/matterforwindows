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
#include <system/SystemPacketBuffer.h>

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
 *
 * These marshal a native C++/WinRT BLE callback (central connect/connect-failure,
 * write/subscribe completion, TX indication receipt, and peripheral GATT
 * server register/advertising completion) onto PlatformMgr() so BLEManagerImpl
 * only ever reacts to them on the Matter event-loop thread. See
 * src/platform/Windows/BLEManagerImpl.cpp.
 */
enum InternalPlatformSpecificEventTypes
{
    kPlatformWindowsEvent = kRange_InternalPlatformSpecific,
    kPlatformWindowsBLECentralConnected,
    kPlatformWindowsBLECentralConnectFailed,
    kPlatformWindowsBLEWriteComplete,
    kPlatformWindowsBLESubscribeOpComplete,
    kPlatformWindowsBLEIndicationReceived,
    kPlatformWindowsBLEPeripheralRegisterAppComplete,
    kPlatformWindowsBLEPeripheralAdvStartComplete,
    kPlatformWindowsBLEPeripheralAdvStopComplete,
};

} // namespace DeviceEventType

/**
 * Represents platform-specific event information for native Windows hosts.
 */
struct ChipDevicePlatformEvent
{
    union
    {
        struct
        {
            BLE_CONNECTION_OBJECT mConnection;
        } BLECentralConnected;
        struct
        {
            CHIP_ERROR mError;
        } BLECentralConnectFailed;
        struct
        {
            BLE_CONNECTION_OBJECT mConnection;
        } BLEWriteComplete;
        struct
        {
            BLE_CONNECTION_OBJECT mConnection;
            bool mIsSubscribed;
        } BLESubscribeOpComplete;
        struct
        {
            BLE_CONNECTION_OBJECT mConnection;
            chip::System::PacketBuffer * mData;
        } BLEIndicationReceived;
        struct
        {
            CHIP_ERROR mError;
        } BLEPeripheralRegisterAppComplete;
        struct
        {
            CHIP_ERROR mError;
        } BLEPeripheralAdvStartComplete;
        struct
        {
            CHIP_ERROR mError;
        } BLEPeripheralAdvStopComplete;
        // Kept so this union is never empty on non-BLE builds; harmless otherwise.
        uint32_t Unused;
    };
};

} // namespace DeviceLayer
} // namespace chip
