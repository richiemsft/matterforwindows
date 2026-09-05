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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <DeviceInfoProviderImpl.h>
#include <app/server/Server.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <data-model-providers/codegen/Instance.h>
#include <dishwasher-mode.h>
#include <laundry-washer-mode.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <microwave-oven-mode.h>
#include <operational-state-delegate-impl.h>
#include <oven-modes.h>
#include <oven-operational-state-delegate.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/TestOnlyCommissionableDataProvider.h>
#include <platform/Windows/BLEManagerImpl.h>
#include <platform/Windows/ConfigurationManagerImpl.h>
#include <rvc-modes.h>
#include <rvc-operational-state-delegate-impl.h>
#include <tcc-mode.h>
#include <tls-certificate-management-instance.h>
#include <tls-client-management-instance.h>

namespace {

using namespace chip;
using namespace chip::DeviceLayer;

constexpr uint32_t kDefaultRunSeconds = 300;
constexpr uint32_t kMaximumRunSeconds = 3600;
constexpr char kStorageRoot[]         = "windows-all-clusters-kvs";

class AllClustersDeviceInfoProvider final : public DeviceInstanceInfoProvider
{
public:
    CHIP_ERROR GetVendorName(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "Matter SDK"); }

    CHIP_ERROR GetVendorId(uint16_t & vendorId) override
    {
        vendorId = CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductName(char * buffer, size_t bufferSize) override
    {
        return CopyString(buffer, bufferSize, "Windows All Clusters");
    }

    CHIP_ERROR GetProductId(uint16_t & productId) override
    {
        productId = CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPartNumber(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "WIN-ALL-1"); }

    CHIP_ERROR GetProductURL(char * buffer, size_t bufferSize) override
    {
        return CopyString(buffer, bufferSize, "https://project-chip.github.io/connectedhomeip-doc/");
    }

    CHIP_ERROR GetProductLabel(char * buffer, size_t bufferSize) override
    {
        return CopyString(buffer, bufferSize, "Windows All Clusters");
    }

    CHIP_ERROR GetSerialNumber(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "WINALL0001"); }

    CHIP_ERROR GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day) override
    {
        year  = 2026;
        month = 1;
        day   = 1;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersion(uint16_t & hardwareVersion) override
    {
        hardwareVersion = 1;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersionString(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "1.0"); }

    CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan & uniqueIdSpan) override
    {
        static constexpr uint8_t kUniqueId[16] = { 0x57, 0x49, 0x4E, 0x44, 0x4F, 0x57, 0x53, 0x2D,
                                                   0x41, 0x4C, 0x4C, 0x2D, 0x30, 0x30, 0x30, 0x31 };
        return CopySpanToMutableSpan(ByteSpan(kUniqueId), uniqueIdSpan);
    }

private:
    static CHIP_ERROR CopyString(char * buffer, size_t bufferSize, const char * value)
    {
        VerifyOrReturnError(std::strlen(value) < bufferSize, CHIP_ERROR_BUFFER_TOO_SMALL);
        Platform::CopyString(buffer, bufferSize, value);
        return CHIP_NO_ERROR;
    }
};

bool ParseRunSeconds(const char * value, uint32_t & runSeconds)
{
    char * end             = nullptr;
    unsigned long parsed   = std::strtoul(value, &end, 10);
    const bool fullyParsed = end != value && end != nullptr && *end == '\0';
    if (!fullyParsed || parsed == 0 || parsed > kMaximumRunSeconds)
    {
        return false;
    }
    runSeconds = static_cast<uint32_t>(parsed);
    return true;
}

int RunAllClusters(uint32_t runSeconds)
{
    static TestOnlyCommissionableDataProvider commissionableDataProvider;
    static AllClustersDeviceInfoProvider deviceInfoProvider;
    static DeviceInfoProviderImpl deviceInfoStorageProvider;
    SetCommissionableDataProvider(&commissionableDataProvider);
    SetDeviceInstanceInfoProvider(&deviceInfoProvider);
    SetDeviceInfoProvider(&deviceInfoStorageProvider);
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
    app::Clusters::InitializeTlsClientManagement();
    app::Clusters::InitializeTlsCertificateManagement();

    const std::string storageRoot = std::filesystem::absolute(kStorageRoot).string();
    CHIP_ERROR error              = ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(storageRoot.c_str());
    if (error == CHIP_NO_ERROR)
    {
        error = Internal::BLEMgrImpl().ConfigureBle(0, /* aIsCentral = */ false);
    }
    if (error == CHIP_NO_ERROR)
    {
        error = PlatformMgr().InitChipStack();
    }
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Platform initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }

    CommonCaseDeviceServerInitParams initParams;
    error = initParams.InitializeStaticResourcesBeforeServerInit();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Server resource initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().Shutdown();
        return 1;
    }

    initParams.dataModelProvider                  = app::CodegenDataModelProviderInstance(initParams.persistentStorageDelegate);
    initParams.advertiseCommissionableIfNoFabrics = false;
    error                                         = Server::GetInstance().Init(initParams);
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Server initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().Shutdown();
        return 1;
    }

    error = PlatformMgr().StartEventLoopTask();
    if (error == CHIP_NO_ERROR)
    {
        PlatformMgr().LockChipStack();
        error = Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
        PlatformMgr().UnlockChipStack();
    }

    if (error == CHIP_NO_ERROR)
    {
        std::printf("Windows Matter all-clusters app is advertising for %u seconds.\n", runSeconds);
        std::printf("Manual setup code: 34970112332 (PIN 20202021, discriminator 3840)\n");
        std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    }
    else
    {
        std::fprintf(stderr, "Commissioning-window startup failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
    }

    (void) PlatformMgr().StopEventLoopTask();
    app::Clusters::DishwasherMode::Shutdown();
    app::Clusters::LaundryWasherMode::Shutdown();
    app::Clusters::RvcCleanMode::Shutdown();
    app::Clusters::RvcRunMode::Shutdown();
    app::Clusters::MicrowaveOvenMode::Shutdown();
    app::Clusters::RefrigeratorAndTemperatureControlledCabinetMode::Shutdown();
    app::Clusters::OperationalState::Shutdown();
    app::Clusters::RvcOperationalState::Shutdown();
    app::Clusters::OvenMode::Shutdown();
    app::Clusters::OvenCavityOperationalState::Shutdown();
    Server::GetInstance().Shutdown();
    PlatformMgr().Shutdown();
    return error == CHIP_NO_ERROR ? 0 : 1;
}

} // namespace

int main(int argc, char * argv[])
{
    uint32_t runSeconds = kDefaultRunSeconds;
    if (argc > 2 || (argc == 2 && !ParseRunSeconds(argv[1], runSeconds)))
    {
        std::fprintf(stderr, "Usage: %s [run-seconds: 1-3600]\n", argv[0]);
        return 1;
    }

    CHIP_ERROR error = chip::Platform::MemoryInit();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Memory initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }

    const int result = RunAllClusters(runSeconds);
    chip::Platform::MemoryShutdown();
    return result;
}
