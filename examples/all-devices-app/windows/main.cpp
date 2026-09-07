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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <app/DefaultSafeAttributePersistenceProvider.h>
#include <app/DeviceLoadStatusProvider.h>
#include <app/InteractionModelEngine.h>
#include <app/TestEventTriggerDelegate.h>
#include <app/clusters/bindings/BindingManager.h>
#include <app/clusters/bindings/binding-table.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <app_options/DeviceTypeParser.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <data-model-providers/codedriven/CodeDrivenDataModelProvider.h>
#include <device-factory/DeviceFactory.h>
#include <device/api/allocator/DynamicEndpointIdAllocator.h>
#include <device/types/root-node/RootNode.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/DefaultTimerDelegate.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/TestOnlyCommissionableDataProvider.h>
#include <platform/Windows/BLEManagerImpl.h>
#include <platform/Windows/ConfigurationManagerImpl.h>
#include <providers/AllDevicesExampleDeviceInfoProviderImpl.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

namespace {

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;

constexpr uint32_t kMaximumRunSeconds    = 3600;
constexpr char kDefaultStorageRoot[]     = "windows-all-devices-kvs";
constexpr uint16_t kDefaultDiscriminator = 3840;

std::atomic<HANDLE> gStopEvent                 = nullptr;
std::atomic<HANDLE> gShutdownCompleteEvent     = nullptr;
std::atomic<HANDLE> gCloseHandlerCompleteEvent = nullptr;
std::atomic<bool> gCloseHandlerActive = false;

struct AppConfig
{
    std::vector<DeviceTypeParser::Entry> devices;
    std::string storageRoot = kDefaultStorageRoot;
    uint32_t runSeconds     = 0;
    uint16_t discriminator  = kDefaultDiscriminator;
};

class WindowsCommissionableDataProvider final : public TestOnlyCommissionableDataProvider
{
public:
    explicit WindowsCommissionableDataProvider(uint16_t discriminator) : mDiscriminator(discriminator) {}

    CHIP_ERROR GetSetupDiscriminator(uint16_t & setupDiscriminator) override
    {
        setupDiscriminator = mDiscriminator;
        return CHIP_NO_ERROR;
    }

private:
    uint16_t mDiscriminator;
};

class WindowsAllDevicesInfoProvider final : public DeviceInstanceInfoProvider
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
        return CopyString(buffer, bufferSize, "Windows All Devices");
    }

    CHIP_ERROR GetProductId(uint16_t & productId) override
    {
        productId = CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPartNumber(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "WIN-DEV-1"); }
    CHIP_ERROR GetProductURL(char * buffer, size_t bufferSize) override
    {
        return CopyString(buffer, bufferSize, "https://project-chip.github.io/connectedhomeip-doc/");
    }
    CHIP_ERROR GetProductLabel(char * buffer, size_t bufferSize) override
    {
        return CopyString(buffer, bufferSize, "Windows All Devices");
    }
    CHIP_ERROR GetSerialNumber(char * buffer, size_t bufferSize) override { return CopyString(buffer, bufferSize, "WINDEV0001"); }
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
                                                   0x44, 0x45, 0x56, 0x2D, 0x30, 0x30, 0x30, 0x31 };
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

bool ParseUnsigned(const char * value, uint32_t maximum, bool allowZero, uint32_t & result)
{
    char * end           = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || end == nullptr || *end != '\0' || (!allowZero && parsed == 0) || parsed > maximum)
    {
        return false;
    }
    result = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseArguments(int argc, char * argv[], AppConfig & config)
{
    DeviceTypeParser parser;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--device" && index + 1 < argc)
        {
            if (parser.ParseSingleDeviceString(argv[++index]) != CHIP_NO_ERROR)
            {
                return false;
            }
        }
        else if ((argument == "--storage-directory" || argument == "--KVS") && index + 1 < argc)
        {
            config.storageRoot = argv[++index];
        }
        else if (argument == "--discriminator" && index + 1 < argc)
        {
            uint32_t discriminator = 0;
            if (!ParseUnsigned(argv[++index], kMaxDiscriminatorValue, true, discriminator))
            {
                return false;
            }
            config.discriminator = static_cast<uint16_t>(discriminator);
        }
        else if (argument == "--interface-id" && index + 1 < argc)
        {
            if (std::strcmp(argv[++index], "-1") != 0)
            {
                return false;
            }
        }
        else if (argument == "--run-seconds" && index + 1 < argc)
        {
            if (!ParseUnsigned(argv[++index], kMaximumRunSeconds, true, config.runSeconds))
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }

    if (parser.GetDeviceTypeEntries().empty())
    {
        if (parser.ParseSingleDeviceString("on-off-light:1") != CHIP_NO_ERROR)
        {
            return false;
        }
    }

    std::vector<std::string> wildcardDeviceTypes;
    for (const auto & deviceType : DeviceFactory::GetInstance().SupportedDeviceTypes())
    {
        if (deviceType != "aggregator" && deviceType != "bridged-node")
        {
            wildcardDeviceTypes.push_back(deviceType);
        }
    }
    parser.ExpandWildcards(wildcardDeviceTypes);
    config.devices = parser.GetDeviceTypeEntries();
    return DeviceTypeParser::ValidateConfig(config.devices) == CHIP_NO_ERROR;
}

CHIP_ERROR AbsoluteUtf8Path(const std::string & path, std::string & absolutePath)
{
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    VerifyOrReturnError(wideLength > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<wchar_t> widePath(static_cast<size_t>(wideLength));
    VerifyOrReturnError(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, widePath.data(), wideLength) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));

    const std::wstring absoluteWide = std::filesystem::absolute(std::filesystem::path(widePath.data())).wstring();
    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absoluteWide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    VerifyOrReturnError(utf8Length > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<char> utf8Path(static_cast<size_t>(utf8Length));
    VerifyOrReturnError(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absoluteWide.c_str(), -1, utf8Path.data(), utf8Length,
                                           nullptr, nullptr) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));
    absolutePath.assign(utf8Path.data(), static_cast<size_t>(utf8Length - 1));
    return CHIP_NO_ERROR;
}

CHIP_ERROR WideToUtf8(const wchar_t * value, std::string & utf8)
{
    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    VerifyOrReturnError(utf8Length > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<char> buffer(static_cast<size_t>(utf8Length));
    VerifyOrReturnError(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, buffer.data(), utf8Length, nullptr,
                                           nullptr) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));
    utf8.assign(buffer.data(), static_cast<size_t>(utf8Length - 1));
    return CHIP_NO_ERROR;
}

class DynamicDevices
{
public:
    DynamicDevices(CommonCaseDeviceServerInitParams & initParams, TestEventTriggerDelegate & testEventTriggerDelegate,
                   Credentials::GroupDataProvider & groupDataProvider, TimerDelegate & timerDelegate,
                   const std::vector<DeviceTypeParser::Entry> & configurations) :
        mConfigurations(configurations), mDataModelProvider(*initParams.persistentStorageDelegate, mAttributePersistence),
        mRootNode({
            .commissioningWindowManager              = Server::GetInstance().GetCommissioningWindowManager(),
            .configurationManager                    = ConfigurationMgr(),
            .deviceControlServer                     = DeviceControlServer::DeviceControlSvr(),
            .fabricTable                             = Server::GetInstance().GetFabricTable(),
            .accessControl                           = Server::GetInstance().GetAccessControl(),
            .persistentStorage                       = Server::GetInstance().GetPersistentStorage(),
            .failSafeContext                         = Server::GetInstance().GetFailSafeContext(),
            .deviceInstanceInfoProvider              = *GetDeviceInstanceInfoProvider(),
            .platformManager                         = PlatformMgr(),
            .groupDataProvider                       = groupDataProvider,
            .sessionManager                          = Server::GetInstance().GetSecureSessionManager(),
            .dnssdServer                             = DnssdServer::Instance(),
            .deviceLoadStatusProvider                = *InteractionModelEngine::GetInstance(),
            .diagnosticDataProvider                  = GetDiagnosticDataProvider(),
            .testEventTriggerDelegate                = &testEventTriggerDelegate,
            .dacProvider                             = *Credentials::GetDeviceAttestationCredentialsProvider(),
            .eventManagement                         = EventManagement::GetInstance(),
            .timerDelegate                           = timerDelegate,
            .minGuaranteedSubscriptionsPerFabric =
                InteractionModelEngine::GetInstance()->GetMinGuaranteedSubscriptionsPerFabric(),
        })
    {}

    CHIP_ERROR Startup(CommonCaseDeviceServerInitParams & initParams, TestEventTriggerDelegate & testEventTriggerDelegate,
                       Credentials::GroupDataProviderImpl & groupDataProvider, TimerDelegate & timerDelegate)
    {
        ReturnErrorOnFailure(mAttributePersistence.Init(initParams.persistentStorageDelegate));

        DeviceFactory::GetInstance().Init({
            .groupDataProvider        = groupDataProvider,
            .fabricTable              = Server::GetInstance().GetFabricTable(),
            .timerDelegate            = timerDelegate,
            .storageDelegate          = *initParams.persistentStorageDelegate,
            .diagnosticDataProvider   = GetDiagnosticDataProvider(),
            .platformManager          = PlatformMgr(),
            .failSafeContext          = Server::GetInstance().GetFailSafeContext(),
            .bindingTable             = Clusters::Binding::Table::GetInstance(),
            .bindingManager           = Clusters::Binding::Manager::GetInstance(),
            .testEventTriggerDelegate = testEventTriggerDelegate,
        });

        std::set<EndpointId> reservedIds = { kRootEndpointId };
        for (const auto & entry : mConfigurations)
        {
            if (entry.endpoint != kInvalidEndpointId)
            {
                reservedIds.insert(entry.endpoint);
            }
        }

        DynamicEndpointIdAllocator endpointIdAllocator(reservedIds);
        endpointIdAllocator.ForceNext(kRootEndpointId);
        ReturnErrorOnFailure(static_cast<DeviceInterface &>(mRootNode).Register(endpointIdAllocator, mDataModelProvider));

        for (const auto & entry : mConfigurations)
        {
            std::unique_ptr<DeviceInterface> device = DeviceFactory::GetInstance().Create(entry.type, entry.label);
            VerifyOrReturnError(device != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
            if (entry.endpoint != kInvalidEndpointId)
            {
                endpointIdAllocator.ForceNext(entry.endpoint);
            }
            ReturnErrorOnFailure(
                device->Register(endpointIdAllocator, mDataModelProvider, EndpointComposition::WithParent(entry.parentId)));
            std::printf("Registered %s\n", entry.type.c_str());
            mDevices.push_back(std::move(device));
        }
        return CHIP_NO_ERROR;
    }

    void Shutdown()
    {
        for (auto & device : mDevices)
        {
            device->Unregister(mDataModelProvider);
        }
        mDevices.clear();
        mRootNode.Unregister(mDataModelProvider);
    }

    CodeDrivenDataModelProvider & DataModelProvider() { return mDataModelProvider; }

private:
    const std::vector<DeviceTypeParser::Entry> & mConfigurations;
    DefaultAttributePersistenceProvider mAttributePersistence;
    CodeDrivenDataModelProvider mDataModelProvider;
    RootNode mRootNode;
    std::vector<std::unique_ptr<DeviceInterface>> mDevices;
};

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    switch (controlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        if (HANDLE stopEvent = gStopEvent.load(std::memory_order_relaxed))
        {
            SetEvent(stopEvent);
            return TRUE;
        }
        break;
    case CTRL_CLOSE_EVENT:
        if (HANDLE stopEvent = gStopEvent.load(std::memory_order_relaxed))
        {
            gCloseHandlerActive.store(true);
            SetEvent(stopEvent);
            if (HANDLE shutdownCompleteEvent = gShutdownCompleteEvent.load(std::memory_order_relaxed))
            {
                (void) WaitForSingleObject(shutdownCompleteEvent, INFINITE);
            }
            if (HANDLE handlerCompleteEvent = gCloseHandlerCompleteEvent.load(std::memory_order_relaxed))
            {
                SetEvent(handlerCompleteEvent);
            }
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

CHIP_ERROR PrintOnboardingInformation()
{
    PayloadContents payload;
    payload.version = 0;
    payload.rendezvousInformation.SetValue(RendezvousInformationFlag::kBLE);
    ReturnErrorOnFailure(GetCommissionableDataProvider()->GetSetupPasscode(payload.setUpPINCode));

    uint16_t discriminator = 0;
    ReturnErrorOnFailure(GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator));
    payload.discriminator.SetLongValue(discriminator);
    ReturnErrorOnFailure(GetDeviceInstanceInfoProvider()->GetVendorId(payload.vendorID));
    ReturnErrorOnFailure(GetDeviceInstanceInfoProvider()->GetProductId(payload.productID));
    PrintOnboardingCodes(payload);
    return CHIP_NO_ERROR;
}

int Run(const AppConfig & config)
{
    WindowsCommissionableDataProvider commissionableDataProvider(config.discriminator);
    static WindowsAllDevicesInfoProvider deviceInstanceInfoProvider;
    static AllDevicesExampleDeviceInfoProviderImpl deviceInfoProvider;
    static Credentials::GroupDataProviderImpl groupDataProvider;
    static DefaultSafeAttributePersistenceProvider safeAttributePersistenceProvider;
    static DefaultTimerDelegate timerDelegate;
    static SimpleTestEventTriggerDelegate testEventTriggerDelegate;
    static constexpr uint8_t kTestEventTriggerEnableKey[16] = {};
    HANDLE stopEvent                    = nullptr;
    HANDLE shutdownCompleteEvent        = nullptr;
    HANDLE closeHandlerCompleteEvent    = nullptr;
    bool consoleControlHandlerInstalled = false;

    SetDeviceInstanceInfoProvider(&deviceInstanceInfoProvider);
    SetDeviceInfoProvider(&deviceInfoProvider);
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());

    std::string storageRoot;
    CHIP_ERROR error = AbsoluteUtf8Path(config.storageRoot, storageRoot);
    if (error == CHIP_NO_ERROR)
    {
        error = ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(storageRoot.c_str());
    }
    if (error == CHIP_NO_ERROR)
    {
        error = DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(0, false);
    }
    if (error == CHIP_NO_ERROR)
    {
        error = PlatformMgr().InitChipStack();
    }
    if (error == CHIP_NO_ERROR)
    {
        SetCommissionableDataProvider(&commissionableDataProvider);
    }
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Platform initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }

    CommonCaseDeviceServerInitParams initParams;
    error = initParams.InitializeStaticResourcesBeforeServerInit();
    if (error == CHIP_NO_ERROR)
    {
        error = safeAttributePersistenceProvider.Init(initParams.persistentStorageDelegate);
    }
    if (error == CHIP_NO_ERROR)
    {
        SetSafeAttributePersistenceProvider(&safeAttributePersistenceProvider);
        error = testEventTriggerDelegate.Init(ByteSpan(kTestEventTriggerEnableKey));
        initParams.testEventTriggerDelegate = &testEventTriggerDelegate;
    }
    if (error == CHIP_NO_ERROR)
    {
        groupDataProvider.SetStorageDelegate(initParams.persistentStorageDelegate);
        groupDataProvider.SetSessionKeystore(initParams.sessionKeystore);
        error = groupDataProvider.Init();
    }
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Server resource initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().Shutdown();
        return 1;
    }
    Credentials::SetGroupDataProvider(&groupDataProvider);

    DynamicDevices devices(initParams, testEventTriggerDelegate, groupDataProvider, timerDelegate, config.devices);
    error = devices.Startup(initParams, testEventTriggerDelegate, groupDataProvider, timerDelegate);
    if (error == CHIP_NO_ERROR)
    {
        initParams.dataModelProvider = &devices.DataModelProvider();
        initParams.groupDataProvider = &groupDataProvider;
        error                        = Server::GetInstance().Init(initParams);
    }
    if (error == CHIP_NO_ERROR)
    {
        error = PlatformMgr().StartEventLoopTask();
    }
    if (error == CHIP_NO_ERROR)
    {
        PlatformMgr().LockChipStack();
        error = Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow();
        PlatformMgr().UnlockChipStack();
    }
    if (error == CHIP_NO_ERROR)
    {
        error = PrintOnboardingInformation();
    }

    if (error == CHIP_NO_ERROR)
    {
        stopEvent                  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        shutdownCompleteEvent      = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        closeHandlerCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (stopEvent == nullptr)
        {
            error = CHIP_ERROR_WINDOWS(GetLastError());
        }
        else if (shutdownCompleteEvent == nullptr)
        {
            error = CHIP_ERROR_WINDOWS(GetLastError());
        }
        else if (closeHandlerCompleteEvent == nullptr)
        {
            error = CHIP_ERROR_WINDOWS(GetLastError());
        }
        else
        {
            gStopEvent.store(stopEvent, std::memory_order_relaxed);
            gShutdownCompleteEvent.store(shutdownCompleteEvent, std::memory_order_relaxed);
            gCloseHandlerCompleteEvent.store(closeHandlerCompleteEvent, std::memory_order_relaxed);
            if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE))
            {
                error = CHIP_ERROR_WINDOWS(GetLastError());
            }
            else
            {
                consoleControlHandlerInstalled = true;
                ChipLogProgress(DeviceLayer, "===== APP STATUS: Starting event loop =====");
                if (config.runSeconds == 0)
                {
                    std::printf("Windows Matter all-devices app is advertising until stopped.\n");
                }
                else
                {
                    std::printf("Windows Matter all-devices app is advertising for %u seconds.\n", config.runSeconds);
                }

                const DWORD timeout = config.runSeconds == 0 ? INFINITE : config.runSeconds * 1000;
                if (WaitForSingleObject(stopEvent, timeout) == WAIT_FAILED)
                {
                    error = CHIP_ERROR_WINDOWS(GetLastError());
                }
            }
        }
    }
    else
    {
        std::fprintf(stderr, "Application startup failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
    }

    (void) PlatformMgr().StopEventLoopTask();
    devices.Shutdown();
    Server::GetInstance().Shutdown();
    groupDataProvider.Finish();
    Credentials::SetGroupDataProvider(nullptr);
    PlatformMgr().Shutdown();
    if (shutdownCompleteEvent != nullptr)
    {
        SetEvent(shutdownCompleteEvent);
    }
    if (gCloseHandlerActive.load() && closeHandlerCompleteEvent != nullptr)
    {
        (void) WaitForSingleObject(closeHandlerCompleteEvent, INFINITE);
    }
    if (consoleControlHandlerInstalled)
    {
        (void) SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    }
    gStopEvent.store(nullptr, std::memory_order_relaxed);
    gShutdownCompleteEvent.store(nullptr, std::memory_order_relaxed);
    gCloseHandlerCompleteEvent.store(nullptr, std::memory_order_relaxed);
    if (closeHandlerCompleteEvent != nullptr)
    {
        CloseHandle(closeHandlerCompleteEvent);
    }
    if (shutdownCompleteEvent != nullptr)
    {
        CloseHandle(shutdownCompleteEvent);
    }
    if (stopEvent != nullptr)
    {
        CloseHandle(stopEvent);
    }
    return error == CHIP_NO_ERROR ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t * wideArgv[])
{
    std::vector<std::string> arguments(static_cast<size_t>(argc));
    std::vector<char *> argv(static_cast<size_t>(argc));
    for (int index = 0; index < argc; ++index)
    {
        if (WideToUtf8(wideArgv[index], arguments[static_cast<size_t>(index)]) != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Command line contains text that cannot be encoded as UTF-8\n");
            return 1;
        }
        argv[static_cast<size_t>(index)] = arguments[static_cast<size_t>(index)].data();
    }

    AppConfig config;
    if (!ParseArguments(argc, argv.data(), config))
    {
        std::fprintf(stderr,
                     "Usage: %s [--device type[:endpoint][,parent=id]]... [--storage-directory path|--KVS path] "
                     "[--discriminator 0-4095] [--interface-id -1] [--run-seconds 0-3600]\n",
                     argv[0]);
        return 1;
    }

    CHIP_ERROR error = chip::Platform::MemoryInit();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Memory initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }
    const int result = Run(config);
    chip::Platform::MemoryShutdown();
    return result;
}
