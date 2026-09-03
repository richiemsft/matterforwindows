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

// Windows/MSVC facade for <platform/CHIPDeviceLayer.h>.
//
// Several upstream System-layer unit tests (TestSystemPacketBuffer, ...)
// include <platform/CHIPDeviceLayer.h> solely to reach the CHIP memory
// allocator (chip::Platform::MemoryInit / MemoryShutdown / MemoryAlloc /
// MemoryFree), which actually lives in <lib/support/CHIPMem.h>. The canonical
// header pulls the whole Device Layer (PlatformManager, ...), which is not part
// of the Phase 2 Windows port.
//
// The Windows test executables that use this facade only enable the subset of
// upstream tests that need nothing beyond the allocator (the tests that drive
// DeviceLayer::PlatformMgr() / DeviceLayer::SystemLayer() for a real event loop
// are intentionally not built on Windows until the Device Layer is ported).
// Providing just the allocator here keeps those upstream test sources
// compilable verbatim.

#include <cstdlib>

#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>

namespace chip {
namespace DeviceLayer {

// Minimal PlatformManager stand-in. Some upstream System-layer tests
// (TestSystemPacketBuffer) call PlatformMgr().InitChipStack() from their
// SetUpTestSuite even though the test bodies only exercise the packet-buffer
// allocator (initialized separately via chip::Platform::MemoryInit). The
// Windows Device Layer is not part of Phase 2, so this stub satisfies that
// incidental lifecycle call without pulling the Device Layer. It intentionally
// does nothing: the tests linked against it never depend on a running platform.
class PlatformManagerStub
{
public:
    CHIP_ERROR InitChipStack() { return CHIP_NO_ERROR; }
    void Shutdown() {}
};

inline PlatformManagerStub & PlatformMgr()
{
    static PlatformManagerStub sPlatformManager;
    return sPlatformManager;
}

} // namespace DeviceLayer
} // namespace chip

// MSVC's C runtime has no POSIX random()/srandom(). TestSystemPacketBuffer uses
// them only to fill buffers with pseudo-random bytes for round-trip checks, so
// map them to the C runtime rand()/srand(). Internal linkage keeps this local
// to the single translation unit that includes this facade.
static inline long random()
{
    return static_cast<long>(::rand());
}

static inline void srandom(unsigned int seed)
{
    ::srand(seed);
}

