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

#include <system/SystemClock.h>
#include <system/SystemError.h>
#include <system/SystemMutex.h>
#include <system/windows/WindowsClock.h>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

int main()
{
    const uint64_t monotonicBefore = chip::System::SystemClock().GetMonotonicMicroseconds64().count();
    Sleep(2);
    const uint64_t monotonicAfter = chip::System::SystemClock().GetMonotonicMicroseconds64().count();
    if (monotonicAfter <= monotonicBefore)
    {
        return 1;
    }

    constexpr uint64_t kJanuary2020UnixMicroseconds = 1577836800000000;
    chip::System::Clock::Microseconds64 realTime;
    if (chip::System::SystemClock().GetClock_RealTime(realTime) != CHIP_NO_ERROR ||
        realTime.count() < kJanuary2020UnixMicroseconds)
    {
        return 1;
    }
    if (chip::System::Internal::WindowsClock::FileTimeToUnixMicroseconds(0).has_value())
    {
        return 1;
    }

    if (CHIP_ERROR_WINDOWS(ERROR_SUCCESS) != CHIP_NO_ERROR ||
        CHIP_ERROR_HRESULT(S_FALSE) != CHIP_NO_ERROR)
    {
        return 2;
    }

    const CHIP_ERROR accessDenied = CHIP_ERROR_WINDOWS(ERROR_ACCESS_DENIED);
    const char * accessDeniedDescription = chip::System::DescribeErrorWindows(accessDenied);
    const size_t accessDeniedLength       = std::strlen(accessDeniedDescription);
    if (!accessDenied.IsRange(chip::ChipError::Range::kOS) ||
        accessDenied.GetValue() != ERROR_ACCESS_DENIED ||
        accessDeniedLength == 0 || accessDeniedDescription[accessDeniedLength - 1] == '\r' ||
        accessDeniedDescription[accessDeniedLength - 1] == '\n' ||
        CHIP_ERROR_HRESULT(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) != accessDenied)
    {
        return 3;
    }

    const CHIP_ERROR genericFailure = CHIP_ERROR_HRESULT(E_FAIL);
    if (!genericFailure.IsRange(chip::ChipError::Range::kPlatformExtended) ||
        chip::System::GetHRESULT(genericFailure) != static_cast<int32_t>(E_FAIL))
    {
        return 4;
    }

    const CHIP_ERROR unknownError = CHIP_ERROR_HRESULT(0x81234567);
    if (std::strcmp(chip::System::DescribeErrorWindows(unknownError), "Unknown Windows error") != 0 ||
        chip::System::GetWindowsError(CHIP_ERROR_WINDOWS(0x00FFFFFF)) != 0x00FFFFFF ||
        CHIP_ERROR_WINDOWS(0x01000000) != CHIP_ERROR_INVALID_ARGUMENT)
    {
        return 5;
    }

    chip::System::RegisterWindowsErrorFormatter();
    chip::System::RegisterWindowsErrorFormatter();
    char formattedError[256];
    if (!chip::System::FormatWindowsError(formattedError, sizeof(formattedError), genericFailure) ||
        std::strstr(formattedError, "HRESULT") == nullptr ||
        chip::System::FormatWindowsError(formattedError, sizeof(formattedError), CHIP_ERROR_INVALID_ARGUMENT))
    {
        return 6;
    }

    chip::System::Mutex mutex;
    if (chip::System::Mutex::Init(mutex) != CHIP_NO_ERROR)
    {
        return 1;
    }
    uint32_t counter = 0;
    std::array<std::thread, 4> threads;
    for (auto & thread : threads)
    {
        thread = std::thread([&mutex, &counter]() {
            for (uint32_t i = 0; i < 10000; ++i)
            {
                std::lock_guard<chip::System::Mutex> lock(mutex);
                ++counter;
            }
        });
    }
    for (auto & thread : threads)
    {
        thread.join();
    }

    return counter == 40000 ? 0 : 1;
}
