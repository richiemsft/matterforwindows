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

// Deterministic, hardware-free smoke for the native Windows C++/WinRT BLE
// backend (src/platform/Windows/{BLEManagerImpl,BleCentral,BlePeripheral,
// BleConnection,BleAdvertisingData,BleCallbackGuard}). It exercises:
//
//   1. Service-data parsing/encoding and SetupDiscriminator matching --
//      pure logic, no WinRT/Bluetooth involved.
//   2. BleCallbackEpoch/BleCallbackGuard invalidation -- proves a guard
//      created before Invalidate() reports stale afterward.
//   3. WinRTBleConnection lifecycle -- construct/Close()/idempotent
//      double-Close(), and that a guard made before Close() is invalidated
//      by it (the same mechanism that protects a real WinRT completion
//      handler from acting on a connection that has since been torn down).
//   4. BLEManagerImpl peripheral- and central-role Init()/Shutdown() and
//      advertising/device-name state through the canonical PlatformMgr()
//      lifecycle. DriveBLEState() will attempt real WinRT GATT-server/
//      scanning calls; on a host with no usable Bluetooth radio those calls
//      fail asynchronously (never crash) and the service is disabled, which
//      this smoke treats as a pass -- proving graceful degradation is
//      exactly as important here as the hardware-present path, and hardware
//      is not guaranteed to be present. Live pairing between a real central
//      and peripheral is out of scope for this deterministic smoke; see
//      docs/guides/windows.md for the hardware-only acceptance status.
//
// Returns 0 on success, 1 on any failure.

#include <platform/CHIPDeviceLayer.h>
#include <platform/Windows/BleAdvertisingData.h>
#include <platform/Windows/BleCallbackGuard.h>
#include <platform/Windows/BleConnection.h>
#include <platform/internal/BLEManager.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace chip;
using namespace chip::Ble;
using namespace chip::DeviceLayer;
using namespace chip::DeviceLayer::Internal;
using namespace std::chrono_literals;

namespace {

int gStep = 0;

#define CHECK(condition)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        ++gStep;                                                                                                                   \
        if (!(condition))                                                                                                          \
        {                                                                                                                          \
            std::printf("Windows BLE smoke failed at step %d: %s\n", gStep, #condition);                                           \
            return 1;                                                                                                              \
        }                                                                                                                          \
    } while (0)

int TestServiceDataParsing()
{
    // Round trip: encode a known discriminator/vendor/product, then parse it back.
    ChipBLEDeviceIdentificationInfo info;
    info.Init();
    info.SetDeviceDiscriminator(0xABC & ChipBLEDeviceIdentificationInfo::kDiscriminatorMask);
    info.SetVendorId(0x1234);
    info.SetProductId(0x5678);

    uint8_t encoded[kChipServiceDataSectionSize];
    EncodeChipServiceData(info, encoded);

    ChipBLEDeviceIdentificationInfo decoded;
    CHECK(ParseChipServiceData(ByteSpan(encoded, sizeof(encoded)), decoded));
    CHECK(decoded.GetDeviceDiscriminator() == info.GetDeviceDiscriminator());
    CHECK(decoded.GetVendorId() == info.GetVendorId());
    CHECK(decoded.GetProductId() == info.GetProductId());

    uint8_t payload[kChipServiceDataPayloadSize];
    EncodeChipServiceDataPayload(info, payload);
    CHECK(memcmp(payload, encoded + sizeof(uint16_t), sizeof(payload)) == 0);

    // Wrong UUID (first two bytes) must not parse as a Matter service.
    uint8_t wrongUuid[kChipServiceDataSectionSize];
    memcpy(wrongUuid, encoded, sizeof(wrongUuid));
    wrongUuid[0] ^= 0xFF;
    ChipBLEDeviceIdentificationInfo ignored;
    CHECK(!ParseChipServiceData(ByteSpan(wrongUuid, sizeof(wrongUuid)), ignored));

    // Wrong length must not parse.
    CHECK(!ParseChipServiceData(ByteSpan(encoded, sizeof(encoded) - 1), ignored));

    // Discriminator matching: long-form matches exactly; short-form matches
    // only the high bits.
    SetupDiscriminator longDiscriminator;
    longDiscriminator.SetLongValue(decoded.GetDeviceDiscriminator());
    CHECK(longDiscriminator.MatchesLongDiscriminator(decoded.GetDeviceDiscriminator()));
    CHECK(!longDiscriminator.MatchesLongDiscriminator(static_cast<uint16_t>(decoded.GetDeviceDiscriminator() ^ 0x001)));

    SetupDiscriminator shortDiscriminator;
    shortDiscriminator.SetShortValue(longDiscriminator.GetShortValue());
    CHECK(shortDiscriminator.MatchesLongDiscriminator(decoded.GetDeviceDiscriminator()));

    return 0;
}

int TestCallbackGuardInvalidation()
{
    BleCallbackEpoch epoch;
    BleCallbackGuard guardBeforeInvalidate = epoch.MakeGuard();
    CHECK(guardBeforeInvalidate.IsValid());

    epoch.Invalidate();
    CHECK(!guardBeforeInvalidate.IsValid());

    // A guard made *after* Invalidate() reflects the new epoch and is valid
    // again until the next Invalidate().
    BleCallbackGuard guardAfterInvalidate = epoch.MakeGuard();
    CHECK(guardAfterInvalidate.IsValid());
    epoch.Invalidate();
    CHECK(!guardAfterInvalidate.IsValid());

    return 0;
}

int TestConnectionLifecycle()
{
    auto connection = std::make_shared<WinRTBleConnection>(BleConnectionRole::kCentral);
    CHECK(connection->GetRole() == BleConnectionRole::kCentral);
    CHECK(!connection->IsClosed());
    CHECK(connection->GetMTU() == 0);

    connection->SetMTU(185);
    CHECK(connection->GetMTU() == 185);

    // A guard taken out for an operation "in flight" against this connection
    // must be invalidated the moment the connection closes -- this is the
    // exact mechanism a real WinRT async completion handler relies on to
    // recognize "this connection was closed/cancelled while I was pending"
    // and do nothing instead of acting on torn-down state.
    BleCallbackGuard inFlightGuard = connection->MakeGuard();
    CHECK(inFlightGuard.IsValid());

    connection->Close();
    CHECK(connection->IsClosed());
    CHECK(!inFlightGuard.IsValid());

    // Close() must be idempotent: a second call does not crash or re-fire
    // invalidation-dependent side effects.
    connection->Close();
    CHECK(connection->IsClosed());

    // A weak_ptr the way a real completion handler would capture one
    // correctly observes destruction once the owning shared_ptr is dropped.
    std::weak_ptr<WinRTBleConnection> weakConnection = connection;
    CHECK(!weakConnection.expired());
    connection.reset();
    CHECK(weakConnection.expired());

    return 0;
}

int TestManagerLifecyclePeripheral()
{
    CHECK(BLEMgrImpl().ConfigureBle(0, /* aIsCentral = */ false) == CHIP_NO_ERROR);
    CHECK(PlatformMgr().InitChipStack() == CHIP_NO_ERROR);

    CHECK(BLEMgr().SetDeviceName("WINTEST") == CHIP_NO_ERROR);
    char nameBuf[32] = {};
    CHECK(BLEMgr().GetDeviceName(nameBuf, sizeof(nameBuf)) == CHIP_NO_ERROR);
    CHECK(strcmp(nameBuf, "WINTEST") == 0);

    CHECK(BLEMgr().SetAdvertisingEnabled(true) == CHIP_NO_ERROR);
    CHECK(BLEMgr().IsAdvertisingEnabled());
    CHECK(BLEMgr().SetAdvertisingEnabled(false) == CHIP_NO_ERROR);
    CHECK(!BLEMgr().IsAdvertisingEnabled());

    // Give the event loop a moment to run DriveBLEState()'s async
    // registration attempt (which may fail gracefully with no Bluetooth
    // radio present; see file header).
    CHECK(PlatformMgr().StartEventLoopTask() == CHIP_NO_ERROR);
    std::this_thread::sleep_for(200ms);
    CHECK(PlatformMgr().StopEventLoopTask() == CHIP_NO_ERROR);

    CHECK(BLEMgr().NumConnections() == 0);

    PlatformMgr().Shutdown();
    return 0;
}

int TestManagerLifecycleCentral()
{
    CHECK(BLEMgrImpl().ConfigureBle(0, /* aIsCentral = */ true) == CHIP_NO_ERROR);
    CHECK(PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    CHECK(BLEMgr().NumConnections() == 0);

    PlatformMgr().Shutdown();
    return 0;
}

} // namespace

int main()
{
    if (int result = TestServiceDataParsing())
    {
        return result;
    }
    if (int result = TestCallbackGuardInvalidation())
    {
        return result;
    }
    if (int result = TestConnectionLifecycle())
    {
        return result;
    }
    if (int result = TestManagerLifecyclePeripheral())
    {
        return result;
    }
    if (int result = TestManagerLifecycleCentral())
    {
        return result;
    }

    std::printf("Windows BLE smoke passed (%d checks)\n", gStep);
    return 0;
}
