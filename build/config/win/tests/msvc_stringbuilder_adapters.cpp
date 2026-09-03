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

// GoogleTest pretty-printers for the Windows crypto test executables. These
// mirror the CHIP_CONFIG_TEST_GOOGLETEST branch of
// src/lib/core/StringBuilderAdapters.cpp verbatim, but without the Pigweed
// pw_string ToString<> specializations that the light unit-test framework
// needs and that do not compile under MSVC. See the facade header in
// crypto_test_support/lib/core/StringBuilderAdapters.h for rationale.

#include <lib/core/StringBuilderAdapters.h>

namespace chip {

void PrintTo(const CHIP_ERROR & err, std::ostream * os)
{
    if (CHIP_ERROR::IsSuccess(err))
    {
        *os << "CHIP_NO_ERROR";
        return;
    }
    *os << "CHIP_ERROR:<" << err.Format() << ">";
}

void PrintTo(const std::chrono::duration<uint64_t, std::milli> & time, std::ostream * os)
{
    *os << time.count() << "ms";
}

void PrintTo(const std::chrono::duration<uint64_t, std::micro> & time, std::ostream * os)
{
    *os << time.count() << "us";
}

} // namespace chip
