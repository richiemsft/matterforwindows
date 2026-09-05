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

#include <platform/Windows/BleCentral.h>

#include <algorithm>
#include <exception>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <ble/Ble.h>
#include <lib/core/CHIPEncoding.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/ConnectivityManager.h>
#include <platform/Windows/BleAdvertisingData.h>
#include <platform/internal/BLEManager.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

template <class Callback>
void RunWinRTCallback(const char * operation, Callback && callback) noexcept
{
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
void RunConnectCallback(BleCallbackGuard guard, const char * operation, Callback && callback) noexcept
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
            BLEManagerImpl::HandleConnectFailed(CHIP_ERROR_INTERNAL);
        }
    } catch (std::exception const & error)
    {
        ChipLogError(Ble, "%s failed: %s", operation, error.what());
        if (guard.IsValid())
        {
            BLEManagerImpl::HandleConnectFailed(CHIP_ERROR_INTERNAL);
        }
    }
}

} // namespace

CHIP_ERROR BleCentralScanner::ScanAndConnect(Span<const SetupDiscriminator> discriminators)
{
    Cancel(); // Supersede any attempt already in progress.

    try
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        mDiscriminators.assign(discriminators.begin(), discriminators.end());
        mObservedDiscriminators.clear();
        mObservedServiceDataSections.clear();
        mAdvertisementCount       = 0;
        mServiceDataSectionCount  = 0;
        mMatterAdvertisementCount = 0;
        mConnecting.store(false, std::memory_order_release);
        for (SetupDiscriminator const & discriminator : mDiscriminators)
        {
            if (discriminator.IsShortDiscriminator())
            {
                ChipLogProgress(Ble, "Scanning for Matter BLE device with short discriminator %u", discriminator.GetShortValue());
            }
            else
            {
                ChipLogProgress(Ble, "Scanning for Matter BLE device with long discriminator %u", discriminator.GetLongValue());
            }
        }
        mWatcher = BluetoothLEAdvertisementWatcher();
        mWatcher.ScanningMode(BluetoothLEScanningMode::Active);
        auto guard                                   = mEpoch.MakeGuard();
        std::weak_ptr<BleCentralScanner> weakScanner = weak_from_this();
        mReceivedRevoker =
            mWatcher.Received(winrt::auto_revoke,
                              [weakScanner, guard](BluetoothLEAdvertisementWatcher const & watcher,
                                                   BluetoothLEAdvertisementReceivedEventArgs const & args) {
                                  RunWinRTCallback("BLE advertisement callback", [weakScanner, guard, &watcher, &args] {
                                      auto scanner = weakScanner.lock();
                                      if (scanner && guard.IsValid())
                                      {
                                          scanner->HandleAdvertisementReceived(watcher, args, guard);
                                      }
                                  });
                              });
        mWatcher.Start();
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "Failed to start BLE scan: 0x%08lx", static_cast<unsigned long>(error.code().value));
        return BLE_ERROR_ADAPTER_UNAVAILABLE;
    }
    return CHIP_NO_ERROR;
}

void BleCentralScanner::Cancel()
{
    mEpoch.Invalidate();
    mReceivedRevoker.revoke();

    std::lock_guard<std::mutex> lock(mStateMutex);
    if (mWatcher)
    {
        try
        {
            mWatcher.Stop();
        } catch (winrt::hresult_error const &)
        {
            // Best-effort; the watcher is being torn down regardless.
        }
    }
    mWatcher = nullptr;
    mConnecting.store(false, std::memory_order_release);
    mDiscriminators.clear();
    mObservedDiscriminators.clear();
    mObservedServiceDataSections.clear();
}

void BleCentralScanner::LogScanSummary()
{
    std::lock_guard<std::mutex> lock(mStateMutex);
    ChipLogProgress(Ble, "BLE scan received %u advertisements, %u 16-bit service-data sections, and %u valid Matter advertisements",
                    mAdvertisementCount, mServiceDataSectionCount, mMatterAdvertisementCount);
}

void BleCentralScanner::Shutdown()
{
    Cancel();
}

void BleCentralScanner::HandleAdvertisementReceived(BluetoothLEAdvertisementWatcher const & watcher,
                                                    BluetoothLEAdvertisementReceivedEventArgs const & args, BleCallbackGuard guard)
{
    (void) watcher;

    std::lock_guard<std::mutex> lock(mStateMutex);
    if (!guard.IsValid())
    {
        return;
    }
    if (mConnecting.load(std::memory_order_acquire))
    {
        return;
    }

    ++mAdvertisementCount;
    for (auto const & section : args.Advertisement().GetSectionsByType(0x16 /* Service Data - 16-bit UUID */))
    {
        ++mServiceDataSectionCount;
        auto reader = DataReader::FromBuffer(section.Data());
        std::vector<uint8_t> bytes(reader.UnconsumedBufferLength());
        if (!bytes.empty())
        {
            reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        }

        uint16_t serviceUuid = 0;
        if (bytes.size() >= sizeof(serviceUuid))
        {
            serviceUuid = chip::Encoding::LittleEndian::Get16(bytes.data());
        }
        uint32_t sectionKey = (static_cast<uint32_t>(bytes.size()) << 16) | serviceUuid;
        if (std::find(mObservedServiceDataSections.begin(), mObservedServiceDataSections.end(), sectionKey) ==
            mObservedServiceDataSections.end())
        {
            mObservedServiceDataSections.push_back(sectionKey);
            ChipLogProgress(Ble, "Found 16-bit BLE service-data section UUID 0x%04X with %u bytes", serviceUuid,
                            static_cast<unsigned int>(bytes.size()));
        }

        Ble::ChipBLEDeviceIdentificationInfo info;
        if (!ParseChipServiceData(ByteSpan(bytes.data(), bytes.size()), info))
        {
            continue;
        }

        ++mMatterAdvertisementCount;
        uint16_t discriminator = info.GetDeviceDiscriminator();
        bool matched = std::any_of(mDiscriminators.begin(), mDiscriminators.end(), [discriminator](SetupDiscriminator const & d) {
            return d.MatchesLongDiscriminator(discriminator);
        });
        if (std::find(mObservedDiscriminators.begin(), mObservedDiscriminators.end(), discriminator) ==
            mObservedDiscriminators.end())
        {
            mObservedDiscriminators.push_back(discriminator);
            ChipLogProgress(Ble, "Found Matter BLE advertisement with long discriminator %u (%s)", discriminator,
                            matched ? "matched" : "did not match");
        }
        if (!matched)
        {
            continue;
        }

        if (mConnecting.exchange(true, std::memory_order_acq_rel))
        {
            return; // Another advertisement already claimed this scan; ignore the race.
        }

        try
        {
            mWatcher.Stop();
        } catch (winrt::hresult_error const &)
        {}

        ConnectAndDiscover(args.BluetoothAddress(), discriminator, guard);
        return;
    }
}

void BleCentralScanner::ConnectAndDiscover(uint64_t bluetoothAddress, uint16_t matchedDiscriminator, BleCallbackGuard guard)
{
    (void) matchedDiscriminator;
    try
    {
        auto deviceOp = BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress);
        deviceOp.Completed([guard](IAsyncOperation<BluetoothLEDevice> const & deviceAsyncOp, AsyncStatus deviceStatus) {
            RunConnectCallback(guard, "BluetoothLEDevice::FromBluetoothAddressAsync completion", [&] {
                if (deviceStatus != AsyncStatus::Completed)
                {
                    BLEManagerImpl::HandleConnectFailed(CHIP_ERROR_INTERNAL);
                    return;
                }
                BluetoothLEDevice device = deviceAsyncOp.GetResults();
                if (!device)
                {
                    BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                    return;
                }

                auto serviceOp = device.GetGattServicesForUuidAsync(GuidFromChipBleUUID(Ble::CHIP_BLE_SVC_ID));
                serviceOp.Completed([device, guard](IAsyncOperation<GattDeviceServicesResult> const & serviceAsyncOp,
                                                    AsyncStatus serviceStatus) {
                    RunConnectCallback(guard, "GetGattServicesForUuidAsync completion", [&] {
                        if (serviceStatus != AsyncStatus::Completed)
                        {
                            BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                            return;
                        }
                        GattDeviceServicesResult serviceResult = serviceAsyncOp.GetResults();
                        if (serviceResult.Status() != GattCommunicationStatus::Success || serviceResult.Services().Size() == 0)
                        {
                            BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                            return;
                        }
                        GattDeviceService service = serviceResult.Services().GetAt(0);

                        auto rxOp = service.GetCharacteristicsForUuidAsync(GuidFromChipBleUUID(Ble::CHIP_BLE_CHAR_1_UUID));
                        rxOp.Completed([device, service, guard](IAsyncOperation<GattCharacteristicsResult> const & rxAsyncOp,
                                                                AsyncStatus rxStatus) {
                            RunConnectCallback(guard, "RX GetCharacteristicsForUuidAsync completion", [&] {
                                if (rxStatus != AsyncStatus::Completed)
                                {
                                    BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                                    return;
                                }
                                GattCharacteristicsResult rxResult = rxAsyncOp.GetResults();
                                if (rxResult.Status() != GattCommunicationStatus::Success || rxResult.Characteristics().Size() == 0)
                                {
                                    BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                                    return;
                                }
                                GattCharacteristic rxCharacteristic = rxResult.Characteristics().GetAt(0);

                                auto txOp = service.GetCharacteristicsForUuidAsync(GuidFromChipBleUUID(Ble::CHIP_BLE_CHAR_2_UUID));
                                txOp.Completed([device, service, rxCharacteristic,
                                                guard](IAsyncOperation<GattCharacteristicsResult> const & txAsyncOp,
                                                       AsyncStatus txStatus) {
                                    RunConnectCallback(guard, "TX GetCharacteristicsForUuidAsync completion", [&] {
                                        if (txStatus != AsyncStatus::Completed)
                                        {
                                            BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                                            return;
                                        }
                                        GattCharacteristicsResult txResult = txAsyncOp.GetResults();
                                        if (txResult.Status() != GattCommunicationStatus::Success ||
                                            txResult.Characteristics().Size() == 0)
                                        {
                                            BLEManagerImpl::HandleConnectFailed(BLE_ERROR_NOT_CHIP_DEVICE);
                                            return;
                                        }
                                        GattCharacteristic txCharacteristic = txResult.Characteristics().GetAt(0);

                                        auto connection = std::make_shared<WinRTBleConnection>(BleConnectionRole::kCentral);
                                        connection->SetCentralGatt(device, service, rxCharacteristic, txCharacteristic);

                                        std::weak_ptr<WinRTBleConnection> weakConnection = connection;
                                        BleCallbackGuard connectionGuard                 = connection->MakeGuard();
                                        connection->SetConnectionStatusChangedRevoker(device.ConnectionStatusChanged(
                                            winrt::auto_revoke,
                                            [weakConnection, connectionGuard](BluetoothLEDevice const & changedDevice,
                                                                              IInspectable const &) {
                                                RunWinRTCallback("BLE connection-status callback", [&] {
                                                    auto conn = weakConnection.lock();
                                                    if (!conn || !connectionGuard.IsValid() || conn->IsClosed())
                                                    {
                                                        return;
                                                    }
                                                    if (changedDevice.ConnectionStatus() == BluetoothConnectionStatus::Disconnected)
                                                    {
                                                        BLEManagerImpl::HandleConnectionClosed(conn.get());
                                                    }
                                                });
                                            }));

                                        try
                                        {
                                            auto sessionOp = GattSession::FromDeviceIdAsync(device.BluetoothDeviceId());
                                            sessionOp.Completed([weakConnection, connectionGuard](
                                                                    IAsyncOperation<GattSession> const & sessionAsyncOp,
                                                                    AsyncStatus sessionStatus) {
                                                RunWinRTCallback("GattSession::FromDeviceIdAsync completion", [&] {
                                                    auto conn = weakConnection.lock();
                                                    if (!conn || !connectionGuard.IsValid() || conn->IsClosed() ||
                                                        sessionStatus != AsyncStatus::Completed)
                                                    {
                                                        return;
                                                    }
                                                    GattSession session = sessionAsyncOp.GetResults();
                                                    if (session)
                                                    {
                                                        conn->SetMTU(static_cast<uint16_t>(session.MaxPduSize()));
                                                    }
                                                });
                                            });
                                        } catch (winrt::hresult_error const &)
                                        {}

                                        BLEManagerImpl::HandleNewConnection(std::move(connection));
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
    } catch (winrt::hresult_error const & error)
    {
        ChipLogError(Ble, "Connect/discover failed: 0x%08lx", static_cast<unsigned long>(error.code().value));
        BLEManagerImpl::HandleConnectFailed(CHIP_ERROR_INTERNAL);
    }
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
