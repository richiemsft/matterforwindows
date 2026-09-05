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
 *          Defines the concrete type behind BLE_CONNECTION_OBJECT for the
 *          native Windows C++/WinRT BLE backend (see BlePlatformConfig.h).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>

#include <ble/Ble.h>
#include <lib/core/CHIPError.h>
#include <platform/Windows/BleCallbackGuard.h>
#include <system/SystemPacketBuffer.h>

namespace chip {
namespace DeviceLayer {
namespace Internal {

class BlePeripheralServer;

// Converts the shared, platform-agnostic 16-byte chip::Ble::ChipBleUUID wire
// representation (network/big-endian byte order, as produced by
// StringToUUIDConstexpr) into a winrt::guid (whose Data1/Data2/Data3 fields
// are host-endian integers).
winrt::guid GuidFromChipBleUUID(const Ble::ChipBleUUID & uuid);

enum class BleConnectionRole : uint8_t
{
    kCentral,   // We are the GATT client connected to a commissionee peripheral.
    kPeripheral // We are the GATT server; this represents one subscribed remote central.
};

/**
 * The object identified by a BLE_CONNECTION_OBJECT handle.
 *
 * Always allocated with std::make_shared and owned by BLEManagerImpl for the
 * lifetime of the connection; BLE_CONNECTION_OBJECT is the resulting raw
 * pointer (WinRTBleConnection *). The pointer is stable for as long as the
 * owning shared_ptr lives: BLEManagerImpl only erases it from its connection
 * table -- destroying the object -- after the CHIP BLE stack has called
 * BleApplicationDelegate::NotifyChipConnectionClosed() for it, matching the
 * documented contract of that callback ("CHIP no longer cares about the
 * state of the given BLE connection").
 *
 * Every WinRT completion handler and event handler registered by this class
 * captures a std::weak_ptr<WinRTBleConnection> (obtained via
 * weak_from_this()) rather than a strong reference, plus a BleCallbackGuard
 * snapshot taken at the time the operation was issued. A callback that fires
 * after Close() either finds the weak_ptr already expired (object fully
 * destroyed) or finds the guard invalid (Close() bumped the epoch); in
 * either case it must do nothing. This makes the class safe to use from
 * arbitrary WinRT threadpool threads even though it is only ever mutated on
 * the Matter event-loop thread.
 */
class WinRTBleConnection : public std::enable_shared_from_this<WinRTBleConnection>
{
public:
    explicit WinRTBleConnection(BleConnectionRole role) : mRole(role) {}
    ~WinRTBleConnection() { Close(); }

    WinRTBleConnection(const WinRTBleConnection &)             = delete;
    WinRTBleConnection & operator=(const WinRTBleConnection &) = delete;

    BleConnectionRole GetRole() const { return mRole; }
    bool IsClosed() const { return mClosed.load(std::memory_order_acquire); }

    // A guard for a WinRT operation being issued right now against this
    // connection. IsValid() starts returning false as soon as Close() runs.
    BleCallbackGuard MakeGuard() const { return mEpoch.MakeGuard(); }

    // Idempotent. Marks the connection closed, revokes every WinRT event
    // subscription, and releases the held WinRT reference-counted handles.
    // Must only be called on the Matter event-loop thread.
    void Close();

    uint16_t GetMTU() const { return mMtu.load(std::memory_order_relaxed); }
    void SetMTU(uint16_t mtu) { mMtu.store(mtu, std::memory_order_relaxed); }

    // ----- Central-role GATT client state -----
    void SetCentralGatt(winrt::Windows::Devices::Bluetooth::BluetoothLEDevice device,
                        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService service,
                        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic rxCharacteristic,
                        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic txCharacteristic);

    const winrt::Windows::Devices::Bluetooth::BluetoothLEDevice & CentralDevice() const { return mCentralDevice; }
    const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic & CentralRxCharacteristic() const
    {
        return mCentralRxCharacteristic;
    }
    const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic & CentralTxCharacteristic() const
    {
        return mCentralTxCharacteristic;
    }

    void SetTxValueChangedRevoker(
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic::ValueChanged_revoker revoker)
    {
        mTxValueChangedRevoker = std::move(revoker);
    }
    void SetConnectionStatusChangedRevoker(
        winrt::Windows::Devices::Bluetooth::BluetoothLEDevice::ConnectionStatusChanged_revoker revoker)
    {
        mConnectionStatusChangedRevoker = std::move(revoker);
    }

    // ----- Peripheral-role GATT server state -----
    void SetPeripheralClient(winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSubscribedClient client);
    const winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSubscribedClient & PeripheralClient() const
    {
        return mPeripheralClient;
    }
    // Non-owning; BlePeripheralServer outlives every connection it creates
    // (BLEManagerImpl owns both and tears the server down after closing all
    // of its connections). Used only to route SendIndication() to the one
    // local TX characteristic the server owns.
    void SetPeripheralServer(BlePeripheralServer * server) { mPeripheralServer = server; }
    // A stable, process-local identifier for the remote central this
    // connection represents (the underlying GattSession's device id hash),
    // used only to recognize "is this the same remote client" across
    // SubscribedClientsChanged callbacks; it is not a BLE_CONNECTION_OBJECT
    // replacement.
    uint64_t PeripheralSessionKey() const { return mPeripheralSessionKey; }

    // ----- Downcall implementations shared by BLEManagerImpl -----
    // These perform the actual WinRT calls and, on completion (always
    // marshaled through PlatformMgr()), post the matching ChipDeviceEvent.
    // See BLEManagerImpl.cpp for how the results are consumed.
    CHIP_ERROR SubscribeCharacteristic();                           // central: enable indications on TX
    CHIP_ERROR UnsubscribeCharacteristic();                         // central: disable indications on TX
    CHIP_ERROR SendWriteRequest(System::PacketBufferHandle buffer); // central: write RX
    CHIP_ERROR SendIndication(System::PacketBufferHandle buffer);   // peripheral: indicate TX
    CHIP_ERROR CloseConnection();                                   // central: disconnect device

private:
    const BleConnectionRole mRole;
    std::atomic<bool> mClosed{ false };
    BleCallbackEpoch mEpoch;
    std::atomic<uint16_t> mMtu{ 0 };

    winrt::Windows::Devices::Bluetooth::BluetoothLEDevice mCentralDevice{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService mCentralService{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic mCentralRxCharacteristic{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic mCentralTxCharacteristic{ nullptr };
    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic::ValueChanged_revoker mTxValueChangedRevoker;
    winrt::Windows::Devices::Bluetooth::BluetoothLEDevice::ConnectionStatusChanged_revoker mConnectionStatusChangedRevoker;

    winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattSubscribedClient mPeripheralClient{ nullptr };
    uint64_t mPeripheralSessionKey          = 0;
    BlePeripheralServer * mPeripheralServer = nullptr;
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
