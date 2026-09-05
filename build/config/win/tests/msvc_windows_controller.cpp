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

#include <app/server-cluster/testing/EmptyProvider.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/PersistentStorageOpCertStore.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <crypto/PersistentStorageOperationalKeystore.h>
#include <crypto/RawKeySessionKeystore.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestGroupData.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KvsPersistentStorageDelegate.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace chip;
using namespace chip::Controller;
using namespace chip::DeviceLayer;

namespace {

constexpr NodeId kControllerNodeId                = 0x112233;
constexpr FabricId kControllerFabricId            = 1;
constexpr unsigned kDefaultTimeoutSeconds         = 120;

class EmptyAttestationTrustStore final : public Credentials::AttestationTrustStore
{
public:
    CHIP_ERROR GetProductAttestationAuthorityCert(const ByteSpan &, MutableByteSpan &) const override
    {
        return CHIP_ERROR_CA_CERT_NOT_FOUND;
    }
};

struct PairingState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool complete    = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
};

class PairingDelegate final : public DevicePairingDelegate, public Credentials::DeviceAttestationDelegate
{
public:
    explicit PairingDelegate(PairingState & state) : mState(state) {}

    void OnCommissioningComplete(NodeId deviceId, CHIP_ERROR error) override
    {
        std::printf("Commissioning completed for node 0x%016llX: %" CHIP_ERROR_FORMAT "\n",
                    static_cast<unsigned long long>(deviceId), error.Format());
        {
            std::lock_guard<std::mutex> lock(mState.mutex);
            mState.complete = true;
            mState.error    = error;
        }
        mState.condition.notify_all();
    }

    Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override { return NullOptional; }

    void OnDeviceAttestationCompleted(DeviceCommissioner * commissioner, DeviceProxy * device,
                                      const Credentials::DeviceAttestationVerifier::AttestationDeviceInfo &,
                                      Credentials::AttestationVerificationResult result) override
    {
        if (result != Credentials::AttestationVerificationResult::kSuccess)
        {
            std::printf("WARNING: device attestation returned %u; continuing because this is a development commissioner.\n",
                        static_cast<unsigned>(result));
        }
        const CHIP_ERROR error =
            commissioner->ContinueCommissioningAfterDeviceAttestation(device, Credentials::AttestationVerificationResult::kSuccess);
        if (error != CHIP_NO_ERROR)
        {
            OnCommissioningComplete(kUndefinedNodeId, error);
        }
    }

private:
    PairingState & mState;
};

struct ControllerState
{
    KvsPersistentStorageDelegate storage;
    PersistentStorageOperationalKeystore operationalKeystore;
    Credentials::PersistentStorageOpCertStore opCertStore;
    Crypto::RawKeySessionKeystore sessionKeystore;
    Credentials::GroupDataProviderImpl groupDataProvider;
    ExampleOperationalCredentialsIssuer credentialsIssuer;
    EmptyAttestationTrustStore attestationTrustStore;
    Testing::EmptyProvider dataModelProvider;
    DeviceCommissioner commissioner;
    PairingState pairingState;
    PairingDelegate pairingDelegate{ pairingState };
    bool factoryInitialized      = false;
    bool commissionerInitialized = false;
};

bool ParseUnsigned(const char * text, uint64_t minimum, uint64_t maximum, uint64_t & value)
{
    char * end                       = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum)
    {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

CHIP_ERROR InitializeController(ControllerState & state, bool & restored)
{
    std::printf("Initializing persistent controller storage...\n");
    ReturnErrorOnFailure(state.storage.Init(&DeviceLayer::PersistedStorage::KeyValueStoreMgr()));
    ReturnErrorOnFailure(state.operationalKeystore.Init(&state.storage));
    ReturnErrorOnFailure(state.opCertStore.Init(&state.storage));

    std::printf("Initializing group data...\n");
    state.groupDataProvider.SetStorageDelegate(&state.storage);
    state.groupDataProvider.SetSessionKeystore(&state.sessionKeystore);
    ReturnErrorOnFailure(state.groupDataProvider.Init());
    Credentials::SetGroupDataProvider(&state.groupDataProvider);

    Controller::FactoryInitParams factoryParams;
    factoryParams.fabricIndependentStorage = &state.storage;
    factoryParams.operationalKeystore      = &state.operationalKeystore;
    factoryParams.opCertStore              = &state.opCertStore;
    factoryParams.sessionKeystore          = &state.sessionKeystore;
    factoryParams.groupDataProvider        = &state.groupDataProvider;
    factoryParams.dataModelProvider        = &state.dataModelProvider;
    std::printf("Initializing controller factory...\n");
    ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().Init(factoryParams));
    state.factoryInitialized = true;

    std::printf("Initializing operational credentials issuer...\n");
    ReturnErrorOnFailure(state.credentialsIssuer.Initialize(state.storage));

    SetupParams commissionerParams;
    commissionerParams.operationalCredentialsDelegate = &state.credentialsIssuer;
    commissionerParams.controllerVendorId             = VendorId::TestVendor1;
    commissionerParams.pairingDelegate                 = &state.pairingDelegate;
    commissionerParams.deviceAttestationVerifier =
        Credentials::GetDefaultDACVerifier(&state.attestationTrustStore, nullptr);

    FabricTable * fabrics = DeviceControllerFactory::GetInstance().GetSystemState()->Fabrics();
    VerifyOrReturnError(fabrics != nullptr, CHIP_ERROR_INCORRECT_STATE);
    if (fabrics->FabricCount() > 0)
    {
        std::printf("Restoring a persisted controller fabric...\n");
        commissionerParams.fabricIndex.SetValue(fabrics->begin()->GetFabricIndex());
        restored = true;
    }
    else
    {
        std::printf("Creating a new controller fabric...\n");
        uint8_t nocBuffer[kMaxCHIPDERCertLength];
        uint8_t icacBuffer[kMaxCHIPDERCertLength];
        uint8_t rcacBuffer[kMaxCHIPDERCertLength];
        MutableByteSpan noc(nocBuffer);
        MutableByteSpan icac(icacBuffer);
        MutableByteSpan rcac(rcacBuffer);
        Crypto::P256Keypair operationalKey;

        ReturnErrorOnFailure(operationalKey.Initialize(Crypto::ECPKeyTarget::ECDSA));
        ReturnErrorOnFailure(state.credentialsIssuer.GenerateNOCChainAfterValidation(
            kControllerNodeId, kControllerFabricId, kUndefinedCATs, operationalKey.Pubkey(), rcac, icac, noc));

        commissionerParams.operationalKeypair = &operationalKey;
        commissionerParams.controllerRCAC     = rcac;
        commissionerParams.controllerICAC     = icac;
        commissionerParams.controllerNOC      = noc;
        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(commissionerParams, state.commissioner));
        state.commissionerInitialized = true;
    }

    if (!state.commissionerInitialized)
    {
        std::printf("Initializing the commissioner on the persisted fabric...\n");
        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(commissionerParams, state.commissioner));
        state.commissionerInitialized = true;
    }

    uint8_t compressedFabricId[sizeof(uint64_t)];
    MutableByteSpan compressedFabricIdSpan(compressedFabricId);
    ReturnErrorOnFailure(state.commissioner.GetCompressedFabricIdBytes(compressedFabricIdSpan));
    ReturnErrorOnFailure(Credentials::SetSingleIpkEpochKey(
        &state.groupDataProvider, state.commissioner.GetFabricIndex(), GroupTesting::DefaultIpkValue::GetDefaultIpk(),
        compressedFabricIdSpan));
    return CHIP_NO_ERROR;
}

void ShutdownController(ControllerState & state)
{
    PlatformMgr().LockChipStack();
    if (state.commissionerInitialized)
    {
        state.commissioner.Shutdown();
        state.commissionerInitialized = false;
    }
    if (state.factoryInitialized)
    {
        DeviceControllerFactory::GetInstance().Shutdown();
        state.factoryInitialized = false;
    }
    Credentials::SetGroupDataProvider(nullptr);
    state.groupDataProvider.Finish();
    state.opCertStore.Finish();
    state.operationalKeystore.Finish();
    PlatformMgr().UnlockChipStack();
}

void PrintUsage(const char * executable)
{
    std::fprintf(stderr, "Usage:\n");
    std::fprintf(stderr, "  %s status\n", executable);
    std::fprintf(stderr, "  %s pair <node-id> <setup-code> [timeout-seconds: 1-600]\n", executable);
}

int RunController(bool pairMode, NodeId nodeId, const char * setupCode, unsigned timeoutSeconds)
{
    ControllerState state;
    bool restored    = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    PlatformMgr().LockChipStack();
    error = InitializeController(state, restored);
    PlatformMgr().UnlockChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Controller initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        const bool factoryOwnedPlatform = state.factoryInitialized;
        (void) PlatformMgr().StopEventLoopTask();
        ShutdownController(state);
        if (!factoryOwnedPlatform)
        {
            PlatformMgr().Shutdown();
        }
        return 1;
    }

    std::printf("%s controller fabric %u (controller node 0x%016llX).\n", restored ? "Restored" : "Created",
                static_cast<unsigned>(state.commissioner.GetFabricIndex()),
                static_cast<unsigned long long>(state.commissioner.GetNodeId()));

    int exitCode = 0;
    if (pairMode)
    {
        CommissioningParameters commissioningParams;
        commissioningParams.SetDeviceAttestationDelegate(&state.pairingDelegate);

        std::printf("WARNING: development mode accepts device-attestation failures; do not use this tool in production.\n");
        std::printf("Pairing node 0x%016llX over IP; timeout is %u seconds.\n",
                    static_cast<unsigned long long>(nodeId), timeoutSeconds);
        PlatformMgr().LockChipStack();
        error = state.commissioner.PairDevice(nodeId, setupCode, commissioningParams, DiscoveryType::kDiscoveryNetworkOnly);
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Unable to start pairing: %" CHIP_ERROR_FORMAT "\n", error.Format());
            exitCode = 1;
        }
        else
        {
            std::unique_lock<std::mutex> lock(state.pairingState.mutex);
            const bool completed = state.pairingState.condition.wait_for(
                lock, std::chrono::seconds(timeoutSeconds), [&state]() { return state.pairingState.complete; });
            if (!completed)
            {
                lock.unlock();
                std::fprintf(stderr, "Pairing timed out.\n");
                PlatformMgr().LockChipStack();
                (void) state.commissioner.StopPairing(nodeId);
                PlatformMgr().UnlockChipStack();
                lock.lock();
                (void) state.pairingState.condition.wait_for(
                    lock, std::chrono::seconds(5), [&state]() { return state.pairingState.complete; });
                exitCode = 2;
            }
            else if (state.pairingState.error != CHIP_NO_ERROR)
            {
                exitCode = 1;
            }
        }
    }

    (void) PlatformMgr().StopEventLoopTask();
    ShutdownController(state);
    return exitCode;
}

} // namespace

int main(int argc, char * argv[])
{
    const bool statusMode = argc == 2 && std::strcmp(argv[1], "status") == 0;
    const bool pairMode   = argc == 4 || argc == 5;
    uint64_t nodeIdValue  = 0;
    uint64_t timeoutValue = kDefaultTimeoutSeconds;
    if ((!statusMode && !pairMode) ||
        (pairMode && (std::strcmp(argv[1], "pair") != 0 || !ParseUnsigned(argv[2], 1, kMaxOperationalNodeId, nodeIdValue) ||
                      (argc == 5 && !ParseUnsigned(argv[4], 1, 600, timeoutValue)))))
    {
        PrintUsage(argv[0]);
        return 1;
    }

    CHIP_ERROR error = Platform::MemoryInit();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Memory initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        return 1;
    }

    error = PlatformMgr().InitChipStack();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Platform initialization failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        Platform::MemoryShutdown();
        return 1;
    }
    error = PlatformMgr().StartEventLoopTask();
    if (error != CHIP_NO_ERROR)
    {
        std::fprintf(stderr, "Event-loop startup failed: %" CHIP_ERROR_FORMAT "\n", error.Format());
        PlatformMgr().Shutdown();
        Platform::MemoryShutdown();
        return 1;
    }

    const int exitCode =
        RunController(pairMode, static_cast<NodeId>(nodeIdValue), pairMode ? argv[3] : nullptr, static_cast<unsigned>(timeoutValue));
    Platform::MemoryShutdown();
    return exitCode;
}
