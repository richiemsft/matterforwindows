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

// Small POSIX string-comparison shim for the upstream Inet/System test sources
// compiled under MSVC. It is force-included (/FI) into the affected test
// translation units so their bodies stay verbatim; the CHIP libraries under
// test are not affected. MSVC spells these POSIX functions with a leading
// underscore.

#include <string.h>

static inline int strcasecmp(const char * a, const char * b)
{
    return _stricmp(a, b);
}

static inline int strncasecmp(const char * a, const char * b, size_t n)
{
    return _strnicmp(a, b, n);
}
