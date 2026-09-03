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
 *          Platform-specific configuration overrides for CHIP on native
 *          Windows hosts.
 */

#pragma once

// ==================== General Platform Adaptations ====================

#define CHIP_CONFIG_ABORT() abort()

using CHIP_CONFIG_PERSISTED_STORAGE_KEY_TYPE = const char *;
#define CHIP_CONFIG_PERSISTED_STORAGE_MAX_KEY_LENGTH 16

#define CHIP_CONFIG_LIFETIIME_PERSISTED_COUNTER_KEY "life-count"

#define CHIP_CONFIG_ERROR_FORMAT_AS_STRING 1
#define CHIP_CONFIG_ERROR_SOURCE 1

#define CHIP_CONFIG_VERBOSE_VERIFY_OR_DIE 1

#define CHIP_CONFIG_IM_STATUS_CODE_VERBOSE_FORMAT 1

// ==================== Security Adaptations ====================

// If unspecified, assume crypto is fast on a desktop Windows host.
#ifndef CHIP_CONFIG_SLOW_CRYPTO
#define CHIP_CONFIG_SLOW_CRYPTO 0
#endif // CHIP_CONFIG_SLOW_CRYPTO

// ==================== General Configuration Overrides ====================

#ifndef CHIP_CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS
#define CHIP_CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS 8
#endif // CHIP_CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS

#ifndef CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS
#define CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS 8
#endif // CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS

#ifndef CHIP_LOG_FILTERING
#define CHIP_LOG_FILTERING 1
#endif // CHIP_LOG_FILTERING

// Size the C++ lambda event to hold local captures on an [I]LP64/LLP64 host,
// where pointers are 8 bytes wide.
#ifndef CHIP_CONFIG_LAMBDA_EVENT_SIZE
#define CHIP_CONFIG_LAMBDA_EVENT_SIZE (48)
#endif // CHIP_CONFIG_LAMBDA_EVENT_SIZE
