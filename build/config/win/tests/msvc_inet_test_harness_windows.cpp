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

// Native-Windows harness for the upstream Inet EndPoint unit test
// (src/inet/tests/TestInetEndPoint.cpp).
//
// The upstream test drives the test globals and lifecycle helpers declared in
// src/inet/tests/TestInetCommon.h (gSystemLayer, gUDP, gTCP, InitSystemLayer,
// InitNetwork, ...). The canonical implementation of that contract,
// TestInetCommonPosix.cpp, is entangled with POSIX-only includes (<unistd.h>,
// <signal.h> via TestSetupSignalling) and, in its LwIP branch, the Device
// Layer PlatformManager. None of that is available in the Phase 2 Windows port.
//
// Rather than shim a large POSIX surface, this file provides a small,
// sockets-only reimplementation of the same TestInetCommon.h / TestSetupSignalling.h
// contract against the native Windows System layer (System::LayerImpl ==
// LayerImplWindows) and the shared WinSock UDP/TCP endpoint managers. It mirrors
// the socket path of TestInetCommonPosix.cpp; the upstream test source itself is
// compiled verbatim.

#include "TestInetCommon.h"
#include "TestSetupSignalling.h"

#include <cstdio>
#include <cstdlib>

#include <inet/InetConfig.h>
#include <lib/core/ErrorStr.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <system/SystemClock.h>

using namespace chip;
using namespace chip::Inet;

System::LayerImpl gSystemLayer;

#if INET_CONFIG_ENABLE_UDP_ENDPOINT
Inet::UDPEndPointManagerImpl gUDP;
#endif

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
Inet::TCPEndPointManagerImpl gTCP;
#endif

bool gDone = false;

void InetFailError(CHIP_ERROR err, const char * msg)
{
    if (err != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "%s: %s\n", msg, ErrorStr(err));
        std::exit(-1);
    }
}

void InitTestInetCommon()
{
    SuccessOrDie(chip::Platform::MemoryInit());
}

void ShutdownTestInetCommon()
{
    chip::Platform::MemoryShutdown();
}

void InitSystemLayer()
{
    SuccessOrDie(gSystemLayer.Init());
}

void ShutdownSystemLayer()
{
    gSystemLayer.Shutdown();
}

void InitNetwork()
{
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    SuccessOrDie(gTCP.Init(gSystemLayer));
#endif
#if INET_CONFIG_ENABLE_UDP_ENDPOINT
    SuccessOrDie(gUDP.Init(gSystemLayer));
#endif
}

void ServiceEvents(uint32_t aSleepTimeMilliseconds)
{
    // Bound WaitForEvents() so it never blocks past the requested interval.
    SuccessOrDie(gSystemLayer.StartTimer(
        System::Clock::Milliseconds32(aSleepTimeMilliseconds), [](System::Layer *, void *) -> void {}, nullptr));

    gSystemLayer.PrepareEvents();
    gSystemLayer.WaitForEvents();
    gSystemLayer.HandleEvents();
}

void ShutdownNetwork()
{
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    gTCP.Shutdown();
#endif
#if INET_CONFIG_ENABLE_UDP_ENDPOINT
    gUDP.Shutdown();
#endif
}

void DumpMemory(const uint8_t * mem, uint32_t len, const char * prefix, uint32_t rowWidth)
{
    (void) mem;
    (void) len;
    (void) prefix;
    (void) rowWidth;
}

void DumpMemory(const uint8_t * mem, uint32_t len, const char * prefix)
{
    (void) mem;
    (void) len;
    (void) prefix;
}

// TestSetupSignalling.h contract. The upstream POSIX implementation installs a
// SIGUSR1 handler so an external harness can terminate a long-running tool.
// The unit test only calls SetSIGUSR1Handler() from SetUpTestSuite; on Windows
// there is no SIGUSR1 and no external signaller, so these are no-ops.
void SetSIGUSR1Handler() {}

void SetSignalHandler(SignalHandler handler)
{
    (void) handler;
}
