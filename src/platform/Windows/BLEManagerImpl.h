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
 *          Provides an implementation of the BLEManager singleton object for
 *          native Windows hosts, backed by C++/WinRT
 *          Windows.Devices.Bluetooth.
 *
 *          Like every other platform's BLEManagerImpl, this is either a
 *          central (controller/commissioner) or a peripheral
 *          (commissionee), selected once via ConfigureBle() before Init();
 *          it does not run both roles at once. Central scanning/connection
 *          is implemented by BleCentralScanner (BleCentral.h); peripheral
 *          advertising/GATT server is implemented by BlePeripheralServer
 *          (BlePeripheral.h). Every WinRT callback -- async completions and
 *          events alike -- is marshaled onto PlatformMgr() by posting a
 *          ChipDeviceEvent before any Matter callback runs; see
 *          BleCallbackGuard.h and BleConnection.h for how late callbacks
 *          (after Cancel()/Shutdown()/Close()) are recognized and ignored.
 */

#pragma once

#include <memory>

#include <ble/Ble.h>
#include <lib/core/Global.h>
#include <lib/support/BitFlags.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SetupDiscriminator.h>
#include <lib/support/Span.h>

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

namespace chip {
namespace DeviceLayer {
namespace Internal {

using namespace chip::Ble;

// Forward-declared only: BleCentralScanner/BlePeripheralServer/
// WinRTBleConnection hold C++/WinRT (Windows.Devices.Bluetooth) types, which
// require C++20 coroutine support and C++ exceptions enabled (see
// src/platform/Windows/BUILD.gn's ":windows-ble-compiler-defaults" config).
// This header is reached transitively by every Windows Device Layer/
// ConnectivityManager translation unit -- most of which compile under the
// project's normal exception-free C++17 default -- so it must never name a
// WinRT type or include a WinRT header. Held via shared_ptr specifically
// because shared_ptr's destructor does not require the pointee to be a
// complete type (unlike unique_ptr's default deleter), so BLEManagerImpl
// stays constructible/destructible from any translation unit that only has
// these forward declarations; only BLEManagerImpl.cpp (and the other
// windows-ble/*.cpp files) need the real definitions to actually construct
// one.
class BleCentralScanner;
class BlePeripheralServer;
class WinRTBleConnection;

/**
 * Concrete implementation of the BLEManagerImpl singleton object for native
 * Windows hosts.
 */
class BLEManagerImpl final : public BLEManager,
                             private Ble::BleLayer,
                             private Ble::BlePlatformDelegate,
                             private Ble::BleApplicationDelegate,
                             private Ble::BleConnectionDelegate
{
    friend BLEManager;

public:
    // Selects central (controller) vs. peripheral (commissionee) operation.
    // aAdapterId is accepted for API parity with other platforms but is
    // unused: WinRT's Windows.Devices.Bluetooth surface operates against the
    // single default Bluetooth radio and does not support selecting among
    // multiple local adapters.
    CHIP_ERROR ConfigureBle(uint32_t aAdapterId, bool aIsCentral);

    // ----- Driven by BleCentral/BleConnection/BlePeripheral WinRT callbacks.
    // Every one of these is called only after the calling WinRT callback has
    // validated its BleCallbackGuard, so by the time control reaches here the
    // operation is still logically current; each function's own job is only
    // to translate that into the shared ChipDeviceEvent contract.
    static void HandleNewConnection(std::shared_ptr<WinRTBleConnection> connection);
    static void HandleConnectFailed(CHIP_ERROR error);
    static void HandleWriteComplete(BLE_CONNECTION_OBJECT conId);
    static void HandleSubscribeOpComplete(BLE_CONNECTION_OBJECT conId, bool subscribed);
    static void HandleTXCharChanged(BLE_CONNECTION_OBJECT conId, const uint8_t * value, size_t len);
    static void HandleConnectionClosed(BLE_CONNECTION_OBJECT conId);
    static void HandleConnectionError(BLE_CONNECTION_OBJECT conId, CHIP_ERROR error);
    static void HandleTXComplete(BLE_CONNECTION_OBJECT conId);

    static void NotifyBLEPeripheralRegisterAppComplete(CHIP_ERROR error);
    static void NotifyBLEPeripheralAdvStartComplete(CHIP_ERROR error);
    static void NotifyBLEPeripheralAdvStopComplete(CHIP_ERROR error);
    static void HandleSubscribedClientAdded(std::shared_ptr<WinRTBleConnection> connection);
    static void HandleSubscribedClientRemoved(uint64_t sessionKey);
    static void HandleRXCharWrite(const uint8_t * value, size_t len);

private:
    // ===== Members that implement the BLEManager internal interface.

    CHIP_ERROR _Init();
    void _Shutdown();
    bool _IsAdvertisingEnabled();
    CHIP_ERROR _SetAdvertisingEnabled(bool val);
    bool _IsAdvertising();
    CHIP_ERROR _SetAdvertisingMode(BLEAdvertisingMode mode);
    CHIP_ERROR _GetDeviceName(char * buf, size_t bufSize);
    CHIP_ERROR _SetDeviceName(const char * deviceName);
    uint16_t _NumConnections();
    void _OnPlatformEvent(const ChipDeviceEvent * event);
    BleLayer * _GetBleLayer();

    // ===== Members that implement virtual methods on BlePlatformDelegate.

    CHIP_ERROR SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                       const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                         const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR CloseConnection(BLE_CONNECTION_OBJECT conId) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT conId) const override;
    CHIP_ERROR SendIndication(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                              System::PacketBufferHandle pBuf) override;
    CHIP_ERROR SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId,
                                System::PacketBufferHandle pBuf) override;

    // ===== Members that implement virtual methods on BleApplicationDelegate.

    void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId) override;

    // ===== Members that implement virtual methods on BleConnectionDelegate.

    void NewConnection(BleLayer * bleLayer, void * appState, const SetupDiscriminator & connDiscriminator) override;
    void NewConnection(BleLayer *, void *, BLE_CONNECTION_OBJECT) override {}
    CHIP_ERROR CancelConnection() override;

    // ===== Members for internal use by the following friends.

    friend BLEManager & BLEMgr();
    friend BLEManagerImpl & BLEMgrImpl();

    static Global<BLEManagerImpl> sInstance;

    enum class Flags : uint16_t
    {
        kAsyncInitCompleted       = 0x0001,
        kAdvertisingEnabled       = 0x0002,
        kFastAdvertisingEnabled   = 0x0004,
        kAdvertising              = 0x0008,
        kAdvertisingRefreshNeeded = 0x0010,
        kControlOpInProgress      = 0x0020,
        kAppRegistered            = 0x0040,
        kUseCustomDeviceName      = 0x0080,
    };

    enum
    {
        kMaxDeviceNameLength = 20,
    };

    void DriveBLEState();
    void DisableBLEService(CHIP_ERROR err);
    static void HandleAdvertisingTimer(chip::System::Layer *, void * appState);
    static void HandleScanTimeout(chip::System::Layer *, void * appState);
    static void AdoptCentralConnectionWork(intptr_t context);
    static void AdoptPeripheralConnectionWork(intptr_t context);
    static void RemovePeripheralConnectionWork(intptr_t context);
    static void HandlePeripheralWriteWork(intptr_t context);
    static void RegisterConnection(std::shared_ptr<WinRTBleConnection> connection);
    static void PostCentralConnected(BLE_CONNECTION_OBJECT conId);
    static void PostPeripheralSubscribed(BLE_CONNECTION_OBJECT conId);

    void NotifyCHIPoBLEConnectionEstablished();
    void NotifyCHIPoBLEConnectionClosed();
    void NotifyCHIPoBLEAdvertisingChange(enum ActivityChange change);

    CHIPoBLEServiceMode mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_NotSupported;
    BitFlags<Flags> mFlags;
    bool mIsCentral  = false;
    void * mAppState = nullptr; // BleConnectionDelegate appState for the in-flight/most-recent NewConnection() call.

    char mDeviceName[kMaxDeviceNameLength + 1] = {};

    // Actually allocated (make_shared) lazily in _Init(), where the real
    // BleCentral.h/BlePeripheral.h definitions are visible; see the class
    // comment above on why these are shared_ptr<ForwardDeclared>.
    std::shared_ptr<BleCentralScanner> mCentralScanner;
    std::shared_ptr<BlePeripheralServer> mPeripheralServer;

    // The single active connection, central or peripheral (CHIPoBLE, like
    // every other platform's BLEManagerImpl, supports exactly one concurrent
    // BLE connection today). Owned here so BLE_CONNECTION_OBJECT (a raw
    // WinRTBleConnection *) stays valid for as long as the CHIP BLE stack
    // references it; erased only from NotifyChipConnectionClosed().
    std::shared_ptr<WinRTBleConnection> mConnection;
};

inline BLEManager & BLEMgr()
{
    return BLEManagerImpl::sInstance.get();
}

inline BLEManagerImpl & BLEMgrImpl()
{
    return BLEManagerImpl::sInstance.get();
}

inline Ble::BleLayer * BLEManagerImpl::_GetBleLayer()
{
    return this;
}

inline bool BLEManagerImpl::_IsAdvertisingEnabled()
{
    return mFlags.Has(Flags::kAdvertisingEnabled);
}

inline bool BLEManagerImpl::_IsAdvertising()
{
    return mFlags.Has(Flags::kAdvertising);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
