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
 *          Provides an implementation of the BLEManager singleton object
 *          for native Windows hosts.
 */

#include <platform/ConnectivityManager.h>

#include "platform/internal/BLEManager.h"

#include <new>
#include <utility>
#include <vector>

#include <ble/Ble.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/CodeUtils.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/Windows/BleCentral.h>
#include <platform/Windows/BleConnection.h>
#include <platform/Windows/BlePeripheral.h>

using namespace ::chip::Ble;

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

constexpr System::Clock::Timeout kNewConnectionScanTimeout = System::Clock::Seconds16(20);

struct ConnectionWorkContext
{
    explicit ConnectionWorkContext(std::shared_ptr<WinRTBleConnection> value) : connection(std::move(value)) {}

    std::shared_ptr<WinRTBleConnection> connection;
};

struct PeripheralRemovalWorkContext
{
    explicit PeripheralRemovalWorkContext(uint64_t value) : sessionKey(value) {}

    uint64_t sessionKey;
};

struct PeripheralWriteWorkContext
{
    PeripheralWriteWorkContext(const uint8_t * value, size_t length) : bytes(value, value + length) {}

    std::vector<uint8_t> bytes;
};

} // namespace

Global<BLEManagerImpl> BLEManagerImpl::sInstance;

CHIP_ERROR BLEManagerImpl::_Init()
{
    ReturnErrorOnFailure(BleLayer::Init(this, this, this, &DeviceLayer::SystemLayer()));

    // Allocated here (not as in-class member initializers) because the real
    // BleCentralScanner/BlePeripheralServer definitions -- and therefore
    // make_shared<T> -- are only visible in this .cpp; see BLEManagerImpl.h.
    if (!mCentralScanner)
    {
        mCentralScanner = std::make_shared<BleCentralScanner>();
    }
    if (!mPeripheralServer)
    {
        mPeripheralServer = std::make_shared<BlePeripheralServer>();
    }

    mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Enabled;
    mFlags.ClearAll().Set(Flags::kAdvertisingEnabled, CHIP_DEVICE_CONFIG_CHIPOBLE_ENABLE_ADVERTISING_AUTOSTART && !mIsCentral);
    mFlags.Set(Flags::kFastAdvertisingEnabled, true);

    memset(mDeviceName, 0, sizeof(mDeviceName));

    return DeviceLayer::SystemLayer().ScheduleLambda([this] { DriveBLEState(); });
}

void BLEManagerImpl::_Shutdown()
{
    DeviceLayer::SystemLayer().CancelTimer(HandleAdvertisingTimer, this);

    if (mCentralScanner)
    {
        mCentralScanner->Shutdown();
    }
    if (mPeripheralServer)
    {
        mPeripheralServer->Shutdown();
    }

    if (mConnection)
    {
        mConnection->Close();
        mConnection.reset();
    }
}

CHIP_ERROR BLEManagerImpl::ConfigureBle(uint32_t aAdapterId, bool aIsCentral)
{
    (void) aAdapterId; // WinRT does not support selecting among multiple local Bluetooth adapters.
    mIsCentral = aIsCentral;
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::_SetAdvertisingEnabled(bool val)
{
    mFlags.Set(Flags::kAdvertisingEnabled, val);
    return DeviceLayer::SystemLayer().ScheduleLambda([this] { DriveBLEState(); });
}

CHIP_ERROR BLEManagerImpl::_SetAdvertisingMode(BLEAdvertisingMode mode)
{
    switch (mode)
    {
    case BLEAdvertisingMode::kFastAdvertising:
        mFlags.Set(Flags::kFastAdvertisingEnabled, true);
        break;
    case BLEAdvertisingMode::kSlowAdvertising:
        mFlags.Set(Flags::kFastAdvertisingEnabled, false);
        break;
    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    mFlags.Set(Flags::kAdvertisingRefreshNeeded);
    return DeviceLayer::SystemLayer().ScheduleLambda([this] { DriveBLEState(); });
}

CHIP_ERROR BLEManagerImpl::_GetDeviceName(char * buf, size_t bufSize)
{
    VerifyOrReturnError(strlen(mDeviceName) < bufSize, CHIP_ERROR_BUFFER_TOO_SMALL);
    Platform::CopyString(buf, bufSize, mDeviceName);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::_SetDeviceName(const char * deviceName)
{
    VerifyOrReturnError(mServiceMode != ConnectivityManager::kCHIPoBLEServiceMode_NotSupported,
                        CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);

    if (deviceName != nullptr && deviceName[0] != 0)
    {
        VerifyOrReturnError(strlen(deviceName) < kMaxDeviceNameLength, CHIP_ERROR_INVALID_ARGUMENT);
        Platform::CopyString(mDeviceName, sizeof(mDeviceName), deviceName);
        mFlags.Set(Flags::kUseCustomDeviceName);
    }
    else
    {
        uint16_t discriminator;
        ReturnErrorOnFailure(GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator));
        snprintf(mDeviceName, sizeof(mDeviceName), "%s%04u", CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX, discriminator);
        mDeviceName[kMaxDeviceNameLength] = 0;
        mFlags.Clear(Flags::kUseCustomDeviceName);
    }
    return CHIP_NO_ERROR;
}

uint16_t BLEManagerImpl::_NumConnections()
{
    return mConnection ? 1 : 0;
}

void BLEManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
    switch (event->Type)
    {
    case DeviceEventType::kCHIPoBLESubscribe:
        HandleSubscribeReceived(event->CHIPoBLESubscribe.ConId, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        NotifyCHIPoBLEConnectionEstablished();
        break;
    case DeviceEventType::kCHIPoBLEUnsubscribe:
        HandleUnsubscribeReceived(event->CHIPoBLEUnsubscribe.ConId, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        NotifyCHIPoBLEConnectionClosed();
        break;
    case DeviceEventType::kCHIPoBLEWriteReceived:
        HandleWriteReceived(event->CHIPoBLEWriteReceived.ConId, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID,
                            PacketBufferHandle::Adopt(event->CHIPoBLEWriteReceived.Data));
        break;
    case DeviceEventType::kCHIPoBLEIndicateConfirm:
        HandleIndicationConfirmation(event->CHIPoBLEIndicateConfirm.ConId, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        break;
    case DeviceEventType::kCHIPoBLEConnectionError:
        HandleConnectionError(event->CHIPoBLEConnectionError.ConId, event->CHIPoBLEConnectionError.Reason);
        break;
    case DeviceEventType::kServiceProvisioningChange:
        mFlags.Clear(Flags::kAdvertisingRefreshNeeded);
        DriveBLEState();
        break;
    case DeviceEventType::kPlatformWindowsBLECentralConnected:
        DeviceLayer::SystemLayer().CancelTimer(HandleScanTimeout, this);
        mCentralScanner->Cancel();
        if (event->Platform.BLECentralConnected.mConnection != nullptr)
        {
            BleConnectionDelegate::OnConnectionComplete(mAppState, event->Platform.BLECentralConnected.mConnection);
        }
        break;
    case DeviceEventType::kPlatformWindowsBLECentralConnectFailed:
        DeviceLayer::SystemLayer().CancelTimer(HandleScanTimeout, this);
        mCentralScanner->Cancel();
        BleConnectionDelegate::OnConnectionError(mAppState, event->Platform.BLECentralConnectFailed.mError);
        break;
    case DeviceEventType::kPlatformWindowsBLEWriteComplete:
        HandleWriteConfirmation(event->Platform.BLEWriteComplete.mConnection, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID);
        break;
    case DeviceEventType::kPlatformWindowsBLESubscribeOpComplete:
        if (event->Platform.BLESubscribeOpComplete.mIsSubscribed)
        {
            HandleSubscribeComplete(event->Platform.BLESubscribeOpComplete.mConnection, &CHIP_BLE_SVC_ID,
                                    &Ble::CHIP_BLE_CHAR_2_UUID);
        }
        else
        {
            HandleUnsubscribeComplete(event->Platform.BLESubscribeOpComplete.mConnection, &CHIP_BLE_SVC_ID,
                                      &Ble::CHIP_BLE_CHAR_2_UUID);
        }
        break;
    case DeviceEventType::kPlatformWindowsBLEIndicationReceived:
        HandleIndicationReceived(event->Platform.BLEIndicationReceived.mConnection, &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID,
                                 PacketBufferHandle::Adopt(event->Platform.BLEIndicationReceived.mData));
        break;
    case DeviceEventType::kPlatformWindowsBLEPeripheralRegisterAppComplete:
        if (event->Platform.BLEPeripheralRegisterAppComplete.mError == CHIP_NO_ERROR)
        {
            mFlags.Clear(Flags::kControlOpInProgress);
            mFlags.Set(Flags::kAppRegistered);
            DriveBLEState();
        }
        else
        {
            DisableBLEService(event->Platform.BLEPeripheralRegisterAppComplete.mError);
        }
        break;
    case DeviceEventType::kPlatformWindowsBLEPeripheralAdvStartComplete:
        mFlags.Clear(Flags::kControlOpInProgress).Clear(Flags::kAdvertisingRefreshNeeded);
        if (event->Platform.BLEPeripheralAdvStartComplete.mError == CHIP_NO_ERROR)
        {
            mFlags.Set(Flags::kAdvertising);
            NotifyCHIPoBLEAdvertisingChange(kActivity_Started);
        }
        else
        {
            DisableBLEService(event->Platform.BLEPeripheralAdvStartComplete.mError);
        }
        break;
    case DeviceEventType::kPlatformWindowsBLEPeripheralAdvStopComplete:
        mFlags.Clear(Flags::kControlOpInProgress).Clear(Flags::kAdvertisingRefreshNeeded);
        if (mFlags.Has(Flags::kAdvertising))
        {
            mFlags.Clear(Flags::kAdvertising);
            NotifyCHIPoBLEAdvertisingChange(kActivity_Stopped);
        }
        break;
    default:
        break;
    }
}

uint16_t BLEManagerImpl::GetMTU(BLE_CONNECTION_OBJECT conId) const
{
    VerifyOrReturnValue(conId != BLE_CONNECTION_UNINITIALIZED, 0,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    return conId->GetMTU();
}

CHIP_ERROR BLEManagerImpl::SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId,
                                                   const ChipBleUUID * charId)
{
    VerifyOrReturnError(conId != BLE_CONNECTION_UNINITIALIZED, BLE_ERROR_GATT_SUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    VerifyOrReturnError(Ble::UUIDsMatch(svcId, &CHIP_BLE_SVC_ID), BLE_ERROR_GATT_SUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "SubscribeCharacteristic() called with invalid service ID"));
    VerifyOrReturnError(Ble::UUIDsMatch(charId, &Ble::CHIP_BLE_CHAR_2_UUID), BLE_ERROR_GATT_SUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "SubscribeCharacteristic() called with invalid characteristic ID"));
    return conId->SubscribeCharacteristic();
}

CHIP_ERROR BLEManagerImpl::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId,
                                                     const ChipBleUUID * charId)
{
    VerifyOrReturnError(conId != BLE_CONNECTION_UNINITIALIZED, BLE_ERROR_GATT_UNSUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    VerifyOrReturnError(Ble::UUIDsMatch(svcId, &CHIP_BLE_SVC_ID), BLE_ERROR_GATT_UNSUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "UnsubscribeCharacteristic() called with invalid service ID"));
    VerifyOrReturnError(Ble::UUIDsMatch(charId, &Ble::CHIP_BLE_CHAR_2_UUID), BLE_ERROR_GATT_UNSUBSCRIBE_FAILED,
                        ChipLogError(DeviceLayer, "UnsubscribeCharacteristic() called with invalid characteristic ID"));
    return conId->UnsubscribeCharacteristic();
}

CHIP_ERROR BLEManagerImpl::CloseConnection(BLE_CONNECTION_OBJECT conId)
{
    VerifyOrReturnError(conId != BLE_CONNECTION_UNINITIALIZED, CHIP_ERROR_INTERNAL,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    ChipLogProgress(DeviceLayer, "Closing BLE GATT connection (con %p)", conId);
    return conId->CloseConnection();
}

CHIP_ERROR BLEManagerImpl::SendIndication(BLE_CONNECTION_OBJECT conId, const ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                                          chip::System::PacketBufferHandle pBuf)
{
    VerifyOrReturnError(conId != BLE_CONNECTION_UNINITIALIZED, BLE_ERROR_GATT_INDICATE_FAILED,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    return conId->SendIndication(std::move(pBuf));
}

CHIP_ERROR BLEManagerImpl::SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                            const Ble::ChipBleUUID * charId, chip::System::PacketBufferHandle pBuf)
{
    VerifyOrReturnError(conId != BLE_CONNECTION_UNINITIALIZED, BLE_ERROR_GATT_WRITE_FAILED,
                        ChipLogError(DeviceLayer, "BLE connection is not initialized in %s", __func__));
    VerifyOrReturnError(Ble::UUIDsMatch(svcId, &CHIP_BLE_SVC_ID), BLE_ERROR_GATT_WRITE_FAILED,
                        ChipLogError(DeviceLayer, "SendWriteRequest() called with invalid service ID"));
    VerifyOrReturnError(Ble::UUIDsMatch(charId, &Ble::CHIP_BLE_CHAR_1_UUID), BLE_ERROR_GATT_WRITE_FAILED,
                        ChipLogError(DeviceLayer, "SendWriteRequest() called with invalid characteristic ID"));
    return conId->SendWriteRequest(std::move(pBuf));
}

void BLEManagerImpl::NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId)
{
    ChipLogProgress(Ble, "Got notification regarding chip connection closure");
    if (conId != nullptr)
    {
        conId->Close();
    }
    if (mConnection.get() == conId)
    {
        mConnection.reset();
    }
    DriveBLEState();
}

void BLEManagerImpl::RegisterConnection(std::shared_ptr<WinRTBleConnection> connection)
{
    BLEManagerImpl & self = BLEMgrImpl();
    // CHIPoBLE, like every other platform's BLEManagerImpl, supports exactly
    // one concurrent BLE connection. A superseding connection replaces
    // (and safely closes) any prior one rather than being refused, matching
    // the "one active connection at a time" contract the rest of this class
    // assumes.
    if (self.mConnection && self.mConnection != connection)
    {
        self.mConnection->Close();
    }
    self.mConnection = std::move(connection);
}

void BLEManagerImpl::NewConnection(BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator)
{
    (void) bleLayer;
    mAppState                        = appState;
    SetupDiscriminator discriminator = connDiscriminator;
    TEMPORARY_RETURN_IGNORED DeviceLayer::SystemLayer().ScheduleLambda([this, discriminator] {
        CHIP_ERROR err = mCentralScanner->ScanAndConnect(Span<const SetupDiscriminator>(&discriminator, 1));
        if (err == CHIP_NO_ERROR)
        {
            err = DeviceLayer::SystemLayer().StartTimer(kNewConnectionScanTimeout, HandleScanTimeout, this);
        }
        if (err != CHIP_NO_ERROR)
        {
            mCentralScanner->Cancel();
            BleConnectionDelegate::OnConnectionError(mAppState, err);
        }
    });
}

CHIP_ERROR BLEManagerImpl::CancelConnection()
{
    if (mCentralScanner)
    {
        mCentralScanner->Cancel();
    }
    DeviceLayer::SystemLayer().CancelTimer(HandleScanTimeout, this);
    return CHIP_NO_ERROR;
}

void BLEManagerImpl::DriveBLEState()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    if (!mFlags.Has(Flags::kAsyncInitCompleted))
    {
        mFlags.Set(Flags::kAsyncInitCompleted);
        return;
    }

    VerifyOrReturn(!mFlags.Has(Flags::kControlOpInProgress));

    if (mIsCentral)
    {
        // Central role has nothing to drive continuously: scanning/connect is
        // initiated on demand by NewConnection().
        return;
    }

    if (mServiceMode != ConnectivityManager::kCHIPoBLEServiceMode_Enabled)
    {
        if (mFlags.Has(Flags::kAdvertising))
        {
            mFlags.Set(Flags::kControlOpInProgress);
            err = mPeripheralServer->StopAdvertising();
        }
        if (err != CHIP_NO_ERROR)
        {
            DisableBLEService(err);
        }
        return;
    }

    if (!mFlags.Has(Flags::kAppRegistered))
    {
        mFlags.Set(Flags::kControlOpInProgress);
        err = mPeripheralServer->RegisterGattApplication();
        if (err != CHIP_NO_ERROR)
        {
            DisableBLEService(err);
        }
        return;
    }

    if (mFlags.Has(Flags::kAdvertisingEnabled))
    {
        if (!mFlags.Has(Flags::kAdvertising) || mFlags.Has(Flags::kAdvertisingRefreshNeeded))
        {
            mFlags.Set(Flags::kControlOpInProgress);
            err = mPeripheralServer->StartAdvertising(mDeviceName, mFlags.Has(Flags::kFastAdvertisingEnabled));
            if (err != CHIP_NO_ERROR)
            {
                DisableBLEService(err);
            }
        }
    }
    else if (mFlags.Has(Flags::kAdvertising))
    {
        mFlags.Set(Flags::kControlOpInProgress);
        err = mPeripheralServer->StopAdvertising();
        if (err != CHIP_NO_ERROR)
        {
            DisableBLEService(err);
        }
    }
}

void BLEManagerImpl::DisableBLEService(CHIP_ERROR err)
{
    ChipLogError(DeviceLayer, "Disabling CHIPoBLE service due to error: %" CHIP_ERROR_FORMAT, err.Format());
    mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Disabled;
    mFlags.Clear(Flags::kControlOpInProgress);
    DeviceLayer::SystemLayer().CancelTimer(HandleAdvertisingTimer, this);
}

void BLEManagerImpl::HandleAdvertisingTimer(chip::System::Layer *, void * appState)
{
    auto * self = static_cast<BLEManagerImpl *>(appState);
    if (self->mFlags.Has(Flags::kFastAdvertisingEnabled))
    {
        ChipLogDetail(DeviceLayer, "bleAdv Timeout : Start slow advertisement");
        TEMPORARY_RETURN_IGNORED self->_SetAdvertisingMode(BLEAdvertisingMode::kSlowAdvertising);
    }
}

void BLEManagerImpl::HandleScanTimeout(chip::System::Layer *, void * appState)
{
    auto * self = static_cast<BLEManagerImpl *>(appState);
    self->mCentralScanner->LogScanSummary();
    self->mCentralScanner->Cancel();
    self->BleConnectionDelegate::OnConnectionError(self->mAppState, CHIP_ERROR_TIMEOUT);
}

void BLEManagerImpl::HandleNewConnection(std::shared_ptr<WinRTBleConnection> connection)
{
    auto * context = new (std::nothrow) ConnectionWorkContext(std::move(connection));
    if (context == nullptr)
    {
        HandleConnectFailed(CHIP_ERROR_NO_MEMORY);
        return;
    }

    CHIP_ERROR error = PlatformMgr().ScheduleWork(&BLEManagerImpl::AdoptCentralConnectionWork, reinterpret_cast<intptr_t>(context));
    if (error != CHIP_NO_ERROR)
    {
        delete context;
        HandleConnectFailed(error);
    }
}

void BLEManagerImpl::AdoptCentralConnectionWork(intptr_t rawContext)
{
    std::unique_ptr<ConnectionWorkContext> context(reinterpret_cast<ConnectionWorkContext *>(rawContext));
    RegisterConnection(context->connection);
    PostCentralConnected(context->connection.get());
}

void BLEManagerImpl::PostCentralConnected(BLE_CONNECTION_OBJECT conId)
{
    ChipDeviceEvent event;
    event.Type                                     = DeviceEventType::kPlatformWindowsBLECentralConnected;
    event.Platform.BLECentralConnected.mConnection = conId;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleConnectFailed(CHIP_ERROR error)
{
    ChipDeviceEvent event;
    event.Type                                    = DeviceEventType::kPlatformWindowsBLECentralConnectFailed;
    event.Platform.BLECentralConnectFailed.mError = error;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleWriteComplete(BLE_CONNECTION_OBJECT conId)
{
    ChipDeviceEvent event;
    event.Type                                  = DeviceEventType::kPlatformWindowsBLEWriteComplete;
    event.Platform.BLEWriteComplete.mConnection = conId;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleSubscribeOpComplete(BLE_CONNECTION_OBJECT conId, bool subscribed)
{
    ChipDeviceEvent event;
    event.Type                                          = DeviceEventType::kPlatformWindowsBLESubscribeOpComplete;
    event.Platform.BLESubscribeOpComplete.mConnection   = conId;
    event.Platform.BLESubscribeOpComplete.mIsSubscribed = subscribed;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleTXCharChanged(BLE_CONNECTION_OBJECT conId, const uint8_t * value, size_t len)
{
    System::PacketBufferHandle buf(System::PacketBufferHandle::NewWithData(value, len));
    VerifyOrReturn(!buf.IsNull(), ChipLogError(DeviceLayer, "Failed to allocate packet buffer in %s", __func__));

    ChipDeviceEvent event;
    event.Type                                       = DeviceEventType::kPlatformWindowsBLEIndicationReceived;
    event.Platform.BLEIndicationReceived.mConnection = conId;
    event.Platform.BLEIndicationReceived.mData       = std::move(buf).UnsafeRelease();
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleConnectionClosed(BLE_CONNECTION_OBJECT conId)
{
    HandleConnectionError(conId, BLE_ERROR_REMOTE_DEVICE_DISCONNECTED);
}

void BLEManagerImpl::HandleConnectionError(BLE_CONNECTION_OBJECT conId, CHIP_ERROR error)
{
    ChipDeviceEvent event;
    event.Type                           = DeviceEventType::kCHIPoBLEConnectionError;
    event.CHIPoBLEConnectionError.ConId  = conId;
    event.CHIPoBLEConnectionError.Reason = error;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleTXComplete(BLE_CONNECTION_OBJECT conId)
{
    ChipDeviceEvent event;
    event.Type                          = DeviceEventType::kCHIPoBLEIndicateConfirm;
    event.CHIPoBLEIndicateConfirm.ConId = conId;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR error)
{
    ChipDeviceEvent event;
    event.Type                                             = DeviceEventType::kPlatformWindowsBLEPeripheralRegisterAppComplete;
    event.Platform.BLEPeripheralRegisterAppComplete.mError = error;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyBLEPeripheralAdvStartComplete(CHIP_ERROR error)
{
    ChipDeviceEvent event;
    event.Type                                          = DeviceEventType::kPlatformWindowsBLEPeripheralAdvStartComplete;
    event.Platform.BLEPeripheralAdvStartComplete.mError = error;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyBLEPeripheralAdvStopComplete(CHIP_ERROR error)
{
    ChipDeviceEvent event;
    event.Type                                         = DeviceEventType::kPlatformWindowsBLEPeripheralAdvStopComplete;
    event.Platform.BLEPeripheralAdvStopComplete.mError = error;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleSubscribedClientAdded(std::shared_ptr<WinRTBleConnection> connection)
{
    auto * context = new (std::nothrow) ConnectionWorkContext(std::move(connection));
    VerifyOrReturn(context != nullptr, ChipLogError(Ble, "Failed to allocate peripheral connection work"));

    CHIP_ERROR error =
        PlatformMgr().ScheduleWork(&BLEManagerImpl::AdoptPeripheralConnectionWork, reinterpret_cast<intptr_t>(context));
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule peripheral connection adoption: %" CHIP_ERROR_FORMAT, error.Format());
        delete context;
    }
}

void BLEManagerImpl::AdoptPeripheralConnectionWork(intptr_t rawContext)
{
    std::unique_ptr<ConnectionWorkContext> context(reinterpret_cast<ConnectionWorkContext *>(rawContext));
    RegisterConnection(context->connection);
    PostPeripheralSubscribed(context->connection.get());
}

void BLEManagerImpl::PostPeripheralSubscribed(BLE_CONNECTION_OBJECT connection)
{
    ChipDeviceEvent event;
    event.Type                    = DeviceEventType::kCHIPoBLESubscribe;
    event.CHIPoBLESubscribe.ConId = connection;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleSubscribedClientRemoved(uint64_t sessionKey)
{
    auto * context = new (std::nothrow) PeripheralRemovalWorkContext(sessionKey);
    VerifyOrReturn(context != nullptr, ChipLogError(Ble, "Failed to allocate peripheral removal work"));

    CHIP_ERROR error =
        PlatformMgr().ScheduleWork(&BLEManagerImpl::RemovePeripheralConnectionWork, reinterpret_cast<intptr_t>(context));
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule peripheral removal: %" CHIP_ERROR_FORMAT, error.Format());
        delete context;
    }
}

void BLEManagerImpl::RemovePeripheralConnectionWork(intptr_t rawContext)
{
    std::unique_ptr<PeripheralRemovalWorkContext> context(reinterpret_cast<PeripheralRemovalWorkContext *>(rawContext));
    BLEManagerImpl & self = BLEMgrImpl();
    VerifyOrReturn(self.mConnection && self.mConnection->GetRole() == BleConnectionRole::kPeripheral);
    VerifyOrReturn(self.mConnection->PeripheralSessionKey() == context->sessionKey);

    ChipDeviceEvent event;
    event.Type                      = DeviceEventType::kCHIPoBLEUnsubscribe;
    event.CHIPoBLEUnsubscribe.ConId = self.mConnection.get();
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::HandleRXCharWrite(const uint8_t * value, size_t len)
{
    PeripheralWriteWorkContext * context = nullptr;
    try
    {
        context = new PeripheralWriteWorkContext(value, len);
    } catch (std::bad_alloc const &)
    {
        ChipLogError(Ble, "Failed to allocate peripheral write work");
        return;
    }

    CHIP_ERROR error = PlatformMgr().ScheduleWork(&BLEManagerImpl::HandlePeripheralWriteWork, reinterpret_cast<intptr_t>(context));
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule peripheral write: %" CHIP_ERROR_FORMAT, error.Format());
        delete context;
    }
}

void BLEManagerImpl::HandlePeripheralWriteWork(intptr_t rawContext)
{
    std::unique_ptr<PeripheralWriteWorkContext> context(reinterpret_cast<PeripheralWriteWorkContext *>(rawContext));
    BLEManagerImpl & self = BLEMgrImpl();
    VerifyOrReturn(self.mConnection && self.mConnection->GetRole() == BleConnectionRole::kPeripheral,
                   ChipLogError(Ble, "Write received with no active peripheral connection"));

    System::PacketBufferHandle buf(System::PacketBufferHandle::NewWithData(context->bytes.data(), context->bytes.size()));
    VerifyOrReturn(!buf.IsNull(), ChipLogError(DeviceLayer, "Failed to allocate packet buffer in %s", __func__));

    ChipDeviceEvent event;
    event.Type                        = DeviceEventType::kCHIPoBLEWriteReceived;
    event.CHIPoBLEWriteReceived.ConId = self.mConnection.get();
    event.CHIPoBLEWriteReceived.Data  = std::move(buf).UnsafeRelease();
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyCHIPoBLEConnectionEstablished()
{
    ChipDeviceEvent event;
    event.Type = DeviceEventType::kCHIPoBLEConnectionEstablished;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyCHIPoBLEConnectionClosed()
{
    ChipDeviceEvent event;
    event.Type = DeviceEventType::kCHIPoBLEConnectionClosed;
    PlatformMgr().PostEventOrDie(&event);
}

void BLEManagerImpl::NotifyCHIPoBLEAdvertisingChange(enum ActivityChange change)
{
    ChipDeviceEvent event;
    event.Type                             = DeviceEventType::kCHIPoBLEAdvertisingChange;
    event.CHIPoBLEAdvertisingChange.Result = change;
    PlatformMgr().PostEventOrDie(&event);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
