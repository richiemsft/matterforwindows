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
#include <app-common/zap-generated/cluster-objects.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/CHIPCluster.h>
#include <controller/CurrentFabricRemover.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <controller/InvokeInteraction.h>
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
constexpr unsigned kDefaultSubscriptionSeconds    = 30;

enum class Mode
{
    kStatus,
    kPair,
    kOn,
    kOff,
    kReadOnOff,
    kSubscribeOnOff,
    kRemoveFabric,
};

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
        std::printf("Commissioning complete for node 0x%016llX: %" CHIP_ERROR_FORMAT "\n",
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

struct OnOffOperationState
{
    OnOffOperationState(Mode requestedMode, EndpointId requestedEndpoint) :
        mode(requestedMode), endpoint(requestedEndpoint), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (complete)
            {
                return;
            }
            error    = result;
            complete = true;
        }
        condition.notify_all();
    }

    void FinishRead(bool value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (complete)
            {
                return;
            }
            onOffValue = value;
            error      = CHIP_NO_ERROR;
            complete   = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeMgr, const SessionHandle & sessionHandle)
    {
        auto * state = static_cast<OnOffOperationState *>(context);
        VerifyOrReturn(state != nullptr);

        CHIP_ERROR error = CHIP_NO_ERROR;
        if (state->mode == Mode::kReadOnOff)
        {
            Controller::ClusterBase cluster(exchangeMgr, sessionHandle, state->endpoint);
            error = cluster.ReadAttribute<app::Clusters::OnOff::Attributes::OnOff::TypeInfo>(
                state,
                [](void * callbackContext, bool value) {
                    auto * operation = static_cast<OnOffOperationState *>(callbackContext);
                    operation->FinishRead(value);
                },
                [](void * callbackContext, CHIP_ERROR readError) {
                    static_cast<OnOffOperationState *>(callbackContext)->Finish(readError);
                });
        }
        else
        {
            auto onSuccess = [state](const app::ConcreteCommandPath &, const app::StatusIB & status,
                                     const app::DataModel::NullObjectType &) {
                state->Finish(status.ToChipError());
            };
            auto onFailure = [state](CHIP_ERROR commandError) { state->Finish(commandError); };

            if (state->mode == Mode::kOn)
            {
                app::Clusters::OnOff::Commands::On::Type request;
                error = Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, state->endpoint, request, onSuccess, onFailure);
            }
            else
            {
                app::Clusters::OnOff::Commands::Off::Type request;
                error = Controller::InvokeCommandRequest(&exchangeMgr, sessionHandle, state->endpoint, request, onSuccess, onFailure);
            }
        }

        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, const ScopedNodeId &, CHIP_ERROR error)
    {
        auto * state = static_cast<OnOffOperationState *>(context);
        VerifyOrReturn(state != nullptr);
        state->Finish(error);
    }

    Mode mode;
    EndpointId endpoint;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete    = false;
    bool onOffValue  = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    Callback::Callback<OnDeviceConnected> onConnected;
    Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

struct SubscriptionState
{
    SubscriptionState(EndpointId requestedEndpoint) :
        endpoint(requestedEndpoint), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void RecordReport(bool value)
    {
        unsigned currentReportCount = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (hasValue && value != lastValue)
            {
                valueChanged = true;
            }
            lastValue          = value;
            hasValue           = true;
            currentReportCount = ++reportCount;
        }
        std::printf("OnOff report %u: %s\n", currentReportCount, value ? "ON" : "OFF");
        condition.notify_all();
    }

    void MarkEstablished(SubscriptionId id)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            subscriptionId = id;
            established    = true;
        }
        std::printf("OnOff subscription established (ID 0x%08X).\n", static_cast<unsigned>(id));
        condition.notify_all();
    }

    void MarkInterrupted(CHIP_ERROR result, uint32_t nextIntervalMs)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            established = false;
            ++interruptionCount;
        }
        std::printf("OnOff subscription interrupted (%" CHIP_ERROR_FORMAT "); retrying in %u ms.\n", result.Format(),
                    nextIntervalMs);
        condition.notify_all();
    }

    void Fail(CHIP_ERROR result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failed)
            {
                return;
            }
            error  = result;
            failed = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeMgr, const SessionHandle & sessionHandle)
    {
        auto * state = static_cast<SubscriptionState *>(context);
        VerifyOrReturn(state != nullptr);

        Controller::ClusterBase cluster(exchangeMgr, sessionHandle, state->endpoint);
        const CHIP_ERROR error = cluster.SubscribeAttribute<app::Clusters::OnOff::Attributes::OnOff::TypeInfo>(
            state, [](void * callbackContext, bool value) { static_cast<SubscriptionState *>(callbackContext)->RecordReport(value); },
            [](void * callbackContext, CHIP_ERROR reportError) {
                static_cast<SubscriptionState *>(callbackContext)->Fail(reportError);
            },
            1, 5,
            [](void * callbackContext, SubscriptionId id) {
                static_cast<SubscriptionState *>(callbackContext)->MarkEstablished(id);
            },
            [](void * callbackContext, CHIP_ERROR resubscribeError, uint32_t nextIntervalMs) {
                static_cast<SubscriptionState *>(callbackContext)->MarkInterrupted(resubscribeError, nextIntervalMs);
            });
        if (error != CHIP_NO_ERROR)
        {
            state->Fail(error);
        }
    }

    static void HandleConnectionFailure(void * context, const ScopedNodeId &, CHIP_ERROR error)
    {
        auto * state = static_cast<SubscriptionState *>(context);
        VerifyOrReturn(state != nullptr);
        state->Fail(error);
    }

    EndpointId endpoint;
    std::mutex mutex;
    std::condition_variable condition;
    bool established             = false;
    bool failed                  = false;
    bool hasValue                = false;
    bool valueChanged            = false;
    bool lastValue               = false;
    unsigned reportCount         = 0;
    unsigned interruptionCount   = 0;
    SubscriptionId subscriptionId = 0;
    CHIP_ERROR error             = CHIP_NO_ERROR;
    Callback::Callback<OnDeviceConnected> onConnected;
    Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

struct FabricRemovalState
{
    explicit FabricRemovalState(DeviceController * controller) :
        remover(controller), callback(&HandleComplete, this)
    {}

    static void HandleComplete(void * context, NodeId, CHIP_ERROR result)
    {
        auto * state = static_cast<FabricRemovalState *>(context);
        VerifyOrReturn(state != nullptr);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->error    = result;
            state->complete = true;
        }
        state->condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete    = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    Controller::CurrentFabricRemover remover;
    Callback::Callback<Controller::OnCurrentFabricRemove> callback;
};

struct ConnectionState
{
    ConnectionState() : onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this) {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error    = result;
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager &, const SessionHandle &)
    {
        static_cast<ConnectionState *>(context)->Finish(CHIP_NO_ERROR);
    }

    static void HandleConnectionFailure(void * context, const ScopedNodeId &, CHIP_ERROR error)
    {
        static_cast<ConnectionState *>(context)->Finish(error);
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete    = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    Callback::Callback<OnDeviceConnected> onConnected;
    Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

CHIP_ERROR ConnectWithRetry(DeviceController & controller, NodeId nodeId, Callback::Callback<OnDeviceConnected> * onConnected,
                            Callback::Callback<OnDeviceConnectionFailure> * onFailure,
                            const Optional<AddressResolve::ResolveResult> & fallbackResult)
{
    auto * caseSessionManager = controller.CASESessionMgr();
    VerifyOrReturnError(caseSessionManager != nullptr, CHIP_ERROR_INCORRECT_STATE);

    caseSessionManager->FindOrEstablishSession(
        controller.GetPeerScopedId(nodeId), onConnected, onFailure,
#if CHIP_DEVICE_CONFIG_ENABLE_AUTOMATIC_CASE_RETRIES
        2, nullptr,
#endif
        TransportPayloadCapability::kMRPPayload, fallbackResult);
    return CHIP_NO_ERROR;
}

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
        const FabricIndex fabricIndex = fabrics->begin()->GetFabricIndex();
        if (fabrics->HasOperationalKeyForFabric(fabricIndex))
        {
            std::printf("Restoring a persisted controller fabric...\n");
            commissionerParams.fabricIndex.SetValue(fabricIndex);
            restored = true;
        }
        else
        {
            std::printf("Removing controller fabric %u because its operational key is missing...\n",
                        static_cast<unsigned>(fabricIndex));
            ReturnErrorOnFailure(fabrics->Delete(fabricIndex));
        }
    }

    if (!restored)
    {
        std::printf("Creating a new controller fabric...\n");
        uint8_t csrBuffer[Crypto::kMIN_CSR_Buffer_Size];
        uint8_t nocBuffer[kMaxCHIPDERCertLength];
        uint8_t icacBuffer[kMaxCHIPDERCertLength];
        uint8_t rcacBuffer[kMaxCHIPDERCertLength];
        MutableByteSpan csr(csrBuffer);
        MutableByteSpan noc(nocBuffer);
        MutableByteSpan icac(icacBuffer);
        MutableByteSpan rcac(rcacBuffer);
        Crypto::P256PublicKey operationalPublicKey;

        ReturnErrorOnFailure(fabrics->AllocatePendingOperationalKey(NullOptional, csr));
        ReturnErrorOnFailure(Crypto::VerifyCertificateSigningRequest(csr.data(), csr.size(), operationalPublicKey));
        ReturnErrorOnFailure(state.credentialsIssuer.GenerateNOCChainAfterValidation(
            kControllerNodeId, kControllerFabricId, kUndefinedCATs, operationalPublicKey, rcac, icac, noc));

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

    constexpr uint8_t operationalKeyProbe = 0;
    Crypto::P256ECDSASignature operationalKeyProbeSignature;
    ReturnErrorOnFailure(fabrics->SignWithOpKeypair(state.commissioner.GetFabricIndex(), ByteSpan(&operationalKeyProbe, 1),
                                                    operationalKeyProbeSignature));

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
    std::fprintf(stderr, "  %s on <node-id> <endpoint-id> [timeout-seconds: 1-600] [fallback-ip]\n", executable);
    std::fprintf(stderr, "  %s off <node-id> <endpoint-id> [timeout-seconds: 1-600] [fallback-ip]\n", executable);
    std::fprintf(stderr, "  %s read-onoff <node-id> <endpoint-id> [timeout-seconds: 1-600] [fallback-ip]\n", executable);
    std::fprintf(stderr,
                 "  %s subscribe-onoff <node-id> <endpoint-id> [report-timeout-seconds: 1-600] [fallback-ip]\n",
                 executable);
    std::fprintf(stderr, "  %s remove-fabric <node-id> [timeout-seconds: 1-600] [fallback-ip]\n", executable);
}

int RunController(Mode mode, NodeId nodeId, EndpointId endpointId, const char * setupCode, unsigned timeoutSeconds,
                  const char * fallbackIp)
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

    Optional<AddressResolve::ResolveResult> fallbackResult;
    if (fallbackIp != nullptr)
    {
        Inet::IPAddress address;
        VerifyOrDie(Inet::IPAddress::FromString(fallbackIp, address));
        AddressResolve::ResolveResult result;
        result.address = Transport::PeerAddress::UDP(address, CHIP_PORT);
        fallbackResult.SetValue(result);
        std::printf("Using %s:%u if operational DNS-SD does not resolve within %u seconds.\n", fallbackIp, CHIP_PORT,
                    CHIP_CONFIG_ADDRESS_RESOLVE_FALLBACK_TIMEOUT_SECONDS);
    }

    OnOffOperationState onOffOperation(mode, endpointId);
    SubscriptionState subscription(endpointId);
    FabricRemovalState fabricRemoval(&state.commissioner);
    ConnectionState connection;
    int exitCode = 0;
    if (mode == Mode::kPair)
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
    else if (mode == Mode::kOn || mode == Mode::kOff || mode == Mode::kReadOnOff)
    {
        PlatformMgr().LockChipStack();
        error = ConnectWithRetry(state.commissioner, nodeId, &onOffOperation.onConnected, &onOffOperation.onConnectionFailure,
                                 fallbackResult);
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Unable to start CASE connection: %" CHIP_ERROR_FORMAT "\n", error.Format());
            exitCode = 1;
        }
        else
        {
            std::unique_lock<std::mutex> lock(onOffOperation.mutex);
            if (!onOffOperation.condition.wait_for(lock, std::chrono::seconds(timeoutSeconds),
                                                   [&onOffOperation]() { return onOffOperation.complete; }))
            {
                std::fprintf(stderr, "On/Off operation timed out.\n");
                exitCode = 2;
            }
            else if (onOffOperation.error != CHIP_NO_ERROR)
            {
                std::fprintf(stderr, "On/Off operation failed: %" CHIP_ERROR_FORMAT "\n", onOffOperation.error.Format());
                exitCode = 1;
            }
        }
    }
    else if (mode == Mode::kSubscribeOnOff)
    {
        PlatformMgr().LockChipStack();
        error =
            ConnectWithRetry(state.commissioner, nodeId, &subscription.onConnected, &subscription.onConnectionFailure, fallbackResult);
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Unable to start CASE connection: %" CHIP_ERROR_FORMAT "\n", error.Format());
            exitCode = 1;
        }
        else
        {
            {
                std::unique_lock<std::mutex> lock(subscription.mutex);
                const bool started = subscription.condition.wait_for(
                    lock, std::chrono::seconds(kDefaultTimeoutSeconds),
                    [&subscription]() { return subscription.failed || (subscription.established && subscription.reportCount > 0); });
                if (!started)
                {
                    std::fprintf(stderr, "OnOff subscription establishment timed out.\n");
                    exitCode = 2;
                }
                else if (subscription.failed)
                {
                    std::fprintf(stderr, "OnOff subscription failed: %" CHIP_ERROR_FORMAT "\n", subscription.error.Format());
                    exitCode = 1;
                }
                else
                {
                    onOffOperation.mode = subscription.lastValue ? Mode::kOff : Mode::kOn;
                    std::printf("Sending %s to trigger a subscription report...\n",
                                onOffOperation.mode == Mode::kOn ? "On" : "Off");
                }
            }

            if (exitCode == 0)
            {
                PlatformMgr().LockChipStack();
                error = ConnectWithRetry(state.commissioner, nodeId, &onOffOperation.onConnected,
                                         &onOffOperation.onConnectionFailure, fallbackResult);
                PlatformMgr().UnlockChipStack();
                if (error != CHIP_NO_ERROR)
                {
                    std::fprintf(stderr, "Unable to start subscription test command: %" CHIP_ERROR_FORMAT "\n", error.Format());
                    exitCode = 1;
                }
            }

            if (exitCode == 0)
            {
                std::unique_lock<std::mutex> lock(onOffOperation.mutex);
                if (!onOffOperation.condition.wait_for(lock, std::chrono::seconds(kDefaultTimeoutSeconds),
                                                       [&onOffOperation]() { return onOffOperation.complete; }))
                {
                    std::fprintf(stderr, "Subscription test command timed out.\n");
                    exitCode = 2;
                }
                else if (onOffOperation.error != CHIP_NO_ERROR)
                {
                    std::fprintf(stderr, "Subscription test command failed: %" CHIP_ERROR_FORMAT "\n",
                                 onOffOperation.error.Format());
                    exitCode = 1;
                }
            }

            if (exitCode == 0)
            {
                std::printf("Waiting up to %u seconds for the resulting OnOff report...\n", timeoutSeconds);
                std::unique_lock<std::mutex> lock(subscription.mutex);
                const bool reported = subscription.condition.wait_for(
                    lock, std::chrono::seconds(timeoutSeconds),
                    [&subscription]() { return subscription.failed || subscription.valueChanged; });
                if (!reported)
                {
                    std::fprintf(stderr, "OnOff subscription did not report the commanded value transition.\n");
                    exitCode = 2;
                }
                else if (subscription.failed)
                {
                    std::fprintf(stderr, "OnOff subscription failed: %" CHIP_ERROR_FORMAT "\n", subscription.error.Format());
                    exitCode = 1;
                }
            }
        }
    }
    else if (mode == Mode::kRemoveFabric)
    {
        PlatformMgr().LockChipStack();
        error = ConnectWithRetry(state.commissioner, nodeId, &connection.onConnected, &connection.onConnectionFailure, fallbackResult);
        PlatformMgr().UnlockChipStack();
        if (error == CHIP_NO_ERROR)
        {
            std::unique_lock<std::mutex> lock(connection.mutex);
            if (!connection.condition.wait_for(lock, std::chrono::seconds(timeoutSeconds),
                                               [&connection]() { return connection.complete; }))
            {
                error = CHIP_ERROR_TIMEOUT;
            }
            else
            {
                error = connection.error;
            }
        }
        if (error != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Unable to establish CASE for remote fabric removal: %" CHIP_ERROR_FORMAT "\n", error.Format());
            exitCode = (error == CHIP_ERROR_TIMEOUT) ? 2 : 1;
        }
    }

    if (mode == Mode::kRemoveFabric && exitCode == 0)
    {
        PlatformMgr().LockChipStack();
        error = fabricRemoval.remover.RemoveCurrentFabric(nodeId, &fabricRemoval.callback);
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            std::fprintf(stderr, "Unable to start remote fabric removal: %" CHIP_ERROR_FORMAT "\n", error.Format());
            exitCode = 1;
        }
        else
        {
            std::unique_lock<std::mutex> lock(fabricRemoval.mutex);
            if (!fabricRemoval.condition.wait_for(lock, std::chrono::seconds(timeoutSeconds),
                                                  [&fabricRemoval]() { return fabricRemoval.complete; }))
            {
                std::fprintf(stderr, "Remote fabric removal timed out.\n");
                exitCode = 2;
            }
            else if (fabricRemoval.error != CHIP_NO_ERROR)
            {
                std::fprintf(stderr, "Remote fabric removal failed: %" CHIP_ERROR_FORMAT "\n", fabricRemoval.error.Format());
                exitCode = 1;
            }
        }
    }

    (void) PlatformMgr().StopEventLoopTask();
    ShutdownController(state);

    if (exitCode == 0)
    {
        if (mode == Mode::kPair)
        {
            std::printf("\n=== COMMISSIONING SUCCEEDED for node 0x%016llX ===\n",
                        static_cast<unsigned long long>(nodeId));
        }
        else if (mode == Mode::kReadOnOff)
        {
            std::printf("\n=== ON/OFF ATTRIBUTE: %s ===\n", onOffOperation.onOffValue ? "ON" : "OFF");
        }
        else if (mode == Mode::kOn || mode == Mode::kOff)
        {
            std::printf("\n=== ON/OFF COMMAND SUCCEEDED: %s ===\n", mode == Mode::kOn ? "ON" : "OFF");
        }
        else if (mode == Mode::kSubscribeOnOff)
        {
            std::printf("\n=== ON/OFF SUBSCRIPTION SUCCEEDED: %u report(s), %u interruption(s), last value %s ===\n",
                        subscription.reportCount, subscription.interruptionCount, subscription.lastValue ? "ON" : "OFF");
        }
        else if (mode == Mode::kRemoveFabric)
        {
            std::printf("\n=== REMOTE FABRIC REMOVAL SUCCEEDED for node 0x%016llX ===\n",
                        static_cast<unsigned long long>(nodeId));
        }
        else
        {
            std::printf("\n=== CONTROLLER STATUS SUCCEEDED ===\n");
        }
    }
    else
    {
        std::printf("\n=== OPERATION FAILED (exit code %d) ===\n", exitCode);
    }
    return exitCode;
}

} // namespace

int main(int argc, char * argv[])
{
    Mode mode             = Mode::kStatus;
    const bool statusMode = argc == 2 && std::strcmp(argv[1], "status") == 0;
    const bool pairMode   = (argc == 4 || argc == 5) && std::strcmp(argv[1], "pair") == 0;
    const bool onMode     = (argc >= 4 && argc <= 6) && std::strcmp(argv[1], "on") == 0;
    const bool offMode    = (argc >= 4 && argc <= 6) && std::strcmp(argv[1], "off") == 0;
    const bool readMode   = (argc >= 4 && argc <= 6) && std::strcmp(argv[1], "read-onoff") == 0;
    const bool subscribeMode = (argc >= 4 && argc <= 6) && std::strcmp(argv[1], "subscribe-onoff") == 0;
    const bool removeMode = (argc >= 3 && argc <= 5) && std::strcmp(argv[1], "remove-fabric") == 0;
    uint64_t nodeIdValue  = 0;
    uint64_t endpointValue = 0;
    uint64_t timeoutValue = kDefaultTimeoutSeconds;
    const bool hasFallbackIp = argc == 6 || (removeMode && argc == 5);
    const char * fallbackIp   = hasFallbackIp ? argv[argc - 1] : nullptr;
    Inet::IPAddress parsedFallbackIp;
    if (subscribeMode)
    {
        timeoutValue = kDefaultSubscriptionSeconds;
    }
    if ((!statusMode && !pairMode && !onMode && !offMode && !readMode && !subscribeMode && !removeMode) ||
        (pairMode && (!ParseUnsigned(argv[2], 1, kMaxOperationalNodeId, nodeIdValue) ||
                      (argc == 5 && !ParseUnsigned(argv[4], 1, 600, timeoutValue)))) ||
        ((onMode || offMode || readMode || subscribeMode) &&
         (!ParseUnsigned(argv[2], 1, kMaxOperationalNodeId, nodeIdValue) ||
          !ParseUnsigned(argv[3], 0, static_cast<uint64_t>(kInvalidEndpointId) - 1, endpointValue) ||
          (argc >= 5 && !ParseUnsigned(argv[4], 1, 600, timeoutValue)))) ||
        (removeMode && (!ParseUnsigned(argv[2], 1, kMaxOperationalNodeId, nodeIdValue) ||
                        (argc >= 4 && !ParseUnsigned(argv[3], 1, 600, timeoutValue)))) ||
        (hasFallbackIp && !Inet::IPAddress::FromString(fallbackIp, parsedFallbackIp)))
    {
        PrintUsage(argv[0]);
        return 1;
    }

    if (pairMode)
    {
        mode = Mode::kPair;
    }
    else if (onMode)
    {
        mode = Mode::kOn;
    }
    else if (offMode)
    {
        mode = Mode::kOff;
    }
    else if (readMode)
    {
        mode = Mode::kReadOnOff;
    }
    else if (subscribeMode)
    {
        mode = Mode::kSubscribeOnOff;
    }
    else if (removeMode)
    {
        mode = Mode::kRemoveFabric;
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

    const int exitCode = RunController(mode, static_cast<NodeId>(nodeIdValue), static_cast<EndpointId>(endpointValue),
                                       pairMode ? argv[3] : nullptr, static_cast<unsigned>(timeoutValue), fallbackIp);
    Platform::MemoryShutdown();
    return exitCode;
}
