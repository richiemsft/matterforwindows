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

// Runtime smoke for the Phase 3 native Windows key-value store. It proves the
// KVS contract end to end against a real, isolated on-disk store: binary
// put/get, offset+size reads, overwrite, delete / not-found, persistence across
// a Shutdown()+Init() restart, invalid-key and traversal/reserved-name safety,
// and factory-reset (ClearAll) scoped strictly to owned files. Returns 0 on
// success, 1 on the first failure. The store lives in a unique directory under
// the user's temporary directory and is fully removed on the way out.

#include <platform/KeyValueStoreManager.h>

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

using namespace chip;
using namespace chip::DeviceLayer::PersistedStorage;

namespace {

int gStep = 0;

#define CHECK(cond)                                                                                                                \
    do                                                                                                                             \
    {                                                                                                                             \
        ++gStep;                                                                                                                   \
        if (!(cond))                                                                                                               \
        {                                                                                                                          \
            std::printf("KVS smoke failed at step %d: %s\n", gStep, #cond);                                                        \
            return false;                                                                                                          \
        }                                                                                                                          \
    } while (0)

bool RemoveTree(const std::wstring & dir)
{
    bool removed = true;
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = findData.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring full = dir + L"\\" + name;
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            {
                removed = RemoveTree(full) && removed;
            }
            else if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                removed = RemoveDirectoryW(full.c_str()) && removed;
            }
            else
            {
                removed = SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL) && removed;
                removed = DeleteFileW(full.c_str()) && removed;
            }
        } while (FindNextFileW(find, &findData));
        removed = GetLastError() == ERROR_NO_MORE_FILES && removed;
        FindClose(find);
    }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return false;
    }
    return RemoveDirectoryW(dir.c_str()) && removed;
}

bool PathExists(const std::wstring & path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

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

bool RunScenarios(const std::string & rootUtf8, const std::wstring & rootWide, const std::wstring & escapedSibling)
{
    CHECK(KeyValueStoreMgrImpl().Init("relative-kvs-root") == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(rootWide.size() > MAX_PATH);

    // ---- Init on an isolated, freshly created root.
    CHECK(KeyValueStoreMgrImpl().Init(rootUtf8.c_str()) == CHIP_NO_ERROR);
    CHECK(KeyValueStoreMgrImpl().Init(rootUtf8.c_str()) == CHIP_ERROR_INCORRECT_STATE);
    CHECK(PathExists(rootWide + L"\\.matter-kvs.owner"));

    // ---- Binary put/get round-trip (embedded NULs and high bytes).
    const uint8_t binary[] = { 0x00, 0x01, 0xFF, 0x00, 0x7F, 0x80, 0xAA, 0x55, 0x00, 0x10 };
    CHECK(KeyValueStoreMgr().Put("bin", binary, sizeof(binary)) == CHIP_NO_ERROR);

    uint8_t readBuf[sizeof(binary)] = {};
    size_t readSize                 = 0;
    CHECK(KeyValueStoreMgr().Get("bin", readBuf, sizeof(readBuf), &readSize) == CHIP_NO_ERROR);
    CHECK(readSize == sizeof(binary));
    CHECK(std::memcmp(readBuf, binary, sizeof(binary)) == 0);

    // ---- Offset + size behavior.
    uint8_t partialBuffer[4] = {};
    readSize                 = 0;
    CHECK(KeyValueStoreMgr().Get("bin", partialBuffer, sizeof(partialBuffer), &readSize, 0) == CHIP_ERROR_BUFFER_TOO_SMALL);
    CHECK(readSize == sizeof(partialBuffer));
    CHECK(std::memcmp(partialBuffer, binary, sizeof(partialBuffer)) == 0);

    std::memset(partialBuffer, 0, sizeof(partialBuffer));
    readSize = 0;
    CHECK(KeyValueStoreMgr().Get("bin", partialBuffer, sizeof(partialBuffer), &readSize, 6) == CHIP_NO_ERROR);
    CHECK(readSize == 4);
    CHECK(std::memcmp(partialBuffer, binary + 6, 4) == 0);

    readSize = 0;
    CHECK(KeyValueStoreMgr().Get("bin", partialBuffer, sizeof(partialBuffer), &readSize, sizeof(binary)) == CHIP_NO_ERROR);
    CHECK(readSize == 0);

    CHECK(KeyValueStoreMgr().Get("bin", partialBuffer, sizeof(partialBuffer), &readSize, sizeof(binary) + 1) ==
          CHIP_ERROR_INVALID_ARGUMENT);

    // ---- Overwrite with a different-length value.
    const uint8_t replacement[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };
    CHECK(KeyValueStoreMgr().Put("bin", replacement, sizeof(replacement)) == CHIP_NO_ERROR);
    uint8_t overwriteBuf[16] = {};
    readSize                 = 0;
    CHECK(KeyValueStoreMgr().Get("bin", overwriteBuf, sizeof(overwriteBuf), &readSize) == CHIP_NO_ERROR);
    CHECK(readSize == sizeof(replacement));
    CHECK(std::memcmp(overwriteBuf, replacement, sizeof(replacement)) == 0);

    // ---- Delete / not-found.
    CHECK(KeyValueStoreMgr().Delete("bin") == CHIP_NO_ERROR);
    CHECK(KeyValueStoreMgr().Get("bin", overwriteBuf, sizeof(overwriteBuf), &readSize) ==
          CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    CHECK(KeyValueStoreMgr().Delete("bin") == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    CHECK(KeyValueStoreMgr().Get("never", overwriteBuf, sizeof(overwriteBuf), &readSize) ==
          CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);

    // ---- Invalid-key rejection.
    uint8_t byte = 0x7;
    CHECK(KeyValueStoreMgr().Put("", &byte, 1) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(KeyValueStoreMgr().Get("", &byte, 1) == CHIP_ERROR_INVALID_ARGUMENT);
    CHECK(KeyValueStoreMgr().Delete("") == CHIP_ERROR_INVALID_ARGUMENT);
    const std::string tooLong(200, 'x');
    CHECK(KeyValueStoreMgr().Put(tooLong.c_str(), &byte, 1) == CHIP_ERROR_INVALID_ARGUMENT);

    // ---- Traversal / reserved-name / separator safety: these are valid key
    // strings and must be stored safely inside the root, never escaping it and
    // never colliding with a reserved DOS device name.
    const char * trickyKeys[] = { "con", "NUL", "..", "../escape", "a/b\\c", "COM1", "Mixed.Case:Key*?" };
    for (const char * trickyKey : trickyKeys)
    {
        const uint8_t marker[] = { 0x42 };
        CHECK(KeyValueStoreMgr().Put(trickyKey, marker, sizeof(marker)) == CHIP_NO_ERROR);
        uint8_t back = 0;
        readSize     = 0;
        CHECK(KeyValueStoreMgr().Get(trickyKey, &back, sizeof(back), &readSize) == CHIP_NO_ERROR);
        CHECK(readSize == 1 && back == 0x42);
    }
    // Nothing escaped the root (e.g. no sibling "escape" file next to the root).
    CHECK(!PathExists(escapedSibling));

    // ---- Persistence across a restart (Shutdown + Init on the same root).
    const uint8_t persisted[] = { 'p', 'e', 'r', 's', 'i', 's', 't' };
    CHECK(KeyValueStoreMgr().Put("survivor", persisted, sizeof(persisted)) == CHIP_NO_ERROR);
    KeyValueStoreMgrImpl().Shutdown();
    CHECK(KeyValueStoreMgrImpl().Init(rootUtf8.c_str()) == CHIP_NO_ERROR);
    uint8_t survivorBuf[16] = {};
    readSize                = 0;
    CHECK(KeyValueStoreMgr().Get("survivor", survivorBuf, sizeof(survivorBuf), &readSize) == CHIP_NO_ERROR);
    CHECK(readSize == sizeof(persisted));
    CHECK(std::memcmp(survivorBuf, persisted, sizeof(persisted)) == 0);

    // ---- Factory reset (ClearAll) is scoped only to owned files. Drop a
    // foreign file into the root and confirm ClearAll leaves it untouched while
    // wiping every stored key.
    const std::wstring foreign = rootWide + L"\\foreign-do-not-delete.txt";
    HANDLE foreignHandle = CreateFileW(foreign.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(foreignHandle != INVALID_HANDLE_VALUE);
    CloseHandle(foreignHandle);
    const std::wstring malformedLookalike = rootWide + L"\\kv_%61";
    foreignHandle = CreateFileW(malformedLookalike.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(foreignHandle != INVALID_HANDLE_VALUE);
    CloseHandle(foreignHandle);
    const std::wstring overlongLookalike = rootWide + L"\\kv_" + std::wstring(65, L'x');
    foreignHandle = CreateFileW(overlongLookalike.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(foreignHandle != INVALID_HANDLE_VALUE);
    CloseHandle(foreignHandle);

    CHECK(KeyValueStoreMgrImpl().ClearAll() == CHIP_NO_ERROR);
    CHECK(KeyValueStoreMgr().Get("survivor", survivorBuf, sizeof(survivorBuf), &readSize) ==
          CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    CHECK(KeyValueStoreMgr().Get("con", &byte, sizeof(byte), &readSize) == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND);
    CHECK(PathExists(foreign));
    CHECK(PathExists(malformedLookalike));
    CHECK(PathExists(overlongLookalike));
    CHECK(PathExists(rootWide + L"\\.matter-kvs.owner"));

    // ---- Operations after Shutdown are rejected, not silently defaulted.
    KeyValueStoreMgrImpl().Shutdown();
    CHECK(KeyValueStoreMgr().Get("survivor", survivorBuf, sizeof(survivorBuf), &readSize) == CHIP_ERROR_UNINITIALIZED);

    return true;
}

bool RejectsNonEmptyUnownedRoot(const std::wstring & root)
{
    CHECK(CreateDirectoryW(root.c_str(), nullptr));
    const std::wstring foreign = root + L"\\existing.txt";
    HANDLE file = CreateFileW(foreign.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);

    std::string rootUtf8;
    CHECK(WideToUtf8(root, rootUtf8));
    CHECK(KeyValueStoreMgrImpl().Init(rootUtf8.c_str()) == CHIP_ERROR_ACCESS_DENIED);
    return true;
}

} // namespace

int main()
{
    wchar_t temporaryDirectory[MAX_PATH] = {};
    if (GetTempPathW(_countof(temporaryDirectory), temporaryDirectory) == 0)
    {
        std::printf("KVS smoke could not resolve the temporary directory\n");
        return 1;
    }

    GUID id{};
    wchar_t guid[40] = {};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, guid, static_cast<int>(_countof(guid))) == 0)
    {
        std::printf("KVS smoke could not generate an isolated root\n");
        return 1;
    }
    const std::wstring base = std::wstring(temporaryDirectory) + L"matter-kvs-smoke-" + guid;
    if (!CreateDirectoryW(base.c_str(), nullptr))
    {
        std::printf("KVS smoke could not create its isolated root\n");
        return 1;
    }

    const std::wstring segment(90, L'x');
    const std::wstring root = base + L"\\" + segment + L"\\" + segment + L"\\" + segment;
    const std::wstring extendedBase = L"\\\\?\\" + base;
    const std::wstring rootWide     = L"\\\\?\\" + root;
    std::string rootUtf8;
    if (!WideToUtf8(root, rootUtf8))
    {
        std::printf("KVS smoke could not encode the long root path\n");
        if (!RemoveTree(extendedBase))
        {
            std::printf("KVS smoke could not remove its isolated root\n");
        }
        return 1;
    }

    const bool scenariosPassed = RunScenarios(rootUtf8, rootWide, extendedBase + L"\\escape");
    const bool ownershipPassed = scenariosPassed && RejectsNonEmptyUnownedRoot(extendedBase + L"\\occupied");

    // Release the lock even when a CHECK returns early, then remove only our
    // isolated directory tree without traversing reparse points.
    KeyValueStoreMgrImpl().Shutdown();
    const bool cleanupPassed = RemoveTree(extendedBase);

    if (!ownershipPassed || !cleanupPassed)
    {
        if (!cleanupPassed)
        {
            std::printf("KVS smoke could not remove its isolated root\n");
        }
        return 1;
    }
    std::printf("KVS smoke passed (%d checks)\n", gStep);
    return 0;
}
