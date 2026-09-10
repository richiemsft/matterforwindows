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
 *          Native Windows C++/WinRT commissionee (peripheral) GATT server and
 *          advertising for CHIPoBLE.
 *
 *          Uses GattServiceProvider to publish the Matter primary service and
 *          its C1 (write, RX)/C2 (indicate, TX) characteristics, and
 *          GattServiceProviderAdvertisingParameters to advertise it. This is
 *          the supported *unpackaged* Win32 desktop path: GattServiceProvider
 *          has been callable from an unpackaged process since Windows 11, but
 *          it still requires the local Bluetooth adapter to support and have
 *          enabled the peripheral ("LE Advertising Extension"/GATT server)
 *          role, which not every Windows PC's Bluetooth radio does. When the
 *          radio or OS declines the peripheral role, every call below fails
 *          asynchronously with a concrete BluetoothError (surfaced here as a
 *          CHIP_ERROR through the normal NotifyBLEPeripheral*Complete
 *          callbacks) rather than silently pretending to advertise.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>

#include <lib/core/CHIPError.h>
#include <platform/Windows/BleCallbackGuard.h>
#include <platform/Windows/BleConnection.h>
#include <system/SystemClock.h>
#include <system/SystemLayer.h>
#include <system/SystemPacketBuffer.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

class BlePeripheralServer
{
public:
    BlePeripheralServer() = default;
    ~BlePeripheralServer() { Shutdown(); }

    BlePeripheralServer(const BlePeripheralServer &)             = delete;
    BlePeripheralServer & operator=(const BlePeripheralServer &) = delete;

    // Creates the GattServiceProvider and its RX/TX characteristics if not
    // already done. Completion (including any BluetoothError, e.g. the local
    // radio does not support the peripheral role) is reported asynchronously
    // via BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete().
    CHIP_ERROR RegisterGattApplication();

    // Starts/stops advertising the Matter service data (short UUID + device
    // identification info) with the given local name. Completion is reported
    // via BLEManagerImpl::NotifyBLEPeripheralAdvStartComplete()/
    // NotifyBLEPeripheralAdvStopComplete().
    CHIP_ERROR StartAdvertising(const char * deviceName, bool fastAdvertising);
    CHIP_ERROR StopAdvertising();
    bool IsAdvertising() const;
    bool IsRegistered() const { return mRegistered.load(std::memory_order_acquire); }

    // Idempotent: revokes every event subscription, disposes the service
    // provider, and closes any tracked connection.
    void Shutdown();

    // Invoked by WinRTBleConnection::SendIndication(). Targets the specific
    // subscribed client the connection represents.
    CHIP_ERROR SendIndication(std::shared_ptr<WinRTBleConnection> connection, System::PacketBufferHandle buffer);

    uint16_t NumConnections() const;

private:
    void HandleWriteRequested(
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const & characteristic,
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteRequestedEventArgs const & args);
    void HandleSubscribedClientsChanged(
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic const & characteristic,
        winrt::Windows::Foundation::IInspectable const & args);

    // GattServiceProvider does not report StartAdvertising()/StopAdvertising()
    // completion via an async result or callback with a result argument; the
    // actual outcome (including e.g. RadioNotAvailable) is only observable a
    // moment later via AdvertisementStatus() -- and observed to briefly read
    // a transient/incorrect value immediately after StartAdvertising()
    // returns, so the started check is delayed via a short SystemLayer timer
    // (see kAdvertisingStatusSettleDelay) rather than checked on the very
    // next event-loop iteration. Both checks run on the Matter event-loop
    // thread. Safe to run after Shutdown(): mServiceProvider is null by then
    // and the functions no-op.
    static void CheckAdvertisingStartedTimerFired(System::Layer * layer, void * appState);
    static void CheckAdvertisingStoppedWork(intptr_t self);

    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattServiceProvider mServiceProvider{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic mRxCharacteristic{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic mTxCharacteristic{ nullptr };

    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic::WriteRequested_revoker
        mWriteRequestedRevoker;
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattLocalCharacteristic::SubscribedClientsChanged_revoker
        mSubscribedClientsChangedRevoker;

    std::atomic<bool> mRegistered{ false };
    std::atomic<bool> mAdvertising{ false };
    BleCallbackEpoch mEpoch;

    mutable std::mutex mConnectionMutex;
    // Whether a remote central is currently subscribed. CHIPoBLE, like every
    // other platform's BLEManagerImpl, supports exactly one concurrent BLE
    // connection today; the actual WinRTBleConnection object is owned by
    // BLEManagerImpl (see BLEManagerImpl::RegisterConnection()), not here --
    // this flag and key exist only so HandleSubscribedClientsChanged() can
    // tell "newly subscribed" from "still subscribed" without querying back
    // into BLEManagerImpl.
    bool mHasConnection            = false;
    uint64_t mConnectionSessionKey = 0;
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
