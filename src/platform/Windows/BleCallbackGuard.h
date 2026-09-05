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

/**
 *    @file
 *          A dependency-free generation counter used to invalidate WinRT
 *          BLE callbacks that complete after the operation they belong to
 *          has been superseded, cancelled, or the owning object has shut
 *          down/closed. This header has no WinRT/COM dependency so its
 *          invalidation semantics can be exercised deterministically without
 *          Bluetooth hardware (see the BLE smoke test).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace chip {
namespace DeviceLayer {
namespace Internal {

/**
 * Shared generation counter owned by a long-lived object (the BLE manager, a
 * central scan/connect attempt, or an individual connection). Every
 * asynchronous WinRT operation issued while a particular "epoch" is current
 * captures a BleCallbackGuard (via MakeGuard()) by value into its completion
 * handler. Invalidate() -- called on cancellation, Close(), or Shutdown() --
 * bumps the epoch so every previously issued guard's IsValid() call starts
 * returning false. A completion handler must check IsValid() before touching
 * any state it captured or invoking a Matter callback/posting a
 * ChipDeviceEvent.
 */
class BleCallbackEpoch
{
public:
    class Guard
    {
    public:
        Guard(std::shared_ptr<std::atomic<uint32_t>> epoch, uint32_t snapshot) : mEpoch(std::move(epoch)), mSnapshot(snapshot) {}

        // Returns true if no Invalidate() call has happened on the owning
        // BleCallbackEpoch since this guard was created.
        bool IsValid() const { return mEpoch->load(std::memory_order_acquire) == mSnapshot; }

    private:
        std::shared_ptr<std::atomic<uint32_t>> mEpoch;
        uint32_t mSnapshot;
    };

    BleCallbackEpoch() : mValue(std::make_shared<std::atomic<uint32_t>>(0)) {}

    // Invalidates every Guard created before this call.
    void Invalidate() { mValue->fetch_add(1, std::memory_order_acq_rel); }

    uint32_t Snapshot() const { return mValue->load(std::memory_order_acquire); }

    Guard MakeGuard() const { return Guard(mValue, Snapshot()); }

private:
    std::shared_ptr<std::atomic<uint32_t>> mValue;
};

using BleCallbackGuard = BleCallbackEpoch::Guard;

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
