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

#include <platform/Windows/BleConnection.h>

#include <cstring>
#include <exception>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/ConnectivityManager.h>
#include "platform/internal/BLEManager.h"
#include <platform/Windows/BlePeripheral.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

IBuffer BufferFromPacket(const System::PacketBufferHandle & buffer)
{
    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(buffer->Start(), buffer->Start() + buffer->DataLength()));
    return writer.DetachBuffer();
}

template <class Callback>
void RunConnectionCallback(std::weak_ptr<WinRTBleConnection> weakConnection, BleCallbackGuard guard, const char * operation,
                           CHIP_ERROR failure, Callback && callback) noexcept
{
    auto connection = weakConnection.lock();
    if (!connection || !guard.IsValid() || connection->IsClosed())
    {
        return;
    }

    try
    {
        callback(connection);
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "%s failed: 0x%08lx", operation, static_cast<unsigned long>(error.code().value));
        if (guard.IsValid() && !connection->IsClosed())
        {
            BLEManagerImpl::HandleConnectionError(connection.get(), failure);
        }
    } catch (std::exception const & error)
    {
        ChipLogError(Ble, "%s failed: %s", operation, error.what());
        if (guard.IsValid() && !connection->IsClosed())
        {
            BLEManagerImpl::HandleConnectionError(connection.get(), failure);
        }
    }
}

} // namespace

winrt::guid GuidFromChipBleUUID(const Ble::ChipBleUUID & uuid)
{
    winrt::guid guid{};
    guid.Data1 = (static_cast<uint32_t>(uuid.bytes[0]) << 24) | (static_cast<uint32_t>(uuid.bytes[1]) << 16) |
        (static_cast<uint32_t>(uuid.bytes[2]) << 8) | static_cast<uint32_t>(uuid.bytes[3]);
    guid.Data2 = static_cast<uint16_t>((static_cast<uint16_t>(uuid.bytes[4]) << 8) | uuid.bytes[5]);
    guid.Data3 = static_cast<uint16_t>((static_cast<uint16_t>(uuid.bytes[6]) << 8) | uuid.bytes[7]);
    memcpy(guid.Data4, &uuid.bytes[8], sizeof(guid.Data4));
    return guid;
}

void WinRTBleConnection::Close()
{
    if (mClosed.exchange(true, std::memory_order_acq_rel))
    {
        return; // already closed
    }
    mEpoch.Invalidate();

    mTxValueChangedRevoker.revoke();
    mConnectionStatusChangedRevoker.revoke();

    if (mRole == BleConnectionRole::kCentral && mCentralDevice)
    {
        mCentralDevice.Close();
    }

    mCentralDevice           = nullptr;
    mCentralService          = nullptr;
    mCentralRxCharacteristic = nullptr;
    mCentralTxCharacteristic = nullptr;
    mPeripheralClient        = nullptr;
}

void WinRTBleConnection::SetCentralGatt(BluetoothLEDevice device, GattDeviceService service, GattCharacteristic rxCharacteristic,
                                        GattCharacteristic txCharacteristic)
{
    mCentralDevice           = std::move(device);
    mCentralService          = std::move(service);
    mCentralRxCharacteristic = std::move(rxCharacteristic);
    mCentralTxCharacteristic = std::move(txCharacteristic);
}

void WinRTBleConnection::SetPeripheralClient(GattSubscribedClient client)
{
    mPeripheralClient = std::move(client);
    if (mPeripheralClient)
    {
        // Session().DeviceId is the stable identity of the remote central;
        // hash it down to a process-local key purely to recognize the same
        // client across SubscribedClientsChanged callbacks.
        mPeripheralSessionKey = std::hash<winrt::hstring>{}(mPeripheralClient.Session().DeviceId().Id());
    }
}

CHIP_ERROR WinRTBleConnection::SubscribeCharacteristic()
{
    VerifyOrReturnError(mRole == BleConnectionRole::kCentral, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mCentralTxCharacteristic, CHIP_ERROR_INCORRECT_STATE);

    auto self  = weak_from_this();
    auto guard = MakeGuard();
    try
    {
        auto op = mCentralTxCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::Indicate);
        op.Completed([self, guard](IAsyncOperation<GattCommunicationStatus> const & asyncOp, AsyncStatus status) {
            RunConnectionCallback(
                self, guard, "SubscribeCharacteristic completion", BLE_ERROR_GATT_SUBSCRIBE_FAILED,
                [&](std::shared_ptr<WinRTBleConnection> const & conn) {
                    if (status != AsyncStatus::Completed || asyncOp.GetResults() != GattCommunicationStatus::Success)
                    {
                        BLEManagerImpl::HandleConnectionError(conn.get(), BLE_ERROR_GATT_SUBSCRIBE_FAILED);
                        return;
                    }
                    conn->mTxValueChangedRevoker = conn->mCentralTxCharacteristic.ValueChanged(
                        winrt::auto_revoke, [self, guard](GattCharacteristic const &, GattValueChangedEventArgs const & args) {
                            RunConnectionCallback(
                                self, guard, "TX characteristic callback", CHIP_ERROR_INTERNAL,
                                [&](std::shared_ptr<WinRTBleConnection> const & innerConn) {
                                    auto reader = DataReader::FromBuffer(args.CharacteristicValue());
                                    std::vector<uint8_t> bytes(reader.UnconsumedBufferLength());
                                    if (!bytes.empty())
                                    {
                                        reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
                                    }
                                    BLEManagerImpl::HandleTXCharChanged(innerConn.get(), bytes.data(), bytes.size());
                                });
                        });
                    BLEManagerImpl::HandleSubscribeOpComplete(conn.get(), true);
                });
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "SubscribeCharacteristic failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WinRTBleConnection::UnsubscribeCharacteristic()
{
    VerifyOrReturnError(mRole == BleConnectionRole::kCentral, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mCentralTxCharacteristic, CHIP_ERROR_INCORRECT_STATE);

    auto self  = weak_from_this();
    auto guard = MakeGuard();
    try
    {
        auto op = mCentralTxCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::None);
        op.Completed([self, guard](IAsyncOperation<GattCommunicationStatus> const & asyncOp, AsyncStatus status) {
            RunConnectionCallback(self, guard, "UnsubscribeCharacteristic completion", BLE_ERROR_GATT_UNSUBSCRIBE_FAILED,
                                  [&](std::shared_ptr<WinRTBleConnection> const & conn) {
                                      if (status != AsyncStatus::Completed ||
                                          asyncOp.GetResults() != GattCommunicationStatus::Success)
                                      {
                                          BLEManagerImpl::HandleConnectionError(conn.get(), BLE_ERROR_GATT_UNSUBSCRIBE_FAILED);
                                          return;
                                      }
                                      conn->mTxValueChangedRevoker.revoke();
                                      BLEManagerImpl::HandleSubscribeOpComplete(conn.get(), false);
                                  });
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "UnsubscribeCharacteristic failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WinRTBleConnection::SendWriteRequest(System::PacketBufferHandle buffer)
{
    VerifyOrReturnError(mRole == BleConnectionRole::kCentral, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mCentralRxCharacteristic, CHIP_ERROR_INCORRECT_STATE);

    IBuffer winrtBuffer;
    try
    {
        winrtBuffer = BufferFromPacket(buffer);
    } catch (winrt::hresult_error const &)
    {
        return CHIP_ERROR_NO_MEMORY;
    }

    auto self  = weak_from_this();
    auto guard = MakeGuard();
    try
    {
        auto op = mCentralRxCharacteristic.WriteValueAsync(winrtBuffer, GattWriteOption::WriteWithResponse);
        op.Completed([self, guard](IAsyncOperation<GattCommunicationStatus> const & asyncOp, AsyncStatus status) {
            RunConnectionCallback(self, guard, "SendWriteRequest completion", BLE_ERROR_GATT_WRITE_FAILED,
                                  [&](std::shared_ptr<WinRTBleConnection> const & conn) {
                                      if (status == AsyncStatus::Completed &&
                                          asyncOp.GetResults() == GattCommunicationStatus::Success)
                                      {
                                          BLEManagerImpl::HandleWriteComplete(conn.get());
                                      }
                                      else
                                      {
                                          BLEManagerImpl::HandleConnectionError(conn.get(), BLE_ERROR_GATT_WRITE_FAILED);
                                      }
                                  });
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "SendWriteRequest failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WinRTBleConnection::SendIndication(System::PacketBufferHandle buffer)
{
    VerifyOrReturnError(mRole == BleConnectionRole::kPeripheral, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mPeripheralClient, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mPeripheralServer != nullptr, CHIP_ERROR_INCORRECT_STATE);

    return mPeripheralServer->SendIndication(shared_from_this(), std::move(buffer));
}

CHIP_ERROR WinRTBleConnection::CloseConnection()
{
    // Central role: BluetoothLEDevice::Close() actively disconnects. Peripheral
    // role: WinRT's local GATT server surface has no per-client "disconnect"
    // API (GattServiceProvider only exposes StartAdvertising()/
    // StopAdvertising() for the whole service, which would affect every
    // subscribed client, not just this one); Close() here only stops this
    // object from being used further by the platform and the CHIP BLE
    // stack -- the remote central's underlying radio link is left for it (or
    // a future StopAdvertising()) to tear down. This is a concrete Windows
    // GATT-server platform constraint, not a bug.
    Close();
    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
