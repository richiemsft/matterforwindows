/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2016-2017 Nest Labs, Inc.
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
 *      This file contains free functions for mapping OS and LwIP
 *      stack-specific errors into CHIP System Layer-specific errors
 *      and for converting those mapped errors into descriptive
 *      error strings.
 */

// Include module header
#include <system/SystemError.h>

#include <lib/core/ErrorStr.h>
#if !defined(_WIN32)
#include <lib/support/CHIPMemString.h>
#endif
#include <lib/support/DLLUtil.h>

#include <lib/core/CHIPConfig.h>

// Include local headers
#if CHIP_SYSTEM_CONFIG_USE_LWIP
#include <lwip/err.h>
#endif // CHIP_SYSTEM_CONFIG_USE_LWIP

#include <limits>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace chip {
namespace System {

namespace Internal {
/**
 * This implements a mapping function for CHIP System Layer errors that allows mapping integers in the number space of the
 * underlying POSIX network and OS stack errors into a platform- or system-specific range. Error codes beyond those currently
 * defined by POSIX or the ISO C/C++ standards are mapped similar to the standard ones.
 *
 *  @param[in] aError  The POSIX network or OS error to map.
 *
 *  @return The mapped POSIX network or OS error.
 */
#if CHIP_CONFIG_ERROR_SOURCE && CHIP_CONFIG_ERROR_STD_SOURCE_LOCATION
DLL_EXPORT CHIP_ERROR MapErrorPOSIX(int aError, std::source_location location)
{
    return (aError == 0 ? CHIP_NO_ERROR : CHIP_ERROR(ChipError::Range::kPOSIX, aError, location));
}
#elif CHIP_CONFIG_ERROR_SOURCE
DLL_EXPORT CHIP_ERROR MapErrorPOSIX(int aError, const char * file, unsigned int line)
{
    return (aError == 0 ? CHIP_NO_ERROR : CHIP_ERROR(ChipError::Range::kPOSIX, aError, file, line));
}
#else
DLL_EXPORT CHIP_ERROR MapErrorPOSIX(int aError)
{
    return (aError == 0 ? CHIP_NO_ERROR : CHIP_ERROR(ChipError::Range::kPOSIX, aError));
}
#endif

#if defined(_WIN32)

namespace {

constexpr uint32_t kMaximumEncapsulatedOsError = UINT32_C(0x00FFFFFF);
constexpr uint32_t kHResultFacilityWin32Mask   = UINT32_C(0xFFFF0000);
constexpr uint32_t kHResultFacilityWin32       = UINT32_C(0x80070000);

#if CHIP_CONFIG_ERROR_SOURCE && CHIP_CONFIG_ERROR_STD_SOURCE_LOCATION
CHIP_ERROR MapWindowsValue(uint32_t code, std::source_location location)
{
    return code <= kMaximumEncapsulatedOsError ? CHIP_ERROR(ChipError::Range::kOS, static_cast<int32_t>(code), location)
                                               : CHIP_ERROR_INVALID_ARGUMENT;
}
#elif CHIP_CONFIG_ERROR_SOURCE
CHIP_ERROR MapWindowsValue(uint32_t code, const char * file, unsigned int line)
{
    return code <= kMaximumEncapsulatedOsError ? CHIP_ERROR(ChipError::Range::kOS, static_cast<int32_t>(code), file, line)
                                               : CHIP_ERROR_INVALID_ARGUMENT;
}
#else
CHIP_ERROR MapWindowsValue(uint32_t code)
{
    return code <= kMaximumEncapsulatedOsError
        ? CHIP_ERROR(ChipError::Range::kOS, static_cast<int32_t>(code))
        : CHIP_ERROR_INVALID_ARGUMENT;
}
#endif

} // namespace

#if CHIP_CONFIG_ERROR_SOURCE && CHIP_CONFIG_ERROR_STD_SOURCE_LOCATION
DLL_EXPORT CHIP_ERROR MapErrorWindows(uint32_t code, std::source_location location)
{
    return code == ERROR_SUCCESS ? CHIP_NO_ERROR : MapWindowsValue(code, location);
}

DLL_EXPORT CHIP_ERROR MapErrorHRESULT(int32_t code, std::source_location location)
{
    if (code >= 0)
    {
        return CHIP_NO_ERROR;
    }
    const uint32_t unsignedCode = static_cast<uint32_t>(code);
    return (unsignedCode & kHResultFacilityWin32Mask) == kHResultFacilityWin32
        ? MapWindowsValue(unsignedCode & UINT32_C(0xFFFF), location)
        : CHIP_ERROR(ChipError::Range::kPlatformExtended, code, location);
}
#elif CHIP_CONFIG_ERROR_SOURCE
DLL_EXPORT CHIP_ERROR MapErrorWindows(uint32_t code, const char * file, unsigned int line)
{
    return code == ERROR_SUCCESS ? CHIP_NO_ERROR : MapWindowsValue(code, file, line);
}

DLL_EXPORT CHIP_ERROR MapErrorHRESULT(int32_t code, const char * file, unsigned int line)
{
    if (code >= 0)
    {
        return CHIP_NO_ERROR;
    }
    const uint32_t unsignedCode = static_cast<uint32_t>(code);
    return (unsignedCode & kHResultFacilityWin32Mask) == kHResultFacilityWin32
        ? MapWindowsValue(unsignedCode & UINT32_C(0xFFFF), file, line)
        : CHIP_ERROR(ChipError::Range::kPlatformExtended, code, file, line);
}
#else
DLL_EXPORT CHIP_ERROR MapErrorWindows(uint32_t code)
{
    return code == ERROR_SUCCESS ? CHIP_NO_ERROR : MapWindowsValue(code);
}

DLL_EXPORT CHIP_ERROR MapErrorHRESULT(int32_t code)
{
    if (code >= 0)
    {
        return CHIP_NO_ERROR;
    }
    const uint32_t unsignedCode = static_cast<uint32_t>(code);
    return (unsignedCode & kHResultFacilityWin32Mask) == kHResultFacilityWin32
        ? MapWindowsValue(unsignedCode & UINT32_C(0xFFFF))
        : CHIP_ERROR(ChipError::Range::kPlatformExtended, code);
}
#endif

#endif // defined(_WIN32)
} // namespace Internal

/**
 * This implements a function to return an NULL-terminated OS-specific descriptive C string, associated with the specified, mapped
 * OS error.
 *
 *  @param[in] aError  The mapped OS-specific error to describe.
 *
 *  @return A NULL-terminated, OS-specific descriptive C string describing the error.
 */
DLL_EXPORT const char * DescribeErrorPOSIX(CHIP_ERROR aError)
{
    const int lError = static_cast<int>(aError.GetValue());
#if CHIP_SYSTEM_CONFIG_THREAD_LOCAL_STORAGE
    static thread_local char errBuf[128];
#else
    static char errBuf[128];
#endif // CHIP_SYSTEM_CONFIG_THREAD_LOCAL_STORAGE

    // Use thread-safe strerror_r when available
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    // GNU version returns char*
    const char * s = strerror_r(lError, errBuf, sizeof(errBuf));
    if (s != nullptr)
    {
        return s; // errBuf or suitable glibc buffer
    }
#elif defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L)
    // POSIX version returns int (0 on success)
    if (strerror_r(lError, errBuf, sizeof(errBuf)) == 0)
    {
        return errBuf;
    }
#else
    // Fallback for platforms without strerror_r
    const char * s = strerror(lError);
    if (s != nullptr)
    {
#if defined(_WIN32)
        const size_t length = strlen(s) < sizeof(errBuf) ? strlen(s) : sizeof(errBuf) - 1;
        memcpy(errBuf, s, length);
        errBuf[length] = '\0';
#else
        chip::Platform::CopyString(errBuf, sizeof(errBuf), s);
#endif
        return errBuf;
    }
#endif

    return "Unknown POSIX error";
}

/**
 * Register a text error formatter for POSIX errors.
 */
void RegisterPOSIXErrorFormatter()
{
    static ErrorFormatter sPOSIXErrorFormatter = { FormatPOSIXError, nullptr };
    static bool sRegistered                    = false;
    if (sRegistered)
    {
        return;
    }
    RegisterErrorFormatter(&sPOSIXErrorFormatter);
    sRegistered = true;
}

/**
 * Given a POSIX error, returns a human-readable NULL-terminated C string
 * describing the error.
 *
 * @param[in] buf                   Buffer into which the error string will be placed.
 * @param[in] bufSize               Size of the supplied buffer in bytes.
 * @param[in] err                   The error to be described.
 *
 * @return true                     If a description string was written into the supplied buffer.
 * @return false                    If the supplied error was not a POSIX error.
 *
 */
bool FormatPOSIXError(char * buf, uint16_t bufSize, CHIP_ERROR err)
{
    if (err.IsRange(ChipError::Range::kPOSIX))
    {
        const char * desc =
#if CHIP_CONFIG_SHORT_ERROR_STR
            nullptr;
#else
            DescribeErrorPOSIX(err);
#endif
        FormatError(buf, bufSize, "OS", err, desc);
        return true;
    }

    return false;
}

#if defined(_WIN32)

DLL_EXPORT uint32_t GetWindowsError(CHIP_ERROR error)
{
    return static_cast<uint32_t>(error.GetValue()) & UINT32_C(0x00FFFFFF);
}

DLL_EXPORT int32_t GetHRESULT(CHIP_ERROR error)
{
    return static_cast<int32_t>(UINT32_C(0x80000000) | (static_cast<uint32_t>(error.GetValue()) & UINT32_C(0x7FFFFFFF)));
}

DLL_EXPORT const char * DescribeErrorWindows(CHIP_ERROR error)
{
#if CHIP_SYSTEM_CONFIG_THREAD_LOCAL_STORAGE
    static thread_local char errorBuffer[256];
#else
    static char errorBuffer[256];
#endif
    wchar_t wideBuffer[256];
    const DWORD nativeError = error.IsRange(ChipError::Range::kPlatformExtended)
        ? static_cast<DWORD>(GetHRESULT(error))
        : static_cast<DWORD>(GetWindowsError(error));
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, nativeError, 0, wideBuffer,
                                  static_cast<DWORD>(sizeof(wideBuffer) / sizeof(wideBuffer[0])), nullptr);
    while (length > 0 &&
           (wideBuffer[length - 1] == L'\r' || wideBuffer[length - 1] == L'\n' || wideBuffer[length - 1] == L' '))
    {
        --length;
    }

    if (length == 0)
    {
        return "Unknown Windows error";
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideBuffer, static_cast<int>(length), errorBuffer,
                                                static_cast<int>(sizeof(errorBuffer) - 1), nullptr, nullptr);
    if (utf8Length <= 0)
    {
        return "Unknown Windows error";
    }

    errorBuffer[utf8Length] = '\0';
    return errorBuffer;
}

void RegisterWindowsErrorFormatter()
{
    static ErrorFormatter formatter = { FormatWindowsError, nullptr };
    static bool registered          = false;
    if (!registered)
    {
        RegisterErrorFormatter(&formatter);
        registered = true;
    }
}

bool FormatWindowsError(char * buf, uint16_t bufSize, CHIP_ERROR err)
{
    if (!err.IsRange(ChipError::Range::kOS) && !err.IsRange(ChipError::Range::kPlatformExtended))
    {
        return false;
    }

    const char * description =
#if CHIP_CONFIG_SHORT_ERROR_STR
        nullptr;
#else
        DescribeErrorWindows(err);
#endif
    FormatError(buf, bufSize, err.IsRange(ChipError::Range::kOS) ? "Win32" : "HRESULT", err, description);
    return true;
}

#endif // defined(_WIN32)

/**
 * This implements a mapping function for CHIP System Layer errors that allows mapping integers in the number space of the
 * Zephyr OS user API stack errors into the POSIX range.
 *
 *  @param[in] aError  The native Zephyr API error to map.
 *
 *  @return The mapped POSIX error.
 */
DLL_EXPORT CHIP_ERROR MapErrorZephyr(int aError)
{
    return Internal::MapErrorPOSIX(-aError CHIP_ERROR_SOURCE_LOCATION_NULL);
}

#if CHIP_SYSTEM_CONFIG_USE_LWIP

/**
 * This implements a mapping function for CHIP System Layer errors that allows mapping underlying LwIP network stack errors into a
 * platform- or system-specific range.
 *
 *  @param[in] aError  The LwIP error to map.
 *
 *  @return The mapped LwIP network or OS error.
 *
 */
DLL_EXPORT CHIP_ERROR MapErrorLwIP(err_t aError)
{
    static_assert(ChipError::CanEncapsulate(ChipError::Range::kLwIP, err_t{}), "Can't represent all LWIP errors");
    return (aError == ERR_OK ? CHIP_NO_ERROR : CHIP_ERROR(ChipError::Range::kLwIP, static_cast<int>(-aError)));
}

/**
 * This implements a function to return an NULL-terminated LwIP-specific descriptive C string, associated with the specified,
 * mapped LwIP error.
 *
 *  @param[in] aError  The mapped LwIP-specific error to describe.
 *
 *  @return A NULL-terminated, LwIP-specific descriptive C string describing the error.
 *
 */
DLL_EXPORT const char * DescribeErrorLwIP(CHIP_ERROR aError)
{
    if (!aError.IsRange(ChipError::Range::kLwIP))
    {
        return nullptr;
    }

    const err_t lError = static_cast<err_t>(-static_cast<err_t>(aError.GetValue()));

    // If we are not compiling with LWIP_DEBUG asserted, the unmapped
    // local value may go unused.

    (void) lError;

    return lwip_strerr(lError);
}

/**
 * Register a text error formatter for LwIP errors.
 */
void RegisterLwIPErrorFormatter()
{
    static ErrorFormatter sLwIPErrorFormatter = { FormatLwIPError, nullptr };

    RegisterErrorFormatter(&sLwIPErrorFormatter);
}

/**
 * Given an LwIP error, returns a human-readable NULL-terminated C string
 * describing the error.
 *
 * @param[in] buf                   Buffer into which the error string will be placed.
 * @param[in] bufSize               Size of the supplied buffer in bytes.
 * @param[in] err                   The error to be described.
 *
 * @return true                     If a description string was written into the supplied buffer.
 * @return false                    If the supplied error was not an LwIP error.
 *
 */
bool FormatLwIPError(char * buf, uint16_t bufSize, CHIP_ERROR err)
{
    if (err.IsRange(ChipError::Range::kLwIP))
    {
        const char * desc =
#if CHIP_CONFIG_SHORT_ERROR_STR
            nullptr;
#else
            DescribeErrorLwIP(err);
#endif
        chip::FormatError(buf, bufSize, "LwIP", err, desc);
        return true;
    }
    return false;
}

#endif // CHIP_SYSTEM_CONFIG_USE_LWIP

} // namespace System
} // namespace chip
