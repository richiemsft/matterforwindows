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

#include <system/windows/WindowsClock.h>

#include <Windows.h>

namespace chip {
namespace System {
namespace Internal {
namespace {

constexpr uint64_t kMicrosecondsPerSecond        = 1000000;
constexpr uint64_t kFileTimeTicksPerMicrosecond = 10;
constexpr uint64_t kWindowsToUnixEpochTicks      = 116444736000000000;

} // namespace

uint64_t WindowsClock::GetMonotonicMicroseconds()
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    const uint64_t ticks     = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t ticksPerS = static_cast<uint64_t>(frequency.QuadPart);
    return (ticks / ticksPerS) * kMicrosecondsPerSecond +
        ((ticks % ticksPerS) * kMicrosecondsPerSecond) / ticksPerS;
}

std::optional<uint64_t> WindowsClock::GetRealTimeMicroseconds()
{
    FILETIME fileTime;
    GetSystemTimePreciseAsFileTime(&fileTime);

    ULARGE_INTEGER ticks;
    ticks.LowPart  = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return FileTimeToUnixMicroseconds(ticks.QuadPart);
}

std::optional<uint64_t> WindowsClock::FileTimeToUnixMicroseconds(uint64_t fileTimeTicks)
{
    if (fileTimeTicks < kWindowsToUnixEpochTicks)
    {
        return std::nullopt;
    }
    return (fileTimeTicks - kWindowsToUnixEpochTicks) / kFileTimeTicksPerMicrosecond;
}

} // namespace Internal
} // namespace System
} // namespace chip
