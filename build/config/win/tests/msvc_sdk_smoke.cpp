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

#include <lib/support/Base64.h>

#include <array>
#include <cstdint>
#include <cstring>

int main()
{
    constexpr std::array<uint8_t, 3> kInput = { 0x4d, 0x53, 0x56 };
    constexpr char kExpected[]                 = "TVNW";

    std::array<char, BASE64_ENCODED_LEN(kInput.size())> encoded;
    const uint16_t encodedLength = chip::Base64Encode(kInput.data(), static_cast<uint16_t>(kInput.size()), encoded.data());
    if (encodedLength != encoded.size() || std::memcmp(encoded.data(), kExpected, encoded.size()) != 0)
    {
        return 1;
    }

    std::array<uint8_t, kInput.size()> decoded;
    const uint16_t decodedLength = chip::Base64Decode(encoded.data(), encodedLength, decoded.data());
    return decodedLength == decoded.size() && decoded == kInput ? 0 : 1;
}
