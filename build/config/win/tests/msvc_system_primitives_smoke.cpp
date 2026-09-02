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
#include <system/windows/WindowsMutex.h>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <thread>

int main()
{
    const uint64_t monotonicBefore = chip::System::Internal::WindowsClock::GetMonotonicMicroseconds();
    Sleep(2);
    const uint64_t monotonicAfter = chip::System::Internal::WindowsClock::GetMonotonicMicroseconds();
    if (monotonicAfter <= monotonicBefore)
    {
        return 1;
    }

    constexpr uint64_t kJanuary2020UnixMicroseconds = 1577836800000000;
    const auto realTime = chip::System::Internal::WindowsClock::GetRealTimeMicroseconds();
    if (!realTime.has_value() || realTime.value() < kJanuary2020UnixMicroseconds)
    {
        return 1;
    }
    if (chip::System::Internal::WindowsClock::FileTimeToUnixMicroseconds(0).has_value())
    {
        return 1;
    }

    chip::System::Internal::WindowsMutex mutex;
    uint32_t counter = 0;
    std::array<std::thread, 4> threads;
    for (auto & thread : threads)
    {
        thread = std::thread([&mutex, &counter]() {
            for (uint32_t i = 0; i < 10000; ++i)
            {
                std::lock_guard<chip::System::Internal::WindowsMutex> lock(mutex);
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
