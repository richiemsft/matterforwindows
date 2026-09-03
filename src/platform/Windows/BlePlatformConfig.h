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

// ==================== Platform Adaptations ====================

// The native BLE backend (C++/WinRT Windows.Devices.Bluetooth) is scheduled for
// a later phase. Until a concrete connection object type exists, the BLE
// connection handle resolves to a plain pointer with a null sentinel so that
// shared headers referring to BLE_CONNECTION_OBJECT still compile. No BLE
// transport is provided by this foundation.
#define BLE_CONNECTION_OBJECT void *
#define BLE_CONNECTION_UNINITIALIZED nullptr

// ========== Platform-specific Configuration Overrides =========

/* none so far */
