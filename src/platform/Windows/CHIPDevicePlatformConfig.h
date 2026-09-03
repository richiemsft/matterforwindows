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
 *          Platform-specific configuration overrides for the chip Device Layer
 *          on native Windows hosts.
 *
 *          This is the Phase 3 foundation configuration. The connectivity,
 *          persistence, DNS-SD, BLE, and Thread features are concretely
 *          disabled here rather than stubbed: the corresponding managers are
 *          not part of this milestone and enabling their macros would require
 *          implementations that do not yet exist.
 */

#pragma once

// ==================== Platform Adaptations ====================

// Wi-Fi is provided by OS-managed adapters in the first release; there is no
// station/AP management surface in the Device Layer foundation.
#ifndef CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION 0
#endif

#ifndef CHIP_DEVICE_CONFIG_ENABLE_WIFI_AP
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_AP 0
#endif

// Thread is reached through an external border router; there is no native
// Thread stack (matching the Darwin host model).
#ifndef CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD 0
#endif

// CHIPoBLE requires the WinRT backend that is scheduled for a later phase.
#ifndef CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
#define CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE 0
#endif

// ========== Platform-specific Configuration Overrides =========

#ifndef CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE
#define CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE 8192
#endif // CHIP_DEVICE_CONFIG_CHIP_TASK_STACK_SIZE

#ifndef CHIP_DEVICE_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS
#define CHIP_DEVICE_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS 1
#endif // CHIP_DEVICE_CONFIG_EVENT_LOGGING_UTC_TIMESTAMPS

#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_TELEMETRY 0
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD_TELEMETRY 0
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD_TELEMETRY_FULL 0
