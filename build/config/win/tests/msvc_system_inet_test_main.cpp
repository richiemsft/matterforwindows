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

// GoogleTest entry point for the native-MSVC upstream System/Inet test
// executables.
//
// Unlike the crypto harness, this main deliberately does NOT initialize the
// CHIP memory allocator. The upstream System/Inet test fixtures that allocate
// (TestSystemPacketBuffer, TestTLVPacketBufferBackingStore, TestSystemTimer,
// TestBasicPacketFilters, TestInetEndPoint, ...) call chip::Platform::MemoryInit
// in their own SetUpTestSuite and MemoryShutdown in TearDownTestSuite. Because
// MemoryInit is not reference-counted (a debug build aborts on a second Init),
// a MemoryInit here would double-initialize. The pure tests (error strings,
// clock, time source, IP address math) allocate nothing and need no allocator.

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>

#include <platform/logging/LogV.h>

namespace chip {
namespace Logging {
namespace Platform {

// Minimal platform logging backend for the Windows System/Inet test harness.
// The canonical support library compiles TextOnlyLogging with logging enabled,
// so a Platform::LogV() implementation must be provided. Route messages to
// stderr. This performs no dynamic allocation, so it is safe to call before or
// after any per-suite MemoryInit/MemoryShutdown.
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
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
