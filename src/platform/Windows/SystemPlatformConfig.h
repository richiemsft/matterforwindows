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
 *          Platform-specific configuration overrides for the CHIP System
 *          Layer on native Windows hosts.
 */

#pragma once

#include <stdint.h>

namespace chip {
namespace DeviceLayer {
struct ChipDeviceEvent;
} // namespace DeviceLayer
} // namespace chip

// ==================== Platform Adaptations ====================

// Windows uses native SRWLOCK-based locking (selected automatically by
// SystemConfig.h from CHIP_SYSTEM_CONFIG_WINDOWS_LOCKING on _WIN32) and the
// native QPC/FILETIME clock rather than POSIX time functions. No POSIX time
// facilities are available on the native host.
#ifndef CHIP_SYSTEM_CONFIG_PLATFORM_PROVIDES_TIME
#define CHIP_SYSTEM_CONFIG_PLATFORM_PROVIDES_TIME 0
#endif

#ifndef CHIP_SYSTEM_CONFIG_USE_POSIX_TIME_FUNCTS
#define CHIP_SYSTEM_CONFIG_USE_POSIX_TIME_FUNCTS 0
#endif

// The event and timer pools are backed by the heap on a full desktop host.
#ifndef CHIP_SYSTEM_CONFIG_POOL_USE_HEAP
#define CHIP_SYSTEM_CONFIG_POOL_USE_HEAP 1
#endif
