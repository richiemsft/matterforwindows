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

// GoogleTest entry point for the native-MSVC upstream crypto test executables.
// Initializes the CHIP memory allocator (required by the CryptoPAL and support
// libraries) before running the upstream src/crypto/tests suites.

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>

#include <lib/support/CHIPMem.h>
#include <platform/logging/LogV.h>

namespace chip {
namespace Logging {
namespace Platform {

// Minimal platform logging backend for the Windows crypto test harness. The
// canonical support library compiles TextOnlyLogging with logging enabled, so a
// Platform::LogV() implementation must be provided. Route messages to stderr.
void LogV(const char * module, uint8_t category, const char * msg, va_list v)
{
    (void) category;
    std::fprintf(stderr, "[%s] ", module);
    std::vfprintf(stderr, msg, v);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace Platform
} // namespace Logging
} // namespace chip

int main(int argc, char ** argv)
{
    if (chip::Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    chip::Platform::MemoryShutdown();
    return result;
}
