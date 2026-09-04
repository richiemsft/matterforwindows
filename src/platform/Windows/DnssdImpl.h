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
 *      Native Windows DNS-SD backend for chip::Dnssd, implemented on top of
 *      the Win32 windns.h service-discovery APIs (DnsServiceRegister/
 *      DnsServiceBrowse/DnsServiceResolve and their DeRegister/Cancel
 *      counterparts). This backend uses the native OS mDNS responder; it does
 *      not bind UDP 5353 or implement any part of the mDNS protocol itself.
 *
 *      The public entry points below are the standard chip::Dnssd platform
 *      contract declared in src/lib/dnssd/platform/Dnssd.h. This header also
 *      exposes a small set of pure, deterministic helpers used by
 *      DnssdImpl.cpp for name construction, UTF-8/UTF-16 conversion, status
 *      mapping, and interface-index conversion. They are exposed so the
 *      focused Windows DNS-SD smoke test (build/config/win/tests) can
 *      exercise these conversion seams without depending on live network
 *      state.
 */

#pragma once

#include <inet/InetInterface.h>
#include <lib/core/CHIPError.h>
#include <lib/dnssd/platform/Dnssd.h>

#include <cstdint>
#include <string>

namespace chip {
namespace Dnssd {
namespace Windows {

/// Builds the `<type>.<protocol>` string (e.g. "_matterc._udp") used for both
/// the Windows DNS-SD query/registration name and the base type reported by
/// the publish-success callback.
std::string MakeFullServiceType(const char * type, DnssdServiceProtocol protocol);

/// Returns the base service type from either a base type (for example,
/// "_matterc") or a subtype query (for example,
/// "_L840._sub._matterc").
std::string GetBaseServiceType(const char * type);

/// Converts a NUL-terminated UTF-8 string to a UTF-16 string. Fails with a
/// mapped CHIP_ERROR if the input is not well-formed UTF-8.
CHIP_ERROR Utf8ToWide(const char * utf8, std::wstring & out);

/// Converts a NUL-terminated UTF-16 string to a UTF-8 std::string. Fails with
/// a mapped CHIP_ERROR if the input cannot be represented in UTF-8.
CHIP_ERROR WideToUtf8(const wchar_t * wide, std::string & out);

/// Maps a Windows DNS-SD completion status (as delivered to a
/// DNS_SERVICE_*_CALLBACK) to a CHIP_ERROR. ERROR_SUCCESS maps to
/// CHIP_NO_ERROR and ERROR_CANCELLED maps to the canonical
/// CHIP_ERROR_CANCELLED; every other status is wrapped with
/// CHIP_ERROR_WINDOWS().
CHIP_ERROR MapServiceStatus(unsigned long status);

/// Converts a numeric Windows interface index (as used by the DNS-SD request
/// structures) to a Matter Inet::InterfaceId. Returns InterfaceId::Null() if
/// the index does not name a live interface (including index 0, which the
/// DNS-SD APIs treat as "any interface").
Inet::InterfaceId InterfaceIdFromIndex(uint32_t index);

} // namespace Windows
} // namespace Dnssd
} // namespace chip
