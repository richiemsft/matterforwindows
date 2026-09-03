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

// Windows/MSVC facade for the POSIX <netinet/in.h>.
//
// The upstream Inet address test (src/inet/tests/TestInetAddress.cpp) includes
// <netinet/in.h> and <sys/socket.h> on non-LwIP builds to reach struct in_addr,
// struct in6_addr, struct sockaddr_in{,6}, AF_INET{,6}, and htonl/htons. On
// Windows those types come from WinSock, which also provides the s_addr /
// s6_addr member macros the test uses. Include WinSock ahead of any <windows.h>.

#include <winsock2.h>
#include <ws2tcpip.h>
