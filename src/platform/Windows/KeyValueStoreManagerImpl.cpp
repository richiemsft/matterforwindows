/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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
 *          Native Windows implementation of the Matter key-value store.
 */

#include <platform/KeyValueStoreManager.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemError.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Keep the Windows surface small and predictable for /W4 /WX builds.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shlobj.h> // SHGetKnownFolderPath, FOLDERID_LocalAppData

namespace chip {
namespace DeviceLayer {
namespace PersistedStorage {

namespace {

// ---- On-disk value format -------------------------------------------------
//
// Every value lives in its own file:
//
//   offset 0  : magic      "MKV1"   (4 bytes)
//   offset 4  : version    uint16   (little-endian)
//   offset 6  : flags      uint16   (little-endian, reserved, 0)
//   offset 8  : valueBytes uint64   (little-endian)
//   offset 16 : crc32      uint32   (little-endian, IEEE, over the value bytes)
//   offset 20 : value      valueBytes
//
// The header lets a truncated, mis-sized, or bit-rotted file be detected and
// reported as an explicit integrity error rather than silently returning a
// wrong or empty value.

constexpr std::array<uint8_t, 4> kMagic  = { 'M', 'K', 'V', '1' };
constexpr uint16_t kFormatVersion        = 1;
constexpr size_t kHeaderSize             = 20;

// A key maps to "kv_" + reversible per-byte encoding. The prefix guarantees the
// generated name can never equal a reserved DOS device name (CON, NUL, COM1 …),
// and keeps our files distinguishable from anything else in the directory.
constexpr wchar_t kValuePrefix[]   = L"kv_";
constexpr wchar_t kLockFileName[]  = L".matter-kvs.lock";
constexpr wchar_t kOwnerFileName[] = L".matter-kvs.owner";
constexpr char kOwnerSignature[]   = "Matter Windows KVS v1\r\n";

// Bounds. kMaxKeyLength is chosen so the worst-case encoded file name (3x
// expansion plus the prefix) stays well under the 255-char NTFS component
// limit, and comfortably exceeds PersistentStorageDelegate::kKeyLengthMax (32).
constexpr size_t kMaxKeyLength   = 64;

uint32_t Crc32(const uint8_t * data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
            crc                 = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void PutLE16(uint8_t * p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void PutLE32(uint8_t * p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void PutLE64(uint8_t * p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

uint16_t GetLE16(const uint8_t * p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t GetLE32(const uint8_t * p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetLE64(const uint8_t * p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

// Reversible, collision-free (even under case-insensitive comparison) mapping of
// an arbitrary key to a single path component. Safe bytes are the lowercase
// letters, digits, '-' and '_'; every other byte (including uppercase letters,
// '.', path separators, and '%' itself) becomes %XX with uppercase hex. Because
// no safe byte has a case variant and every escape uses fixed uppercase hex, two
// distinct keys can never fold to the same file name.
bool IsSafeKeyByte(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' || value == '_';
}

bool EncodeKeyToFileName(const char * key, std::wstring & fileName)
{
    const size_t len = std::strlen(key);
    if (len == 0 || len > kMaxKeyLength)
    {
        return false;
    }

    static const char kHex[] = "0123456789ABCDEF";

    fileName.assign(kValuePrefix);
    fileName.reserve(fileName.size() + len);
    for (size_t i = 0; i < len; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (IsSafeKeyByte(c))
        {
            fileName.push_back(static_cast<wchar_t>(c));
        }
        else
        {
            fileName.push_back(L'%');
            fileName.push_back(static_cast<wchar_t>(kHex[(c >> 4) & 0x0F]));
            fileName.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
        }
    }
    return true;
}

CHIP_ERROR Utf8ToWide(const char * utf8, std::wstring & out)
{
    VerifyOrReturnError(utf8 != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    VerifyOrReturnError(needed > 0, CHIP_ERROR_WINDOWS(GetLastError()));
    std::vector<wchar_t> buffer(static_cast<size_t>(needed));
    VerifyOrReturnError(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, buffer.data(), needed) > 0,
                        CHIP_ERROR_WINDOWS(GetLastError()));
    out.assign(buffer.data(), static_cast<size_t>(needed - 1));
    return CHIP_NO_ERROR;
}

CHIP_ERROR ToAbsolutePath(const std::wstring & in, std::wstring & out)
{
    const bool isExtended = in.rfind(L"\\\\?\\", 0) == 0;
    const bool isUnc      = in.rfind(L"\\\\", 0) == 0;
    const bool isDriveAbsolute =
        in.size() >= 3 && ((in[0] >= L'A' && in[0] <= L'Z') || (in[0] >= L'a' && in[0] <= L'z')) && in[1] == L':' &&
        (in[2] == L'\\' || in[2] == L'/');
    VerifyOrReturnError(isExtended || isUnc || isDriveAbsolute, CHIP_ERROR_INVALID_ARGUMENT);

    constexpr DWORD kMaximumExtendedPath = 32768;
    std::vector<wchar_t> buffer(kMaximumExtendedPath);
    const DWORD written = GetFullPathNameW(in.c_str(), kMaximumExtendedPath, buffer.data(), nullptr);
    VerifyOrReturnError(written != 0, CHIP_ERROR_WINDOWS(GetLastError()));
    VerifyOrReturnError(written < kMaximumExtendedPath, CHIP_ERROR_WINDOWS(ERROR_FILENAME_EXCED_RANGE));
    std::wstring absolute(buffer.data(), written);

    while (absolute.size() > 3 && (absolute.back() == L'\\' || absolute.back() == L'/'))
    {
        absolute.pop_back();
    }
    if (absolute.rfind(L"\\\\?\\", 0) == 0)
    {
        out = std::move(absolute);
    }
    else if (absolute.rfind(L"\\\\", 0) == 0)
    {
        out = L"\\\\?\\UNC\\" + absolute.substr(2);
    }
    else
    {
        out = L"\\\\?\\" + absolute;
    }
    return CHIP_NO_ERROR;
}

// Default per-user versioned root: %LOCALAPPDATA%\Matter\KVS\v1, resolved with
// the known-folder API (Unicode) rather than by concatenating an environment
// variable, so it is correct under redirection, impersonation, and non-ASCII
// user profile paths.
CHIP_ERROR DefaultRootPath(std::wstring & out)
{
    PWSTR raw   = nullptr;
    HRESULT hr  = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw);
    if (FAILED(hr))
    {
        if (raw != nullptr)
        {
            CoTaskMemFree(raw);
        }
        return CHIP_ERROR_HRESULT(hr);
    }
    out.assign(raw);
    CoTaskMemFree(raw);
    out += L"\\Matter\\KVS\\v1";
    return ToAbsolutePath(out, out);
}

CHIP_ERROR EnsureDirectory(const std::wstring & path)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
    {
        return CHIP_NO_ERROR;
    }

    DWORD last = GetLastError();
    if (last == ERROR_ALREADY_EXISTS)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        VerifyOrReturnError(attributes != INVALID_FILE_ATTRIBUTES, CHIP_ERROR_WINDOWS(GetLastError()));
        VerifyOrReturnError((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, CHIP_ERROR_INCORRECT_STATE);
        return CHIP_NO_ERROR;
    }
    VerifyOrReturnError(last == ERROR_PATH_NOT_FOUND, CHIP_ERROR_WINDOWS(last));

    const size_t separator = path.find_last_of(L"\\/");
    VerifyOrReturnError(separator != std::wstring::npos && separator > 6, CHIP_ERROR_WINDOWS(last));
    ReturnErrorOnFailure(EnsureDirectory(path.substr(0, separator)));
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_WINDOWS(GetLastError());
}

CHIP_ERROR MakeTempPath(const std::wstring & root, std::wstring & temp)
{
    // Same-directory (hence same-volume) temp name so the final MoveFileEx is an
    // atomic rename. The embedded '.' guarantees it can never equal a value file
    // name (those never contain '.'), while the "kv_" prefix keeps it inside our
    // owned/cleared name space.
    GUID id{};
    const HRESULT hr = CoCreateGuid(&id);
    VerifyOrReturnError(SUCCEEDED(hr), CHIP_ERROR_HRESULT(hr));
    wchar_t guid[40] = {};
    VerifyOrReturnError(StringFromGUID2(id, guid, static_cast<int>(_countof(guid))) != 0, CHIP_ERROR_INTERNAL);

    temp = root + L"\\kv_";
    temp += guid;
    temp += L".tmp";
    return CHIP_NO_ERROR;
}

bool IsHexDigit(wchar_t value)
{
    return (value >= L'0' && value <= L'9') || (value >= L'A' && value <= L'F');
}

bool DecodeOwnedValueFileName(const wchar_t * fileName, std::string * decodedKey)
{
    const std::wstring name(fileName);
    if (name.rfind(kValuePrefix, 0) != 0 || name.size() == 3)
    {
        return false;
    }

    std::string decoded;
    decoded.reserve(name.size() - 3);
    for (size_t index = 3; index < name.size(); ++index)
    {
        if (decoded.size() >= kMaxKeyLength)
        {
            return false;
        }
        const wchar_t value = name[index];
        if ((value >= L'a' && value <= L'z') || (value >= L'0' && value <= L'9') || value == L'-' || value == L'_')
        {
            decoded.push_back(static_cast<char>(value));
            continue;
        }
        if (value != L'%' || index + 2 >= name.size() || !IsHexDigit(name[index + 1]) || !IsHexDigit(name[index + 2]))
        {
            return false;
        }
        const auto hexValue = [](wchar_t digit) -> unsigned char {
            return static_cast<unsigned char>(digit <= L'9' ? digit - L'0' : digit - L'A' + 10);
        };
        const unsigned char decodedByte =
            static_cast<unsigned char>((hexValue(name[index + 1]) << 4) | hexValue(name[index + 2]));
        if (decodedByte == 0 || IsSafeKeyByte(decodedByte))
        {
            return false;
        }
        decoded.push_back(static_cast<char>(decodedByte));
        index += 2;
    }
    if (decodedKey != nullptr)
    {
        *decodedKey = std::move(decoded);
    }
    return true;
}

bool IsOwnedValueFileName(const wchar_t * fileName)
{
    return DecodeOwnedValueFileName(fileName, nullptr);
}

bool IsOwnedTempFileName(const wchar_t * fileName)
{
    GUID ignored{};
    const std::wstring name(fileName);
    constexpr size_t kPrefixLength = 3;
    constexpr size_t kGuidLength   = 38;
    constexpr wchar_t kSuffix[]    = L".tmp";
    return name.size() == kPrefixLength + kGuidLength + _countof(kSuffix) - 1 && name.rfind(kValuePrefix, 0) == 0 &&
        name.compare(name.size() - (_countof(kSuffix) - 1), _countof(kSuffix) - 1, kSuffix) == 0 &&
        SUCCEEDED(CLSIDFromString(name.substr(kPrefixLength, kGuidLength).c_str(), &ignored));
}

CHIP_ERROR ClaimOwnedRoot(const std::wstring & root)
{
    const std::wstring ownerPath = root + L"\\" + kOwnerFileName;
    HANDLE owner = CreateFileW(ownerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (owner != INVALID_HANDLE_VALUE)
    {
        std::array<char, sizeof(kOwnerSignature) - 1> signature{};
        LARGE_INTEGER fileSize{};
        DWORD read = 0;
        const bool valid = GetFileSizeEx(owner, &fileSize) && fileSize.QuadPart == static_cast<LONGLONG>(signature.size()) &&
            ReadFile(owner, signature.data(), static_cast<DWORD>(signature.size()), &read, nullptr) &&
            read == signature.size() && std::memcmp(signature.data(), kOwnerSignature, signature.size()) == 0;
        CloseHandle(owner);
        return valid ? CHIP_NO_ERROR : CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }
    VerifyOrReturnError(GetLastError() == ERROR_FILE_NOT_FOUND, CHIP_ERROR_WINDOWS(GetLastError()));

    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        bool empty = true;
        do
        {
            if (std::wcscmp(findData.cFileName, L".") != 0 && std::wcscmp(findData.cFileName, L"..") != 0)
            {
                empty = false;
                break;
            }
        } while (FindNextFileW(find, &findData));
        const DWORD terminalError = empty ? GetLastError() : ERROR_NO_MORE_FILES;
        FindClose(find);
        VerifyOrReturnError(terminalError == ERROR_NO_MORE_FILES, CHIP_ERROR_WINDOWS(terminalError));
        VerifyOrReturnError(empty, CHIP_ERROR_ACCESS_DENIED);
    }
    else
    {
        VerifyOrReturnError(GetLastError() == ERROR_FILE_NOT_FOUND, CHIP_ERROR_WINDOWS(GetLastError()));
    }

    owner = CreateFileW(ownerPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    VerifyOrReturnError(owner != INVALID_HANDLE_VALUE, CHIP_ERROR_WINDOWS(GetLastError()));
    DWORD written = 0;
    const bool wrote = WriteFile(owner, kOwnerSignature, static_cast<DWORD>(sizeof(kOwnerSignature) - 1), &written, nullptr) &&
        written == sizeof(kOwnerSignature) - 1 && FlushFileBuffers(owner);
    const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
    CloseHandle(owner);
    if (!wrote)
    {
        DeleteFileW(ownerPath.c_str());
        return CHIP_ERROR_WINDOWS(writeError);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR WriteAll(HANDLE handle, const uint8_t * data, size_t size)
{
    size_t written = 0;
    while (written < size)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written, 1u << 20));
        DWORD wrote       = 0;
        VerifyOrReturnError(WriteFile(handle, data + written, chunk, &wrote, nullptr), CHIP_ERROR_WINDOWS(GetLastError()));
        VerifyOrReturnError(wrote == chunk, CHIP_ERROR_WRITE_FAILED);
        written += wrote;
    }
    return CHIP_NO_ERROR;
}

// Durable, atomic replace: write the full value to a same-volume temp file,
// flush its data to disk, then MoveFileEx over the destination with
// REPLACE_EXISTING | WRITE_THROUGH. A reader therefore only ever observes the
// complete previous value or the complete new value, never a torn write, even
// across a crash. (ReplaceFileW is the ACL-preserving alternative; it is not
// needed here because the destination inherits the ACL of our locked-down root.)
CHIP_ERROR WriteValueFile(const std::wstring & root, const std::wstring & finalPath, const uint8_t * value, size_t valueSize)
{
    std::array<uint8_t, kHeaderSize> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    PutLE16(header.data() + 4, kFormatVersion);
    PutLE16(header.data() + 6, 0);
    PutLE64(header.data() + 8, static_cast<uint64_t>(valueSize));
    PutLE32(header.data() + 16, Crc32(value, valueSize));

    std::wstring tempPath;
    ReturnErrorOnFailure(MakeTempPath(root, tempPath));
    HANDLE handle = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    VerifyOrReturnError(handle != INVALID_HANDLE_VALUE, CHIP_ERROR_WINDOWS(GetLastError()));

    CHIP_ERROR err = WriteAll(handle, header.data(), header.size());
    if (err == CHIP_NO_ERROR && valueSize > 0)
    {
        err = WriteAll(handle, value, valueSize);
    }
    if (err == CHIP_NO_ERROR && !FlushFileBuffers(handle))
    {
        err = CHIP_ERROR_WINDOWS(GetLastError());
    }
    CloseHandle(handle);

    if (err != CHIP_NO_ERROR)
    {
        DeleteFileW(tempPath.c_str());
        return err;
    }

    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const CHIP_ERROR moveErr = CHIP_ERROR_WINDOWS(GetLastError());
        DeleteFileW(tempPath.c_str());
        return moveErr;
    }
    return CHIP_NO_ERROR;
}

// Reads and validates the whole value. Missing file maps to the KVS
// not-found contract; any structural problem maps to an explicit integrity
// error. There is deliberately no silent fallback to an empty value.
CHIP_ERROR ReadValueFile(const std::wstring & finalPath, std::vector<uint8_t> & value)
{
    HANDLE handle =
        CreateFileW(finalPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        const DWORD last = GetLastError();
        if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND)
        {
            return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
        }
        return CHIP_ERROR_WINDOWS(last);
    }

    LARGE_INTEGER sizeLi{};
    if (!GetFileSizeEx(handle, &sizeLi))
    {
        const CHIP_ERROR err = CHIP_ERROR_WINDOWS(GetLastError());
        CloseHandle(handle);
        return err;
    }

    const uint64_t fileSize = static_cast<uint64_t>(sizeLi.QuadPart);
    if (fileSize < kHeaderSize || fileSize > kHeaderSize + KeyValueStoreManagerImpl::kMaxValueLength)
    {
        CloseHandle(handle);
        ChipLogError(DeviceLayer, "KVS value file has invalid size");
        return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(fileSize));
    size_t read = 0;
    while (read < raw.size())
    {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(raw.size() - read, 1u << 20));
        DWORD got         = 0;
        if (!ReadFile(handle, raw.data() + read, chunk, &got, nullptr))
        {
            const CHIP_ERROR err = CHIP_ERROR_WINDOWS(GetLastError());
            CloseHandle(handle);
            return err;
        }
        if (got == 0)
        {
            CloseHandle(handle);
            return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
        }
        read += got;
    }
    CloseHandle(handle);

    if (std::memcmp(raw.data(), kMagic.data(), kMagic.size()) != 0 || GetLE16(raw.data() + 4) != kFormatVersion)
    {
        ChipLogError(DeviceLayer, "KVS value file has bad header");
        return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }

    const uint64_t valueLen = GetLE64(raw.data() + 8);
    if (valueLen != fileSize - kHeaderSize)
    {
        ChipLogError(DeviceLayer, "KVS value length mismatch");
        return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }

    const uint8_t * valueStart = raw.data() + kHeaderSize;
    if (Crc32(valueStart, static_cast<size_t>(valueLen)) != GetLE32(raw.data() + 16))
    {
        ChipLogError(DeviceLayer, "KVS value failed CRC check");
        return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }

    value.assign(valueStart, valueStart + valueLen);
    return CHIP_NO_ERROR;
}

} // namespace

KeyValueStoreManagerImpl KeyValueStoreManagerImpl::sInstance;

CHIP_ERROR KeyValueStoreManagerImpl::Init(const char * storageRoot)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(!mInitialized, CHIP_ERROR_INCORRECT_STATE);

    std::wstring root;
    if (storageRoot != nullptr && storageRoot[0] != '\0')
    {
        std::wstring wide;
        ReturnErrorOnFailure(Utf8ToWide(storageRoot, wide));
        ReturnErrorOnFailure(ToAbsolutePath(wide, root));
    }
    else
    {
        ReturnErrorOnFailure(DefaultRootPath(root));
    }

    ReturnErrorOnFailure(EnsureDirectory(root));
    ReturnErrorOnFailure(ClaimOwnedRoot(root));

    // Advisory single-owner lock. Opened without write/delete sharing so a
    // second process holding the same root fails fast rather than racing.
    std::wstring lockPath = root + L"\\" + kLockFileName;
    HANDLE lockHandle     = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (lockHandle == INVALID_HANDLE_VALUE)
    {
        const DWORD last = GetLastError();
        if (last == ERROR_SHARING_VIOLATION)
        {
            ChipLogError(DeviceLayer, "KVS root is already owned by another process");
            return CHIP_ERROR_ACCESS_DENIED;
        }
        return CHIP_ERROR_WINDOWS(last);
    }

    mRootPath    = std::move(root);
    mLockHandle  = lockHandle;
    mInitialized = true;
    return CHIP_NO_ERROR;
}

void KeyValueStoreManagerImpl::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    if (mLockHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(mLockHandle));
        mLockHandle = nullptr;
    }
    mRootPath.clear();
    mInitialized = false;
}

CHIP_ERROR KeyValueStoreManagerImpl::GetValueSize(const char * key, size_t & valueSize)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(key != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    std::wstring fileName;
    VerifyOrReturnError(EncodeKeyToFileName(key, fileName), CHIP_ERROR_INVALID_ARGUMENT);

    std::vector<uint8_t> stored;
    ReturnErrorOnFailure(ReadValueFile(mRootPath + L"\\" + fileName, stored));
    valueSize = stored.size();
    return CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::GetValue(const char * key, std::vector<uint8_t> & value)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(key != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    std::wstring fileName;
    VerifyOrReturnError(EncodeKeyToFileName(key, fileName), CHIP_ERROR_INVALID_ARGUMENT);
    value.clear();
    return ReadValueFile(mRootPath + L"\\" + fileName, value);
}

CHIP_ERROR KeyValueStoreManagerImpl::_Put(const char * key, const void * value, size_t value_size)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(key != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(value != nullptr || value_size == 0, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(value_size <= kMaxValueLength, CHIP_ERROR_INVALID_ARGUMENT);

    std::wstring fileName;
    VerifyOrReturnError(EncodeKeyToFileName(key, fileName), CHIP_ERROR_INVALID_ARGUMENT);

    const std::wstring finalPath = mRootPath + L"\\" + fileName;
    return WriteValueFile(mRootPath, finalPath, static_cast<const uint8_t *>(value), value_size);
}

CHIP_ERROR KeyValueStoreManagerImpl::_Get(const char * key, void * value, size_t value_size, size_t * read_bytes_size,
                                          size_t offset_bytes)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(key != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(value != nullptr || value_size == 0, CHIP_ERROR_INVALID_ARGUMENT);

    std::wstring fileName;
    VerifyOrReturnError(EncodeKeyToFileName(key, fileName), CHIP_ERROR_INVALID_ARGUMENT);

    const std::wstring finalPath = mRootPath + L"\\" + fileName;
    std::vector<uint8_t> stored;
    ReturnErrorOnFailure(ReadValueFile(finalPath, stored));

    VerifyOrReturnError(offset_bytes <= stored.size(), CHIP_ERROR_INVALID_ARGUMENT);

    const size_t remaining = stored.size() - offset_bytes;
    const size_t copySize  = std::min(value_size, remaining);
    if (read_bytes_size != nullptr)
    {
        *read_bytes_size = copySize;
    }
    if (copySize > 0)
    {
        std::memcpy(value, stored.data() + offset_bytes, copySize);
    }
    return (value_size < remaining) ? CHIP_ERROR_BUFFER_TOO_SMALL : CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::_Delete(const char * key)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(key != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    std::wstring fileName;
    VerifyOrReturnError(EncodeKeyToFileName(key, fileName), CHIP_ERROR_INVALID_ARGUMENT);

    const std::wstring finalPath = mRootPath + L"\\" + fileName;
    if (!DeleteFileW(finalPath.c_str()))
    {
        const DWORD last = GetLastError();
        if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND)
        {
            return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
        }
        return CHIP_ERROR_WINDOWS(last);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::ClearAll()
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);

    // Only files this store owns are removed: value files and stray temp files
    // all carry the "kv_" prefix. The ownership lock and any unrelated content
    // are deliberately left untouched.
    const std::wstring pattern = mRootPath + L"\\kv_*";
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD last = GetLastError();
        if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND)
        {
            return CHIP_NO_ERROR;
        }
        return CHIP_ERROR_WINDOWS(last);
    }

    CHIP_ERROR result = CHIP_NO_ERROR;
    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (!IsOwnedValueFileName(findData.cFileName) && !IsOwnedTempFileName(findData.cFileName)))
        {
            continue;
        }
        const std::wstring victim = mRootPath + L"\\" + findData.cFileName;
        if (!DeleteFileW(victim.c_str()) && result == CHIP_NO_ERROR)
        {
            result = CHIP_ERROR_WINDOWS(GetLastError());
        }
    } while (FindNextFileW(find, &findData));

    const DWORD terminalError = GetLastError();
    FindClose(find);
    if (terminalError != ERROR_NO_MORE_FILES && result == CHIP_NO_ERROR)
    {
        result = CHIP_ERROR_WINDOWS(terminalError);
    }
    return result;
}

CHIP_ERROR KeyValueStoreManagerImpl::ClearPrefix(const char * prefix)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(prefix != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    const size_t prefixLength = std::strlen(prefix);
    VerifyOrReturnError(prefixLength > 0 && prefixLength <= kMaxKeyLength, CHIP_ERROR_INVALID_ARGUMENT);

    const std::wstring pattern = mRootPath + L"\\kv_*";
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD last = GetLastError();
        return last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND ? CHIP_NO_ERROR : CHIP_ERROR_WINDOWS(last);
    }

    CHIP_ERROR result = CHIP_NO_ERROR;
    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        std::string key;
        if (!DecodeOwnedValueFileName(findData.cFileName, &key) || key.compare(0, prefixLength, prefix) != 0)
        {
            continue;
        }

        const std::wstring victim = mRootPath + L"\\" + findData.cFileName;
        if (!DeleteFileW(victim.c_str()) && result == CHIP_NO_ERROR)
        {
            result = CHIP_ERROR_WINDOWS(GetLastError());
        }
    } while (FindNextFileW(find, &findData));

    const DWORD terminalError = GetLastError();
    FindClose(find);
    if (terminalError != ERROR_NO_MORE_FILES && result == CHIP_NO_ERROR)
    {
        result = CHIP_ERROR_WINDOWS(terminalError);
    }
    return result;
}

CHIP_ERROR KeyValueStoreManagerImpl::ClearAllExceptPrefix(const char * preservedPrefix)
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    VerifyOrReturnError(mInitialized, CHIP_ERROR_UNINITIALIZED);
    VerifyOrReturnError(preservedPrefix != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    const size_t prefixLength = std::strlen(preservedPrefix);
    VerifyOrReturnError(prefixLength > 0 && prefixLength <= kMaxKeyLength, CHIP_ERROR_INVALID_ARGUMENT);

    const std::wstring pattern = mRootPath + L"\\kv_*";
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD last = GetLastError();
        return last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND ? CHIP_NO_ERROR : CHIP_ERROR_WINDOWS(last);
    }

    CHIP_ERROR result = CHIP_NO_ERROR;
    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            continue;
        }

        std::string key;
        const bool isValue = DecodeOwnedValueFileName(findData.cFileName, &key);
        if ((!isValue && !IsOwnedTempFileName(findData.cFileName)) ||
            (isValue && key.compare(0, prefixLength, preservedPrefix) == 0))
        {
            continue;
        }

        const std::wstring victim = mRootPath + L"\\" + findData.cFileName;
        if (!DeleteFileW(victim.c_str()) && result == CHIP_NO_ERROR)
        {
            result = CHIP_ERROR_WINDOWS(GetLastError());
        }
    } while (FindNextFileW(find, &findData));

    const DWORD terminalError = GetLastError();
    FindClose(find);
    if (terminalError != ERROR_NO_MORE_FILES && result == CHIP_NO_ERROR)
    {
        result = CHIP_ERROR_WINDOWS(terminalError);
    }
    return result;
}

} // namespace PersistedStorage
} // namespace DeviceLayer
} // namespace chip
