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

#include <platform/CHIPDeviceError.h>
#include <platform/Windows/WindowsConfig.h>

#include <cstdint>
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
#include <windows.h>
#include <objbase.h>

using chip::DeviceLayer::Internal::WindowsConfig;

namespace {

int gStep = 0;

#define CHECK(condition)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        ++gStep;                                                                                                                   \
        if (!(condition))                                                                                                          \
        {                                                                                                                          \
            std::printf("Windows configuration smoke failed at step %d: %s\n", gStep, #condition);                                \
            return false;                                                                                                          \
        }                                                                                                                          \
    } while (0)

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
    CHECK(WindowsConfig::Init(root.c_str()) == CHIP_NO_ERROR);
    CHECK(WindowsConfig::EnsureNamespace(WindowsConfig::kConfigNamespace_ChipFactory) == CHIP_NO_ERROR);
    CHECK(WindowsConfig::EnsureNamespace("unknown") == CHIP_ERROR_INVALID_ARGUMENT);

    CHECK(WindowsConfig::WriteConfigValue(WindowsConfig::kConfigKey_FailSafeArmed, true) == CHIP_NO_ERROR);
    bool boolValue = false;
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_FailSafeArmed, boolValue) == CHIP_NO_ERROR);
    CHECK(boolValue);

    CHECK(WindowsConfig::WriteConfigValue(WindowsConfig::kConfigKey_HardwareVersion, static_cast<uint16_t>(0x1234)) ==
          CHIP_NO_ERROR);
    uint16_t value16 = 0;
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_HardwareVersion, value16) == CHIP_NO_ERROR);
    CHECK(value16 == 0x1234);

    CHECK(WindowsConfig::WriteConfigValue(WindowsConfig::kCounterKey_RebootCount, UINT32_C(0x89ABCDEF)) == CHIP_NO_ERROR);
    uint32_t value32 = 0;
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kCounterKey_RebootCount, value32) == CHIP_NO_ERROR);
    CHECK(value32 == UINT32_C(0x89ABCDEF));

    CHECK(WindowsConfig::WriteConfigValue(WindowsConfig::kConfigKey_MfrDeviceId, UINT64_C(0x0123456789ABCDEF)) ==
          CHIP_NO_ERROR);
    CHECK(WindowsConfig::WriteConfigValue(WindowsConfig::kConfigKey_SetupDiscriminator, UINT32_C(3840)) == CHIP_NO_ERROR);
    uint64_t value64 = 0;
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_MfrDeviceId, value64) == CHIP_NO_ERROR);
    CHECK(value64 == UINT64_C(0x0123456789ABCDEF));
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_MfrDeviceId, value32) ==
          CHIP_ERROR_INTEGRITY_CHECK_FAILED);

    constexpr char kText[] = "native Windows";
    CHECK(WindowsConfig::WriteConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, kText) == CHIP_NO_ERROR);
    size_t length = 0;
    CHECK(WindowsConfig::ReadConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, nullptr, 0, length) == CHIP_NO_ERROR);
    CHECK(length == std::strlen(kText));
    char shortText[4] = {};
    CHECK(WindowsConfig::ReadConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, shortText, sizeof(shortText), length) ==
          CHIP_ERROR_BUFFER_TOO_SMALL);
    char text[32] = {};
    CHECK(WindowsConfig::ReadConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, text, sizeof(text), length) ==
          CHIP_NO_ERROR);
    CHECK(length == std::strlen(kText) && std::strcmp(text, kText) == 0);
    const char embeddedNull[] = { 'a', '\0', 'b' };
    CHECK(WindowsConfig::WriteConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, embeddedNull,
                                             sizeof(embeddedNull)) == CHIP_ERROR_INVALID_ARGUMENT);

    const uint8_t binary[] = { 0x00, 0xFF, 0x80, 0x01, 0x02 };
    CHECK(WindowsConfig::WriteConfigValueBin(WindowsConfig::kConfigKey_MfrDeviceCert, binary, sizeof(binary)) == CHIP_NO_ERROR);
    CHECK(WindowsConfig::ReadConfigValueBin(WindowsConfig::kConfigKey_MfrDeviceCert, nullptr, 0, length) == CHIP_NO_ERROR);
    CHECK(length == sizeof(binary));
    uint8_t binaryRead[sizeof(binary)] = {};
    CHECK(WindowsConfig::ReadConfigValueBin(WindowsConfig::kConfigKey_MfrDeviceCert, binaryRead, sizeof(binaryRead), length) ==
          CHIP_NO_ERROR);
    CHECK(length == sizeof(binary) && std::memcmp(binary, binaryRead, sizeof(binary)) == 0);
    CHECK(WindowsConfig::WriteConfigValueBin(WindowsConfig::kConfigKey_MfrDeviceCert, binary, SIZE_MAX) ==
          CHIP_ERROR_INVALID_ARGUMENT);

    CHECK(WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_MfrDeviceId));
    CHECK(!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_ServiceId));
    CHECK(WindowsConfig::ClearConfigValue(WindowsConfig::kConfigKey_ServiceId) == CHIP_NO_ERROR);

    WindowsConfig::Shutdown();
    CHECK(WindowsConfig::Init(root.c_str()) == CHIP_NO_ERROR);
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_MfrDeviceId, value64) == CHIP_NO_ERROR);
    CHECK(value64 == UINT64_C(0x0123456789ABCDEF));

    CHECK(WindowsConfig::FactoryResetConfig() == CHIP_NO_ERROR);
    CHECK(WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_MfrDeviceId));
    CHECK(WindowsConfig::ReadConfigValue(WindowsConfig::kConfigKey_SetupDiscriminator, value32) == CHIP_NO_ERROR);
    CHECK(value32 == UINT32_C(3840));
    CHECK(!WindowsConfig::ConfigValueExists(WindowsConfig::kConfigKey_FailSafeArmed));
    CHECK(WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_RebootCount));
    CHECK(WindowsConfig::FactoryResetCounters() == CHIP_NO_ERROR);
    CHECK(!WindowsConfig::ConfigValueExists(WindowsConfig::kCounterKey_RebootCount));

    CHECK(WindowsConfig::WriteConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, "") == CHIP_NO_ERROR);
    CHECK(WindowsConfig::ReadConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, text, sizeof(text), length) ==
          CHIP_NO_ERROR);
    CHECK(length == 0 && text[0] == '\0');
    CHECK(WindowsConfig::WriteConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, nullptr) == CHIP_NO_ERROR);
    CHECK(WindowsConfig::ReadConfigValueStr(WindowsConfig::kConfigKey_PairedAccountId, text, sizeof(text), length) ==
          CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND);

    WindowsConfig::Shutdown();
    return true;
}

} // namespace

int main()
{
    wchar_t temporaryDirectory[MAX_PATH] = {};
    GUID id{};
    wchar_t guid[40] = {};
    if (GetTempPathW(_countof(temporaryDirectory), temporaryDirectory) == 0 || FAILED(CoCreateGuid(&id)) ||
        StringFromGUID2(id, guid, static_cast<int>(_countof(guid))) == 0)
    {
        std::printf("Windows configuration smoke could not create an isolated root\n");
        return 1;
    }

    const std::wstring rootWide = std::wstring(temporaryDirectory) + L"matter-config-smoke-" + guid;
    std::string root;
    if (!WideToUtf8(rootWide, root))
    {
        return 1;
    }

    const bool scenariosPassed = RunScenarios(root);
    WindowsConfig::Shutdown();
    const bool cleanupPassed = RemoveTree(rootWide);
    if (!scenariosPassed || !cleanupPassed)
    {
        return 1;
    }
    std::printf("Windows configuration smoke passed (%d checks)\n", gStep);
    return 0;
}
