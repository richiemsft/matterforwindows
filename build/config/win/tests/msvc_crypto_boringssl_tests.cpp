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
 *      Focused native-MSVC correctness tests for the repository-pinned BoringSSL
 *      CryptoPAL (chip_crypto="boringssl", OPENSSL_NO_ASM).
 *
 *      These tests drive the real CryptoPAL primitives through their public
 *      Matter API and check them against authoritative published known-answer
 *      vectors (NIST FIPS 180-4, RFC 4231, RFC 5869, RFC 6070/7914, RFC 6979)
 *      plus algebraic correctness properties. They cover hashing (one-shot and
 *      streaming), HMAC, HKDF, PBKDF2, AES-CCM AEAD, ECDSA signatures/keypairs,
 *      deterministic ECDSA (RFC 6979), ECDH key agreement, and the DRBG.
 *
 *      Why inline vectors instead of the src/crypto/tests headers: the existing
 *      repository vector headers rely on GCC/Clang extensions that native MSVC
 *      rejects under /permissive- (zero-length arrays such as `const uint8_t
 *      x[] = {};` for empty messages/salts/AAD, and C++20 designated
 *      initializers under /std:c++17). The upstream GoogleTest suites in
 *      src/crypto/tests additionally pull the credentials/CHIPCert closure and
 *      lib/core/StringBuilderAdapters.h (Pigweed pw_string), neither of which
 *      is a Windows closure yet. This driver therefore exercises the same real
 *      CryptoPAL implementation with self-contained vectors while those
 *      closures remain blocked.
 */

#include <gtest/gtest.h>

#include <crypto/CHIPCryptoPAL.h>
#include <crypto/DefaultSessionKeystore.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>

#include <array>
#include <cstdint>
#include <cstring>

using namespace chip;
using namespace chip::Crypto;

namespace {

// EXPECT/ASSERT helpers that avoid the Pigweed StringBuilder gtest adapters.
#define EXPECT_CHIP_OK(expr) EXPECT_EQ((expr), CHIP_NO_ERROR)
#define ASSERT_CHIP_OK(expr) ASSERT_EQ((expr), CHIP_NO_ERROR)

// -------------------------------------------------------------------------
// SHA-256 (FIPS 180-4 examples)
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, Sha256KnownAnswers)
{
    // Empty message.
    {
        const uint8_t expected[kSHA256_Hash_Length] = { 0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                                        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                                        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
        uint8_t digest[kSHA256_Hash_Length] = { 0 };
        const uint8_t empty                 = 0;
        EXPECT_CHIP_OK(Hash_SHA256(&empty, 0, digest));
        EXPECT_EQ(memcmp(digest, expected, sizeof(expected)), 0);
    }

    // "abc".
    {
        const uint8_t message[]                     = { 'a', 'b', 'c' };
        const uint8_t expected[kSHA256_Hash_Length] = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                                        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                                        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
        uint8_t digest[kSHA256_Hash_Length] = { 0 };
        EXPECT_CHIP_OK(Hash_SHA256(message, sizeof(message), digest));
        EXPECT_EQ(memcmp(digest, expected, sizeof(expected)), 0);
    }

    // 448-bit message "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".
    {
        const char message[]                        = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        const uint8_t expected[kSHA256_Hash_Length] = { 0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
                                                        0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
                                                        0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
        uint8_t digest[kSHA256_Hash_Length] = { 0 };
        EXPECT_CHIP_OK(Hash_SHA256(reinterpret_cast<const uint8_t *>(message), sizeof(message) - 1, digest));
        EXPECT_EQ(memcmp(digest, expected, sizeof(expected)), 0);
    }
}

TEST(WindowsBoringSslCryptoPAL, Sha256StreamingMatchesOneShot)
{
    const char message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const size_t len     = sizeof(message) - 1;

    uint8_t oneShot[kSHA256_Hash_Length] = { 0 };
    ASSERT_CHIP_OK(Hash_SHA256(reinterpret_cast<const uint8_t *>(message), len, oneShot));

    Hash_SHA256_stream stream;
    ASSERT_CHIP_OK(stream.Begin());
    const size_t split = 20;
    ASSERT_CHIP_OK(stream.AddData(ByteSpan(reinterpret_cast<const uint8_t *>(message), split)));
    ASSERT_CHIP_OK(stream.AddData(ByteSpan(reinterpret_cast<const uint8_t *>(message) + split, len - split)));

    uint8_t streamed[kSHA256_Hash_Length] = { 0 };
    MutableByteSpan streamedSpan(streamed);
    ASSERT_CHIP_OK(stream.Finish(streamedSpan));
    EXPECT_EQ(streamedSpan.size(), kSHA256_Hash_Length);
    EXPECT_EQ(memcmp(streamed, oneShot, kSHA256_Hash_Length), 0);
}

// -------------------------------------------------------------------------
// HMAC-SHA256 (RFC 4231 Test Case 2)
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, HmacSha256Rfc4231Case2)
{
    const uint8_t key[]     = { 'J', 'e', 'f', 'e' };
    const uint8_t message[] = { 'w', 'h', 'a', 't', ' ', 'd', 'o', ' ', 'y', 'a', ' ', 'w', 'a', 'n',
                                't', ' ', 'f', 'o', 'r', ' ', 'n', 'o', 't', 'h', 'i', 'n', 'g', '?' };
    const uint8_t expected[kSHA256_Hash_Length] = { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                                                    0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                                                    0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 };

    HMAC_sha hmac;
    uint8_t out[kSHA256_Hash_Length] = { 0 };
    EXPECT_CHIP_OK(hmac.HMAC_SHA256(key, sizeof(key), message, sizeof(message), out, sizeof(out)));
    EXPECT_EQ(memcmp(out, expected, sizeof(expected)), 0);
}

// -------------------------------------------------------------------------
// HKDF-SHA256 (RFC 5869 Test Case 1)
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, HkdfSha256Rfc5869Case1)
{
    const uint8_t ikm[22] = { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                              0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
    const uint8_t salt[13] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c };
    const uint8_t info[10] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9 };
    const uint8_t expected[42] = { 0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36,
                                   0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56,
                                   0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65 };

    HKDF_sha hkdf;
    uint8_t out[42] = { 0 };
    EXPECT_CHIP_OK(hkdf.HKDF_SHA256(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info), out, sizeof(out)));
    EXPECT_EQ(memcmp(out, expected, sizeof(expected)), 0);
}

// -------------------------------------------------------------------------
// PBKDF2-HMAC-SHA256. The Matter PBKDF2 enforces the Spake2p salt-length
// bounds (>= 16 bytes), so this uses the repository's authoritative vector
// (password="password", salt="saltSALTsaltSALT", c=1, dkLen=20).
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, Pbkdf2Sha256KnownAnswer)
{
    const uint8_t password[]   = { 'p', 'a', 's', 's', 'w', 'o', 'r', 'd' };
    const uint8_t salt[]       = { 's', 'a', 'l', 't', 'S', 'A', 'L', 'T', 's', 'a', 'l', 't', 'S', 'A', 'L', 'T' };
    const uint8_t expected[20] = { 0xf2, 0xe3, 0x4b, 0xd9, 0x50, 0xe9, 0x1c, 0xf3, 0x7d, 0x22,
                                   0xe1, 0x13, 0x5a, 0x39, 0x9b, 0x02, 0xa1, 0x7c, 0xb1, 0x93 };

    PBKDF2_sha256 pbkdf;
    uint8_t out[20] = { 0 };
    EXPECT_CHIP_OK(pbkdf.pbkdf2_sha256(password, sizeof(password), salt, sizeof(salt), 1, sizeof(out), out));
    EXPECT_EQ(memcmp(out, expected, sizeof(expected)), 0);
}

// -------------------------------------------------------------------------
// AES-CCM-128 AEAD correctness (inverse, determinism, authentication)
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, AesCcm128AeadRoundTripAndAuth)
{
    DefaultSessionKeystore keystore;

    Symmetric128BitsKeyByteArray keyMaterial = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
                                                 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f };
    Aes128KeyHandle key;
    ASSERT_CHIP_OK(keystore.CreateKey(keyMaterial, key));

    const uint8_t nonce[kAES_CCM128_Nonce_Length] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
                                                      0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c };
    const uint8_t aad[]       = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
    const uint8_t plaintext[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                  0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31 };

    std::array<uint8_t, sizeof(plaintext)> ciphertext{};
    std::array<uint8_t, kAES_CCM128_Tag_Length> tag{};
    ASSERT_CHIP_OK(AES_CCM_encrypt(plaintext, sizeof(plaintext), aad, sizeof(aad), key, nonce, sizeof(nonce),
                                   ciphertext.data(), tag.data(), tag.size()));

    // Determinism: the same inputs must produce the same ciphertext and tag.
    std::array<uint8_t, sizeof(plaintext)> ciphertext2{};
    std::array<uint8_t, kAES_CCM128_Tag_Length> tag2{};
    ASSERT_CHIP_OK(AES_CCM_encrypt(plaintext, sizeof(plaintext), aad, sizeof(aad), key, nonce, sizeof(nonce),
                                   ciphertext2.data(), tag2.data(), tag2.size()));
    EXPECT_EQ(ciphertext, ciphertext2);
    EXPECT_EQ(tag, tag2);

    // Ciphertext must differ from plaintext (confidentiality sanity).
    EXPECT_NE(memcmp(ciphertext.data(), plaintext, sizeof(plaintext)), 0);

    // Inverse: decryption recovers the plaintext.
    std::array<uint8_t, sizeof(plaintext)> recovered{};
    ASSERT_CHIP_OK(AES_CCM_decrypt(ciphertext.data(), ciphertext.size(), aad, sizeof(aad), tag.data(), tag.size(), key, nonce,
                                   sizeof(nonce), recovered.data()));
    EXPECT_EQ(memcmp(recovered.data(), plaintext, sizeof(plaintext)), 0);

    // Authentication: a flipped tag bit must be rejected.
    {
        std::array<uint8_t, kAES_CCM128_Tag_Length> badTag = tag;
        badTag[0]                                          = static_cast<uint8_t>(badTag[0] ^ 0x80);
        EXPECT_NE(AES_CCM_decrypt(ciphertext.data(), ciphertext.size(), aad, sizeof(aad), badTag.data(), badTag.size(), key,
                                  nonce, sizeof(nonce), recovered.data()),
                  CHIP_NO_ERROR);
    }

    // Authentication: a flipped ciphertext bit must be rejected.
    {
        std::array<uint8_t, sizeof(plaintext)> badCipher = ciphertext;
        badCipher[0]                                     = static_cast<uint8_t>(badCipher[0] ^ 0x01);
        EXPECT_NE(AES_CCM_decrypt(badCipher.data(), badCipher.size(), aad, sizeof(aad), tag.data(), tag.size(), key, nonce,
                                  sizeof(nonce), recovered.data()),
                  CHIP_NO_ERROR);
    }

    // Authentication: the wrong key must be rejected.
    {
        Symmetric128BitsKeyByteArray otherMaterial = { 0 };
        Aes128KeyHandle otherKey;
        ASSERT_CHIP_OK(keystore.CreateKey(otherMaterial, otherKey));
        EXPECT_NE(AES_CCM_decrypt(ciphertext.data(), ciphertext.size(), aad, sizeof(aad), tag.data(), tag.size(), otherKey,
                                  nonce, sizeof(nonce), recovered.data()),
                  CHIP_NO_ERROR);
        keystore.DestroyKey(otherKey);
    }

    keystore.DestroyKey(key);
}

// -------------------------------------------------------------------------
// ECDSA P-256 signatures and keypairs
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, EcdsaSignVerifyRoundTrip)
{
    P256Keypair keypair;
    ASSERT_CHIP_OK(keypair.Initialize(ECPKeyTarget::ECDSA));

    const uint8_t message[] = "Matter native Windows BoringSSL CryptoPAL";
    const size_t messageLen = sizeof(message) - 1;

    P256ECDSASignature signature;
    ASSERT_CHIP_OK(keypair.ECDSA_sign_msg(message, messageLen, signature));
    EXPECT_CHIP_OK(keypair.Pubkey().ECDSA_validate_msg_signature(message, messageLen, signature));

    // A modified message must fail verification.
    uint8_t tampered[sizeof(message)];
    memcpy(tampered, message, sizeof(message));
    tampered[0] = static_cast<uint8_t>(tampered[0] ^ 0x01);
    EXPECT_NE(keypair.Pubkey().ECDSA_validate_msg_signature(tampered, messageLen, signature), CHIP_NO_ERROR);

    // A signature from a different keypair must not verify.
    P256Keypair other;
    ASSERT_CHIP_OK(other.Initialize(ECPKeyTarget::ECDSA));
    EXPECT_NE(other.Pubkey().ECDSA_validate_msg_signature(message, messageLen, signature), CHIP_NO_ERROR);
}

TEST(WindowsBoringSslCryptoPAL, DeterministicEcdsaRfc6979)
{
    // RFC 6979 A.2.5 (P-256 / SHA-256) sample key.
    const uint8_t privateKey[kP256_PrivateKey_Length] = { 0xC9, 0xAF, 0xA9, 0xD8, 0x45, 0xBA, 0x75, 0x16, 0x6B, 0x5C, 0x21,
                                                          0x57, 0x67, 0xB1, 0xD6, 0x93, 0x4E, 0x50, 0xC3, 0xDB, 0x36, 0xE8,
                                                          0x9B, 0x12, 0x7B, 0x8A, 0x62, 0x2B, 0x12, 0x0F, 0x67, 0x21 };
    const uint8_t publicKeyX[kP256_FE_Length] = { 0x60, 0xFE, 0xD4, 0xBA, 0x25, 0x5A, 0x9D, 0x31, 0xC9, 0x61, 0xEB,
                                                  0x74, 0xC6, 0x35, 0x6D, 0x68, 0xC0, 0x49, 0xB8, 0x92, 0x3B, 0x61,
                                                  0xFA, 0x6C, 0xE6, 0x69, 0x62, 0x2E, 0x60, 0xF2, 0x9F, 0xB6 };
    const uint8_t publicKeyY[kP256_FE_Length] = { 0x79, 0x03, 0xFE, 0x10, 0x08, 0xB8, 0xBC, 0x99, 0xA4, 0x1A, 0xE9,
                                                  0xE9, 0x56, 0x28, 0xBC, 0x64, 0xF2, 0xF1, 0xB2, 0x0C, 0x2D, 0x7E,
                                                  0x9F, 0x51, 0x77, 0xA3, 0xC2, 0x94, 0xD4, 0x46, 0x22, 0x99 };

    uint8_t publicKey[kP256_Point_Length];
    publicKey[0] = 0x04;
    memcpy(&publicKey[1], publicKeyX, sizeof(publicKeyX));
    memcpy(&publicKey[1 + sizeof(publicKeyX)], publicKeyY, sizeof(publicKeyY));

    P256Keypair keypair;
    ASSERT_CHIP_OK(keypair.HazardousOperationLoadKeypairFromRaw(ByteSpan(privateKey, sizeof(privateKey)),
                                                                ByteSpan(publicKey, sizeof(publicKey))));

    const uint8_t message[] = { 's', 'a', 'm', 'p', 'l', 'e' };
    const uint8_t expectedSignature[kP256_ECDSA_Signature_Length_Raw] = {
        0xEF, 0xD4, 0x8B, 0x2A, 0xAC, 0xB6, 0xA8, 0xFD, 0x11, 0x40, 0xDD, 0x9C, 0xD4, 0x5E, 0x81, 0xD6,
        0x9D, 0x2C, 0x87, 0x7B, 0x56, 0xAA, 0xF9, 0x91, 0xC3, 0x4D, 0x0E, 0xA8, 0x4E, 0xAF, 0x37, 0x16,
        0xF7, 0xCB, 0x1C, 0x94, 0x2D, 0x65, 0x7C, 0x41, 0xD4, 0x36, 0xC7, 0xA1, 0xB6, 0xE2, 0x9F, 0x65,
        0xF3, 0xE9, 0x00, 0xDB, 0xB9, 0xAF, 0xF4, 0x06, 0x4D, 0xC4, 0xAB, 0x2F, 0x84, 0x3A, 0xCD, 0xA8,
    };

    // Determinism (RFC 6979): repeated deterministic signatures are identical.
    P256ECDSASignature signatureA;
    P256ECDSASignature signatureB;
    ASSERT_CHIP_OK(keypair.ECDSA_sign_msg_det(message, sizeof(message), signatureA));
    ASSERT_CHIP_OK(keypair.ECDSA_sign_msg_det(message, sizeof(message), signatureB));
    ASSERT_EQ(signatureA.Length(), kP256_ECDSA_Signature_Length_Raw);
    ASSERT_EQ(signatureA.Length(), signatureB.Length());
    EXPECT_EQ(memcmp(signatureA.ConstBytes(), signatureB.ConstBytes(), signatureA.Length()), 0);
    EXPECT_EQ(memcmp(signatureA.ConstBytes(), expectedSignature, sizeof(expectedSignature)), 0);

    // The deterministic signature must verify against the public key.
    EXPECT_CHIP_OK(keypair.Pubkey().ECDSA_validate_msg_signature(message, sizeof(message), signatureA));
}

// -------------------------------------------------------------------------
// ECDH P-256 key agreement
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, EcdhSharedSecretAgreement)
{
    P256Keypair alice;
    P256Keypair bob;
    ASSERT_CHIP_OK(alice.Initialize(ECPKeyTarget::ECDH));
    ASSERT_CHIP_OK(bob.Initialize(ECPKeyTarget::ECDH));

    P256ECDHDerivedSecret secretAlice;
    P256ECDHDerivedSecret secretBob;
    ASSERT_CHIP_OK(alice.ECDH_derive_secret(bob.Pubkey(), secretAlice));
    ASSERT_CHIP_OK(bob.ECDH_derive_secret(alice.Pubkey(), secretBob));

    ASSERT_EQ(secretAlice.Length(), kP256_FE_Length);
    ASSERT_EQ(secretAlice.Length(), secretBob.Length());
    EXPECT_EQ(memcmp(secretAlice.ConstBytes(), secretBob.ConstBytes(), secretAlice.Length()), 0);

    // A third party must derive a different shared secret.
    P256Keypair eve;
    ASSERT_CHIP_OK(eve.Initialize(ECPKeyTarget::ECDH));
    P256ECDHDerivedSecret secretEve;
    ASSERT_CHIP_OK(eve.ECDH_derive_secret(alice.Pubkey(), secretEve));
    EXPECT_NE(memcmp(secretEve.ConstBytes(), secretAlice.ConstBytes(), secretAlice.Length()), 0);
}

// -------------------------------------------------------------------------
// DRBG random source
// -------------------------------------------------------------------------

TEST(WindowsBoringSslCryptoPAL, DrbgProducesEntropy)
{
    std::array<uint8_t, 64> first{};
    std::array<uint8_t, 64> second{};
    ASSERT_CHIP_OK(DRBG_get_bytes(first.data(), first.size()));
    ASSERT_CHIP_OK(DRBG_get_bytes(second.data(), second.size()));

    std::array<uint8_t, 64> zero{};
    EXPECT_NE(first, zero);
    EXPECT_NE(first, second);
}

} // namespace

int main(int argc, char ** argv)
{
    if (chip::Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        return 1;
    }
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    chip::Platform::MemoryShutdown();
    return result;
}
