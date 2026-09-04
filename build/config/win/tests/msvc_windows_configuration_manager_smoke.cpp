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

#include <lib/support/CHIPMem.h>
#include <platform/ConfigurationManager.h>
#include <platform/ConnectivityManager.h>
#include <platform/PlatformManager.h>
#include <platform/Windows/WindowsConfig.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>

using namespace chip;
using namespace chip::DeviceLayer;
using chip::DeviceLayer::Internal::WindowsConfig;

namespace {

int gChecks = 0;

#define CHECK(condition)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        ++gChecks;                                                                                                                 \
        if (!(condition))                                                                                                          \
        {                                                                                                                          \
            std::printf("Windows configuration manager smoke failed at check %d: %s\n", gChecks, #condition);                     \
            return false;                                                                                                          \
        }                                                                                                                          \
    } while (0)

class NetworkInfoDelegate final : public ConnectivityManagerDelegate
{
public:
    void OnNetworkInfoChanged() override
    {
        mCallbackThread.store(GetCurrentThreadId(), std::memory_order_release);
        mCallbackCount.fetch_add(1, std::memory_order_acq_rel);
    }

    std::atomic<unsigned int> mCallbackCount{ 0 };
    std::atomic<DWORD> mCallbackThread{ 0 };
};

bool WideToUtf8(const std::wstring & wide, std::string & utf8)
{
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        return false;
    }
    std::vector<char> buffer(static_cast<size_t>(needed));
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buffer.data(), needed, nullptr, nullptr) <= 0)
    {
        return false;
    }
    utf8.assign(buffer.data(), static_cast<size_t>(needed - 1));
    return true;
}

bool RemoveTree(const std::wstring & directory)
{
    bool removed = true;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring path = directory + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            {
                removed = RemoveTree(path) && removed;
            }
            else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                removed = RemoveDirectoryW(path.c_str()) && removed;
            }
            else
            {
                removed = SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) && removed;
                removed = DeleteFileW(path.c_str()) && removed;
            }
        } while (FindNextFileW(find, &data));
        removed = GetLastError() == ERROR_NO_MORE_FILES && removed;
        FindClose(find);
    }
    return RemoveDirectoryW(directory.c_str()) && removed;
}

bool RunScenarios(const std::string & root)
{
    ConfigurationManagerImpl & manager = ConfigurationManagerImpl::GetDefaultInstance();
    CHECK(&ConfigurationMgr() == &manager);
    CHECK(manager.ConfigureStorageRoot(root.c_str()) == CHIP_NO_ERROR);
    CHECK(PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    CHECK(manager.ConfigureStorageRoot(root.c_str()) == CHIP_ERROR_INCORRECT_STATE);

    uint32_t value = 0;
    CHECK(manager.GetRebootCount(value) == CHIP_NO_ERROR && value == 1);
    CHECK(manager.GetTotalOperationalHours(value) == CHIP_NO_ERROR && value == 0);
    CHECK(manager.GetBootReason(value) == CHIP_NO_ERROR && value == 0);
    CHECK(manager.GetConfigurationVersion(value) == CHIP_NO_ERROR && value == 1);

    char uniqueId[ConfigurationManager::kMaxUniqueIDLength + 1] = {};
    CHECK(manager.GetUniqueId(uniqueId, sizeof(uniqueId)) == CHIP_NO_ERROR);
    CHECK(std::strlen(uniqueId) == 16);
    const std::string firstUniqueId(uniqueId);

    uint16_t id = 0;
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_VendorId, id) == CHIP_NO_ERROR);
    CHECK(id == CHIP_DEVICE_CONFIG_DEVICE_VENDOR_ID);
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_ProductId, id) == CHIP_NO_ERROR);
    CHECK(id == CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_ID);

    constexpr Platform::PersistedStorage::Key kCounter = "configuration-manager-smoke";
    CHECK(Platform::PersistedStorage::Write(kCounter, 0x12345678) == CHIP_NO_ERROR);
    CHECK(Platform::PersistedStorage::Read(kCounter, value) == CHIP_NO_ERROR && value == 0x12345678);
    constexpr uint8_t kFabricRecord[] = { 1, 2, 3 };
    CHECK(PersistedStorage::KeyValueStoreMgr().Put("f/1/n", kFabricRecord, sizeof(kFabricRecord)) == CHIP_NO_ERROR);

    uint8_t macStorage[ConfigurationManager::kPrimaryMACAddressLength] = {};
    MutableByteSpan mac(macStorage);
    const CHIP_ERROR macError = manager.GetPrimaryMACAddress(mac);
    CHECK(macError == CHIP_NO_ERROR || macError == CHIP_ERROR_NOT_FOUND);
    if (macError == CHIP_NO_ERROR)
    {
        CHECK(mac.size() == ConfigurationManager::kEthernetMACAddressLength);
        CHECK(std::any_of(mac.begin(), mac.end(), [](uint8_t byte) { return byte != 0; }));
    }

    PlatformMgr().Shutdown();
    CHECK(PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    CHECK(manager.GetRebootCount(value) == CHIP_NO_ERROR && value == 2);
    std::memset(uniqueId, 0, sizeof(uniqueId));
    CHECK(manager.GetUniqueId(uniqueId, sizeof(uniqueId)) == CHIP_NO_ERROR);
    CHECK(firstUniqueId == uniqueId);
    CHECK(Platform::PersistedStorage::Read(kCounter, value) == CHIP_NO_ERROR && value == 0x12345678);

    NetworkInfoDelegate networkInfoDelegate;
    ConnectivityMgr().SetDelegate(&networkInfoDelegate);
    const DWORD mainThread = GetCurrentThreadId();
    CHECK(PlatformMgr().StartEventLoopTask() == CHIP_NO_ERROR);
    ConnectivityMgrImpl().NotifyNetworkChange();
    for (unsigned int attempt = 0; attempt < 100 && networkInfoDelegate.mCallbackCount.load(std::memory_order_acquire) == 0;
         ++attempt)
    {
        Sleep(10);
    }
    CHECK(networkInfoDelegate.mCallbackCount.load(std::memory_order_acquire) >= 1);
    CHECK(networkInfoDelegate.mCallbackThread.load(std::memory_order_acquire) != mainThread);

    ConfigurationMgr().InitiateFactoryReset();
    for (unsigned int attempt = 0;
         attempt < 100 && WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_RebootCount); ++attempt)
    {
        Sleep(10);
    }
    CHECK(!WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_RebootCount));
    CHECK(!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_UniqueId));
    CHECK(WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_VendorId));
    CHECK(Platform::PersistedStorage::Read(kCounter, value) == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    size_t fabricRecordSize = 0;
    CHECK(PersistedStorage::KeyValueStoreMgr().Get("f/1/n", nullptr, 0, &fabricRecordSize) ==
          CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    CHECK(PlatformMgr().StopEventLoopTask() == CHIP_NO_ERROR);
    ConnectivityMgr().SetDelegate(nullptr);
    PlatformMgr().Shutdown();

    return true;
}

} // namespace

int main()
{
    if (Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        return 1;
    }

    wchar_t temporaryDirectory[MAX_PATH] = {};
    GUID id{};
    wchar_t guid[40] = {};
    if (GetTempPathW(_countof(temporaryDirectory), temporaryDirectory) == 0 || FAILED(CoCreateGuid(&id)) ||
        StringFromGUID2(id, guid, static_cast<int>(_countof(guid))) == 0)
    {
        Platform::MemoryShutdown();
        return 1;
    }

    const std::wstring rootWide = std::wstring(temporaryDirectory) + L"matter-config-manager-smoke-" + guid;
    std::string root;
    const bool converted = WideToUtf8(rootWide, root);
    const bool scenarios = converted && RunScenarios(root);
    const bool cleanup = converted && RemoveTree(rootWide);
    Platform::MemoryShutdown();

    if (!scenarios || !cleanup)
    {
        return 1;
    }
    std::printf("Windows configuration manager smoke passed (%d checks)\n", gChecks);
    return 0;
}
