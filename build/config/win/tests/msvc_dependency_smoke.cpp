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

#include <json/json.h>
#include <mbedtls/sha256.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

int main()
{
    constexpr char kInput[] = "Matter on Windows";
    std::array<uint8_t, SHA256_DIGEST_LENGTH> boringDigest{};
    std::array<uint8_t, SHA256_DIGEST_LENGTH> mbedDigest{};

    if (SHA256(reinterpret_cast<const uint8_t *>(kInput), std::strlen(kInput), boringDigest.data()) == nullptr)
    {
        return 1;
    }
    if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char *>(kInput), std::strlen(kInput), mbedDigest.data(), 0) != 0)
    {
        return 1;
    }
    if (boringDigest != mbedDigest)
    {
        return 1;
    }

    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::string document = R"({"platform":"windows","native":true})";
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(document.data(), document.data() + document.size(), &parsed, &errors))
    {
        return 1;
    }

    return parsed["platform"].asString() == "windows" && parsed["native"].asBool() ? 0 : 1;
}
