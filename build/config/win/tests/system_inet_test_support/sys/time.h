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

// Windows/MSVC facade for the POSIX <sys/time.h>.
//
// The Inet test helper header (src/inet/tests/TestInetCommon.h) includes
// <sys/time.h> unconditionally. MSVC has no such header; the symbols the
// helper actually needs (clock/time helpers, and struct timeval when a socket
// path is involved) come from <time.h> and WinSock. Provide the minimal set so
// the upstream helper/test sources compile verbatim on Windows.

#include <time.h>
#include <winsock2.h> // struct timeval
