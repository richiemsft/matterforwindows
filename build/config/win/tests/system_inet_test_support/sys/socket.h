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

// Windows/MSVC facade for the POSIX <sys/socket.h>.
//
// The upstream Inet address test includes <sys/socket.h> alongside
// <netinet/in.h> on non-LwIP builds. On Windows the sockaddr / address families
// live in WinSock. Include WinSock ahead of any <windows.h>.

#include <winsock2.h>
#include <ws2tcpip.h>
