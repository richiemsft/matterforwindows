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
#pragma once

// Windows/MSVC facade for <lib/core/StringBuilderAdapters.h>.
//
// The upstream header pulls in Pigweed's pw_string closure (pw_string ->
// pw_result / pw_containers / Fuchsia stdcompat <bit>), which relies on GCC
// builtins (__builtin_clzll, __BYTE_ORDER__, ...) that MSVC does not provide.
// That closure only teaches Pigweed's *light* unit-test framework how to
// stringize CHIP types. The Windows System/Inet tests run on GoogleTest, which
// only needs the chip::PrintTo() overloads below (implemented with
// CHIP_ERROR::Format() and std::ostream, requiring no Pigweed machinery).
//
// This facade is placed ahead of //src on the include path for the Windows
// System/Inet test executables (see //build/config/win/tests). The upstream
// test sources are compiled verbatim; only the GoogleTest diagnostic adapter
// differs from the canonical (Pigweed-backed) one. Mirrors the crypto facade
// in crypto_test_support/lib/core/StringBuilderAdapters.h.

#include <chrono>
#include <cstdint>
#include <ostream>

#include <lib/core/CHIPError.h>

namespace chip {

void PrintTo(const CHIP_ERROR & err, std::ostream * os);
void PrintTo(const std::chrono::duration<uint64_t, std::milli> & time, std::ostream * os);
void PrintTo(const std::chrono::duration<uint64_t, std::micro> & time, std::ostream * os);

} // namespace chip
