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

// Empty <unistd.h> shim for the Windows crypto test executables. The upstream
// src/crypto/tests/TestChipCryptoPAL.cpp includes <unistd.h> but does not call
// any POSIX file API from it; MSVC has no <unistd.h>. This shim lets the
// upstream test body compile unmodified. It is only on the include path for the
// crypto test executables.
