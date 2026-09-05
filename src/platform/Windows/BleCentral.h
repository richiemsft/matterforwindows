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
 *          Native Windows C++/WinRT controller (central) BLE scanning and
 *          connection establishment for CHIPoBLE.
 *
 *          Scans with a BluetoothLEAdvertisementWatcher, matches the Matter
 *          service-data discriminator against the requested
 *          SetupDiscriminator set (BleAdvertisingData.h), connects with
 *          BluetoothLEDevice::FromBluetoothAddressAsync(), and discovers the
 *          Matter primary service and its C1 (RX, write)/C2 (TX, indicate)
 *          characteristics.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.h>

#include <lib/core/CHIPError.h>
#include <lib/support/SetupDiscriminator.h>
#include <lib/support/Span.h>
#include <platform/Windows/BleCallbackGuard.h>
#include <platform/Windows/BleConnection.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

class BleCentralScanner : public std::enable_shared_from_this<BleCentralScanner>
{
public:
    BleCentralScanner() = default;
    ~BleCentralScanner() { Shutdown(); }

    BleCentralScanner(const BleCentralScanner &)             = delete;
    BleCentralScanner & operator=(const BleCentralScanner &) = delete;

    // Starts scanning for an advertisement matching any of the given
    // discriminators, then connects and discovers GATT on the first match.
    // On success, BLEManagerImpl::HandleNewConnection() is invoked with a
    // BLE_CONNECTION_OBJECT ready for BlePlatformDelegate use (subscribe,
    // write); on failure or timeout, BLEManagerImpl::HandleConnectFailed().
    // Only one scan/connect attempt is tracked at a time; starting a new one
    // implicitly cancels any attempt already in progress.
    CHIP_ERROR ScanAndConnect(Span<const SetupDiscriminator> discriminators);

    // Cancels any in-progress scan or connect attempt. No further
    // HandleNewConnection()/HandleConnectFailed() calls will be made for it.
    // Idempotent.
    void Cancel();

    // Logs aggregate scan results to distinguish radio/driver failures from
    // nearby devices that are not advertising the Matter service.
    void LogScanSummary();

    // Revokes every event subscription and cancels any in-progress attempt.
    void Shutdown();

private:
    void HandleAdvertisementReceived(
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher const & watcher,
        winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs const & args,
        BleCallbackGuard guard);
    void ConnectAndDiscover(uint64_t bluetoothAddress, uint16_t matchedDiscriminator, BleCallbackGuard guard);

    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher mWatcher{ nullptr };
    winrt::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher::Received_revoker mReceivedRevoker;

    BleCallbackEpoch mEpoch;
    std::mutex mStateMutex;
    std::vector<SetupDiscriminator> mDiscriminators;
    std::vector<uint16_t> mObservedDiscriminators;
    std::vector<uint32_t> mObservedServiceDataSections;
    uint32_t mAdvertisementCount       = 0;
    uint32_t mServiceDataSectionCount  = 0;
    uint32_t mMatterAdvertisementCount = 0;
    std::atomic<bool> mConnecting{ false }; // true once a matching advertisement has been found
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
