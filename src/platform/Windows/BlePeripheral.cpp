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

#include <platform/Windows/BlePeripheral.h>

#include <cstring>
#include <exception>
#include <functional>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <ble/Ble.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/ConfigurationManager.h>
#include <platform/ConnectivityManager.h>
#include "platform/internal/BLEManager.h"
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/Windows/BleAdvertisingData.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage::Streams;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

CHIP_ERROR MapBluetoothError(BluetoothError error)
{
    if (error == BluetoothError::Success)
    {
        return CHIP_NO_ERROR;
    }
    if (error == BluetoothError::RadioNotAvailable || error == BluetoothError::DisabledByPolicy ||
        error == BluetoothError::NotSupported)
    {
        return BLE_ERROR_ADAPTER_UNAVAILABLE;
    }
    return CHIP_ERROR_INTERNAL;
}

template <class Callback>
void RunPeripheralEvent(BleCallbackGuard guard, const char * operation, Callback && callback) noexcept
{
    if (!guard.IsValid())
    {
        return;
    }

    try
    {
        callback();
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "%s failed: 0x%08lx", operation, static_cast<unsigned long>(error.code().value));
    } catch (std::exception const & error)
    {
        ChipLogError(Ble, "%s failed: %s", operation, error.what());
    }
}

template <class Callback>
void RunRegistrationCallback(BleCallbackGuard guard, const char * operation, Callback && callback) noexcept
{
    if (!guard.IsValid())
    {
        return;
    }

    try
    {
        callback();
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "%s failed: 0x%08lx", operation, static_cast<unsigned long>(error.code().value));
        if (guard.IsValid())
        {
            BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR_INTERNAL);
        }
    } catch (std::exception const & error)
    {
        ChipLogError(Ble, "%s failed: %s", operation, error.what());
        if (guard.IsValid())
        {
            BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR_INTERNAL);
        }
    }
}

} // namespace

CHIP_ERROR BlePeripheralServer::RegisterGattApplication()
{
    if (mRegistered.load(std::memory_order_acquire))
    {
        return CHIP_NO_ERROR;
    }

    auto guard = mEpoch.MakeGuard();
    try
    {
        auto createOp = GattServiceProvider::CreateAsync(GuidFromChipBleUUID(Ble::CHIP_BLE_SVC_ID));
        createOp.Completed([this, guard](IAsyncOperation<GattServiceProviderResult> const & op, AsyncStatus status) {
            RunRegistrationCallback(guard, "GattServiceProvider::CreateAsync completion", [&] {
                if (status != AsyncStatus::Completed)
                {
                    BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR_INTERNAL);
                    return;
                }
                GattServiceProviderResult result = op.GetResults();
                if (result.Error() != BluetoothError::Success)
                {
                    ChipLogError(Ble, "GattServiceProvider::CreateAsync failed: BluetoothError %u",
                                 static_cast<unsigned>(result.Error()));
                    BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(MapBluetoothError(result.Error()));
                    return;
                }
                mServiceProvider = result.ServiceProvider();

                GattLocalCharacteristicParameters rxParams;
                rxParams.CharacteristicProperties(GattCharacteristicProperties::Write);
                rxParams.WriteProtectionLevel(GattProtectionLevel::Plain);

                auto rxOp =
                    mServiceProvider.Service().CreateCharacteristicAsync(GuidFromChipBleUUID(Ble::CHIP_BLE_CHAR_1_UUID), rxParams);
                rxOp.Completed([this, guard](IAsyncOperation<GattLocalCharacteristicResult> const & rxAsyncOp,
                                             AsyncStatus rxStatus) {
                    RunRegistrationCallback(guard, "RX characteristic creation completion", [&] {
                        if (rxStatus != AsyncStatus::Completed || rxAsyncOp.GetResults().Error() != BluetoothError::Success)
                        {
                            BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR_INTERNAL);
                            return;
                        }
                        mRxCharacteristic = rxAsyncOp.GetResults().Characteristic();
                        mWriteRequestedRevoker =
                            mRxCharacteristic.WriteRequested(winrt::auto_revoke,
                                                             [this, guard](GattLocalCharacteristic const & characteristic,
                                                                           GattWriteRequestedEventArgs const & args) {
                                                                 RunPeripheralEvent(guard, "GATT write-request callback", [&] {
                                                                     HandleWriteRequested(characteristic, args);
                                                                 });
                                                             });

                        GattLocalCharacteristicParameters txParams;
                        txParams.CharacteristicProperties(GattCharacteristicProperties::Indicate);
                        txParams.ReadProtectionLevel(GattProtectionLevel::Plain);

                        auto txOp = mServiceProvider.Service().CreateCharacteristicAsync(
                            GuidFromChipBleUUID(Ble::CHIP_BLE_CHAR_2_UUID), txParams);
                        txOp.Completed([this, guard](IAsyncOperation<GattLocalCharacteristicResult> const & txAsyncOp,
                                                     AsyncStatus txStatus) {
                            RunRegistrationCallback(guard, "TX characteristic creation completion", [&] {
                                if (txStatus != AsyncStatus::Completed || txAsyncOp.GetResults().Error() != BluetoothError::Success)
                                {
                                    BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR_INTERNAL);
                                    return;
                                }
                                mTxCharacteristic                = txAsyncOp.GetResults().Characteristic();
                                mSubscribedClientsChangedRevoker = mTxCharacteristic.SubscribedClientsChanged(
                                    winrt::auto_revoke,
                                    [this, guard](GattLocalCharacteristic const & characteristic, IInspectable const & args) {
                                        RunPeripheralEvent(guard, "Subscribed-clients callback",
                                                           [&] { HandleSubscribedClientsChanged(characteristic, args); });
                                    });

                                mRegistered.store(true, std::memory_order_release);
                                BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_NO_ERROR);
                            });
                        });
                    });
                });
            });
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "RegisterGattApplication failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR BlePeripheralServer::StartAdvertising(const char * deviceName, bool fastAdvertising)
{
    VerifyOrReturnError(mServiceProvider, CHIP_ERROR_INCORRECT_STATE);
    (void) fastAdvertising; // WinRT does not expose fast/slow advertising-interval selection to GattServiceProvider.

    // The over-the-air BLE local name is the OS's Bluetooth radio "friendly
    // name" (Settings > Bluetooth & devices); GattServiceProvider does not
    // let an application override the advertised local name per-service.
    // deviceName is still used for GetDeviceName()/SetDeviceName() API
    // compatibility and logging. This is a concrete Windows platform
    // constraint, not a bug.
    ChipLogProgress(Ble, "Starting CHIPoBLE advertising as \"%s\" (advertised local name comes from the OS Bluetooth radio name)",
                    deviceName);

    Ble::ChipBLEDeviceIdentificationInfo info;
    info.Init();
    uint16_t discriminator = 0;
    if (GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator) == CHIP_NO_ERROR)
    {
        info.SetDeviceDiscriminator(discriminator);
    }
    uint16_t vendorId  = 0;
    uint16_t productId = 0;
    if (GetDeviceInstanceInfoProvider()->GetVendorId(vendorId) == CHIP_NO_ERROR)
    {
        info.SetVendorId(vendorId);
    }
    if (GetDeviceInstanceInfoProvider()->GetProductId(productId) == CHIP_NO_ERROR)
    {
        info.SetProductId(productId);
    }

    uint8_t serviceDataBytes[kChipServiceDataPayloadSize];
    EncodeChipServiceDataPayload(info, serviceDataBytes);
    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(serviceDataBytes, serviceDataBytes + sizeof(serviceDataBytes)));

    GattServiceProviderAdvertisingParameters advertisingParameters;
    advertisingParameters.IsConnectable(true);
    advertisingParameters.IsDiscoverable(true);
    try
    {
        advertisingParameters.ServiceData(writer.DetachBuffer());
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "This Windows version cannot advertise Matter service data: 0x%08lx",
                     static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
    }

    try
    {
        mServiceProvider.StartAdvertising(advertisingParameters);
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "StartAdvertising failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }

    // GattServiceProvider does not report StartAdvertising() completion via an
    // async result; the actual outcome (including RadioNotAvailable) is only
    // observable a moment later via AdvertisementStatus(). Check it once from
    // a deferred callback on the Matter event loop so this still follows the
    // "marshal every callback through PlatformMgr()" contract even though the
    // underlying WinRT call is itself synchronous-looking.
    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork(&BlePeripheralServer::CheckAdvertisingStartedWork,
                                                        reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

CHIP_ERROR BlePeripheralServer::StopAdvertising()
{
    VerifyOrReturnError(mServiceProvider, CHIP_ERROR_INCORRECT_STATE);
    try
    {
        mServiceProvider.StopAdvertising();
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "StopAdvertising failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return CHIP_ERROR_INTERNAL;
    }
    mAdvertising.store(false, std::memory_order_release);

    TEMPORARY_RETURN_IGNORED PlatformMgr().ScheduleWork(&BlePeripheralServer::CheckAdvertisingStoppedWork,
                                                        reinterpret_cast<intptr_t>(this));
    return CHIP_NO_ERROR;
}

bool BlePeripheralServer::IsAdvertising() const
{
    return mAdvertising.load(std::memory_order_acquire);
}

void BlePeripheralServer::CheckAdvertisingStartedWork(intptr_t self)
{
    auto * server = reinterpret_cast<BlePeripheralServer *>(self);
    if (!server->mServiceProvider)
    {
        return; // Shut down (or never registered) since this was scheduled; nothing to report.
    }
    bool started = server->mServiceProvider.AdvertisementStatus() == GattServiceProviderAdvertisementStatus::Started;
    server->mAdvertising.store(started, std::memory_order_release);
    BLEManagerImpl::NotifyBLEPeripheralAdvStartComplete(started ? CHIP_NO_ERROR : BLE_ERROR_ADAPTER_UNAVAILABLE);
}

void BlePeripheralServer::CheckAdvertisingStoppedWork(intptr_t self)
{
    auto * server = reinterpret_cast<BlePeripheralServer *>(self);
    if (!server->mServiceProvider)
    {
        return; // Shut down since this was scheduled; nothing to report.
    }
    BLEManagerImpl::NotifyBLEPeripheralAdvStopComplete(CHIP_NO_ERROR);
}

void BlePeripheralServer::Shutdown()
{
    mEpoch.Invalidate();
    mWriteRequestedRevoker.revoke();
    mSubscribedClientsChangedRevoker.revoke();

    if (mServiceProvider)
    {
        try
        {
            mServiceProvider.StopAdvertising();
        } catch (winrt::hresult_error const &)
        {
            // Best-effort; the provider is being torn down regardless.
        }
    }

    mServiceProvider  = nullptr;
    mRxCharacteristic = nullptr;
    mTxCharacteristic = nullptr;
    mRegistered.store(false, std::memory_order_release);
    mAdvertising.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(mConnectionMutex);
    mHasConnection        = false;
    mConnectionSessionKey = 0;
}

CHIP_ERROR BlePeripheralServer::SendIndication(std::shared_ptr<WinRTBleConnection> connection, System::PacketBufferHandle buffer)
{
    VerifyOrReturnError(mTxCharacteristic, BLE_ERROR_GATT_INDICATE_FAILED);
    VerifyOrReturnError(connection && connection->PeripheralClient(), BLE_ERROR_GATT_INDICATE_FAILED);

    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(buffer->Start(), buffer->Start() + buffer->DataLength()));

    std::weak_ptr<WinRTBleConnection> weakConnection = connection;
    auto guard                                       = connection->MakeGuard();
    try
    {
        auto op = mTxCharacteristic.NotifyValueAsync(writer.DetachBuffer(), connection->PeripheralClient());
        op.Completed([weakConnection, guard](IAsyncOperation<GattClientNotificationResult> const & asyncOp, AsyncStatus status) {
            auto conn = weakConnection.lock();
            if (!conn || !guard.IsValid() || conn->IsClosed())
            {
                return;
            }
            try
            {
                if (status == AsyncStatus::Completed && asyncOp.GetResults().Status() == GattCommunicationStatus::Success)
                {
                    BLEManagerImpl::HandleTXComplete(conn.get());
                }
                else
                {
                    BLEManagerImpl::HandleConnectionError(conn.get(), BLE_ERROR_GATT_INDICATE_FAILED);
                }
            } catch (winrt::hresult_error const & error)
            {
                ChipLogError(Ble, "NotifyValueAsync completion failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
                if (guard.IsValid() && !conn->IsClosed())
                {
                    BLEManagerImpl::HandleConnectionError(conn.get(), BLE_ERROR_GATT_INDICATE_FAILED);
                }
            }
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "SendIndication failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return BLE_ERROR_GATT_INDICATE_FAILED;
    }
    return CHIP_NO_ERROR;
}

uint16_t BlePeripheralServer::NumConnections() const
{
    std::lock_guard<std::mutex> lock(mConnectionMutex);
    return mHasConnection ? 1 : 0;
}

void BlePeripheralServer::HandleWriteRequested(GattLocalCharacteristic const & characteristic,
                                               GattWriteRequestedEventArgs const & args)
{
    (void) characteristic;
    auto deferral = args.GetDeferral();
    auto guard    = mEpoch.MakeGuard();
    try
    {
        auto op = args.GetRequestAsync();
        op.Completed([deferral, guard](IAsyncOperation<GattWriteRequest> const & asyncOp, AsyncStatus status) {
            try
            {
                if (guard.IsValid() && status == AsyncStatus::Completed)
                {
                    GattWriteRequest request = asyncOp.GetResults();
                    if (request)
                    {
                        auto reader = DataReader::FromBuffer(request.Value());
                        std::vector<uint8_t> bytes(reader.UnconsumedBufferLength());
                        if (!bytes.empty())
                        {
                            reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
                        }
                        BLEManagerImpl::HandleRXCharWrite(bytes.data(), bytes.size());
                        if (request.Option() == GattWriteOption::WriteWithResponse)
                        {
                            request.Respond();
                        }
                    }
                }
            } catch (winrt::hresult_error const & error)
            {
                ChipLogError(Ble, "GetRequestAsync completion failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
            }
            deferral.Complete();
        });
    } catch (winrt::hresult_error const &)
    {
        deferral.Complete();
    }
}

void BlePeripheralServer::HandleSubscribedClientsChanged(GattLocalCharacteristic const & characteristic, IInspectable const & args)
{
    (void) args;
    auto clients = characteristic.SubscribedClients();

    std::lock_guard<std::mutex> lock(mConnectionMutex);
    if (clients.Size() == 0)
    {
        if (mHasConnection)
        {
            uint64_t sessionKey   = mConnectionSessionKey;
            mHasConnection        = false;
            mConnectionSessionKey = 0;
            BLEManagerImpl::HandleSubscribedClientRemoved(sessionKey);
        }
        return;
    }

    if (mHasConnection)
    {
        return; // A connection is already tracked; see class documentation on single-connection support.
    }

    GattSubscribedClient client = clients.GetAt(0);
    mHasConnection              = true;
    mConnectionSessionKey       = std::hash<winrt::hstring>{}(client.Session().DeviceId().Id());

    auto connection = std::make_shared<WinRTBleConnection>(BleConnectionRole::kPeripheral);
    connection->SetPeripheralServer(this);
    connection->SetPeripheralClient(client);
    connection->SetMTU(static_cast<uint16_t>(client.Session().MaxPduSize()));
    BLEManagerImpl::HandleSubscribedClientAdded(std::move(connection));
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
