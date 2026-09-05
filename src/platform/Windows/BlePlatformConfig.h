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
 *          Platform-specific configuration overrides for the CHIP BLE
 *          Layer on native Windows hosts.
 */

#pragma once

namespace chip {
namespace DeviceLayer {
namespace Internal {
class WinRTBleConnection;
} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

// ==================== Platform Adaptations ====================

// The native BLE backend is implemented with C++/WinRT
// (Windows.Devices.Bluetooth). BLE_CONNECTION_OBJECT is a stable,
// heap-allocated WinRTBleConnection owned by BLEManagerImpl for the life of
// the connection; see src/platform/Windows/BleConnection.h.
#define BLE_CONNECTION_OBJECT chip::DeviceLayer::Internal::WinRTBleConnection *
#define BLE_CONNECTION_UNINITIALIZED nullptr

// ========== Platform-specific Configuration Overrides =========

/* none so far */
