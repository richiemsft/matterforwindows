/*
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
#include "CameraSignalingCommand.h"

#include <app/InteractionModelEngine.h>
#include <clusters/Descriptor/ClusterId.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <json/json.h>
#include <system/SystemClock.h>

#include <array>
#include <fstream>

using namespace chip;
using chip::Controller::CameraProviderClient;
using chip::Controller::CameraSignalingCluster;

namespace {
// Test-vendor-specific diagnostic type; this is not a Camera Controller product.
constexpr app::DataModel::DeviceTypeEntry kDiagnosticTypes[] = { { 0xFFF10001, 1 } };
} // namespace

CHIP_ERROR CameraSignalingAccess::Check(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                                        Access::Privilege privilege)
{
    VerifyOrReturnError(mRequestor != nullptr, CHIP_ERROR_ACCESS_DENIED);
    if (path.endpoint == mRequestor->GetPaths()[0].mEndpointId && path.cluster == app::Clusters::Descriptor::Id)
    {
        VerifyOrReturnError(subject.authMode == Access::AuthMode::kCase && !subject.isCommissioning &&
                                subject.subject == mPeer.GetNodeId() && subject.fabricIndex == mPeer.GetFabricIndex() &&
                                privilege == Access::Privilege::kView,
                            CHIP_ERROR_ACCESS_DENIED);
        return CHIP_NO_ERROR;
    }
    return mRequestor->CheckAccess(subject, path, privilege);
}

CameraSignalingCommand::CameraSignalingCommand(CredentialIssuerCommands * credentials) :
    CHIPCommand("signaling-check", credentials, "Negotiate a video-only offer, then end the remote signaling session.")
{
    AddArgument("node-id", 1, UINT64_MAX, &mNodeId);
    AddArgument("endpoint-id", 0, kInvalidEndpointId - 1, &mCamera);
    AddArgument("video-stream-id", 0, UINT16_MAX - 1, &mVideoStream, "An already allocated LiveView stream owned by the caller.");
    AddArgument("offer-file", &mOfferFile, "UTF-8 SDP offer from an external media engine (maximum 16 KiB).");
    AddArgument("timeout", 1, 25, &mTimeout, "Connection/negotiation budget in seconds (default 20), plus up to 30s for End.");
    AddArgument("listen-port", 1, UINT16_MAX, &mListenPort,
                "Incoming CASE/TCP port (configured storage port, or 5540 by default). Must be available locally.");
    AddArgument("signals-file", &mSignalsFile,
                "Optional NEW JSONL file for answer/ICE/End/error data. Contains sensitive SDP and network details.");
}

CHIP_ERROR CameraSignalingCommand::ReadOffer(std::istream & input, std::string & offer)
{
    offer.clear();
    std::array<char, CameraSignalingCluster::kMaxSdpBytes + 1> buffer{};
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    VerifyOrReturnError(!input.bad(), CHIP_ERROR_READ_FAILED);
    const auto size = static_cast<size_t>(input.gcount());
    VerifyOrReturnError(
        CameraSignalingCluster::IsValidSignalingText(CharSpan(buffer.data(), size), CameraSignalingCluster::kMaxSdpBytes),
        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    offer.assign(buffer.data(), size);
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraSignalingCommand::Run()
{
    if (IsInteractive())
    {
        ChipLogError(chipTool, "Camera signaling-check requires a standalone invocation.");
        return CHIP_ERROR_INCORRECT_STATE;
    }
    VerifyOrReturnError(IsOperationalNodeId(mNodeId), CHIP_ERROR_INVALID_ARGUMENT);
    std::ifstream input(mOfferFile, std::ios::binary);
    if (!input.is_open())
    {
        ChipLogError(chipTool, "Cannot open the camera SDP offer file.");
        return CHIP_ERROR_READ_FAILED;
    }
    ReturnErrorOnFailure(ReadOffer(input, mOffer));
    if (mSignalsFile.HasValue())
    {
        // Exclusive creation avoids overwriting an offer, existing transcript, or controller storage.
        mOutput.reset(std::fopen(mSignalsFile.Value(), "wbx"));
        if (!mOutput)
        {
            ChipLogError(chipTool, "Cannot create a NEW camera signals file.");
            return CHIP_ERROR_WRITE_FAILED;
        }
        mTranscript.reserve(CameraSignalingCluster::kMaxSignals);
    }
    CHIP_ERROR status = CHIPCommand::Run();
    if (mOwnFactory)
    {
        // Setup can fail before CHIPCommand reaches its normal command cleanup.
        if (!mCleanupDone)
        {
            Cleanup();
            MaybeTearDownStack();
        }
        Controller::DeviceControllerFactory::GetInstance().Shutdown();
        mProvider.reset();
        mOwnFactory  = false;
        mCleanupDone = false;
    }
    // File I/O happens only after the event loop has stopped, never on the Matter task.
    CHIP_ERROR transcriptError = WriteTranscript();
    if (status == CHIP_NO_ERROR)
    {
        status = transcriptError;
    }
    if (mOutput && std::fclose(mOutput.release()) != 0)
    {
        ChipLogError(chipTool, "Failed to close the camera signals file.");
        if (status == CHIP_NO_ERROR)
        {
            status = CHIP_ERROR_WRITE_FAILED;
        }
    }
    mOffer.clear();
    mTranscript.clear();
    return status;
}

System::Clock::Timeout CameraSignalingCommand::GetWaitDuration() const
{
    return System::Clock::Seconds32(mTimeout.ValueOr(20) + 35);
}

CHIP_ERROR CameraSignalingCommand::ConfigureFactory(Controller::FactoryInitParams & params)
{
    VerifyOrReturnError(Controller::DeviceControllerFactory::GetInstance().GetSystemState() == nullptr, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(mAttributes.Init(&mDefaultStorage));
    mProvider.emplace(mDefaultStorage, mAttributes);
    params.dataModelProvider = &*mProvider;
    mOwnFactory              = true;
    params.enableTCPServer   = true;
    // A common nonzero port is required for UDP/TCP listeners and DNS-SD advertisement.
    params.listenPort = mListenPort.ValueOr(params.listenPort != 0 ? params.listenPort : CHIP_PORT);
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraSignalingCommand::RunCommand()
{
    VerifyOrReturnError(mProvider.has_value() && app::InteractionModelEngine::GetInstance()->GetDataModelProvider() == &*mProvider,
                        CHIP_ERROR_INCORRECT_STATE);
    auto state = Controller::DeviceControllerFactory::GetInstance().GetSystemState();
    VerifyOrReturnError(state != nullptr && state->Fabrics() != nullptr && state->SystemLayer() != nullptr,
                        CHIP_ERROR_INCORRECT_STATE);
    mSystem = state->SystemLayer();
    EndpointId endpoint;
    ReturnErrorOnFailure(Controller::CameraRequestorEndpoint::FindAvailableEndpoint(*mProvider, endpoint));
    mRequestor.emplace(endpoint, Span<const app::DataModel::DeviceTypeEntry>(kDiagnosticTypes), *state->Fabrics(), *mSystem);
    mAccess.SetRequestor(&mRequestor->Signaling(), CurrentCommissioner().GetPeerScopedId(mNodeId));
    // Init refuses to replace an application's existing ACL.
    ReturnErrorOnFailure(Access::GetAccessControl().Init(&mAccess, mDeviceTypes));
    mAccessOwned = true;
    ReturnErrorOnFailure(mRequestor->Register(*mProvider, Access::GetAccessControl()));
    using Selection = Controller::CameraSignalingCluster::StreamSelection;
    ReturnErrorOnFailure(mRequestor->Signaling().CreateSession(CurrentCommissioner().GetPeerScopedId(mNodeId), mCamera,
                                                               Selection::ExplicitId(mVideoStream), Selection::Absent(), mHandle));
    mClient   = std::make_unique<CameraProviderClient>(mRequestor->Signaling(), mHandle);
    mPhase    = Phase::Connecting;
    mDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(mTimeout.ValueOr(20));
    ReturnErrorOnFailure(mSystem->StartTimer(System::Clock::Milliseconds32(20), OnPoll, this));
    ChipLogProgress(chipTool, "Camera signaling-check: CASE/TCP, requestor endpoint %u, no media decoding.", endpoint);
    return CurrentCommissioner().GetConnectedDevice(mNodeId, &mConnected, &mConnectionFailed,
                                                    TransportPayloadCapability::kLargePayload);
}

void CameraSignalingCommand::OnConnected(void * context, Messaging::ExchangeManager & exchanges, const SessionHandle & session)
{
    auto & command = *static_cast<CameraSignalingCommand *>(context);
    VerifyOrReturn(command.mPhase == Phase::Connecting);
    VerifyOrReturn(command.mSession.Grab(session), command.BeginEnd(CHIP_ERROR_NOT_CONNECTED));
    command.mExchanges = &exchanges;
    command.mPhase     = Phase::Offering;
    CHIP_ERROR error   = command.mClient->SendOffer(exchanges, session, CharSpan(command.mOffer.data(), command.mOffer.size()));
    if (error != CHIP_NO_ERROR)
    {
        command.BeginEnd(error);
    }
}

void CameraSignalingCommand::OnConnectionFailed(void * context, const ScopedNodeId &, CHIP_ERROR error)
{
    static_cast<CameraSignalingCommand *>(context)->BeginEnd(error);
}

void CameraSignalingCommand::OnPoll(System::Layer *, void * context)
{
    static_cast<CameraSignalingCommand *>(context)->Poll();
}

CHIP_ERROR CameraSignalingCommand::RecordSignal(CameraSignalingCluster::Signal && signal)
{
    static constexpr const char * names[] = { "answer", "candidates", "end", "failure" };
    const char * name                     = names[static_cast<size_t>(signal.type)];
    ChipLogProgress(chipTool, "Camera %s: SDP bytes=%u, ICE candidates=%u", name, static_cast<unsigned>(signal.sdp.size()),
                    static_cast<unsigned>(signal.candidates.size()));
    if (!mOutput || mOutputFailed)
    {
        return CHIP_NO_ERROR;
    }
    if (mTranscript.size() == CameraSignalingCluster::kMaxSignals)
    {
        mOutputFailed = true;
        ChipLogError(chipTool, "Camera transcript queue is full.");
        return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
    }
    mTranscript.push_back(std::move(signal));
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraSignalingCommand::WriteTranscript()
{
    if (!mOutput)
    {
        return CHIP_NO_ERROR;
    }
    static constexpr const char * names[] = { "answer", "candidates", "end", "failure" };
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    for (const auto & signal : mTranscript)
    {
        Json::Value value;
        value["type"]              = names[static_cast<size_t>(signal.type)];
        value["handle"]            = Json::UInt64(signal.handle);
        value["nodeId"]            = Json::UInt64(signal.peer.GetNodeId());
        value["fabricIndex"]       = signal.peer.GetFabricIndex();
        value["cameraEndpointId"]  = signal.cameraEndpoint;
        value["protocolSessionId"] = signal.protocolId.has_value() ? Json::Value(*signal.protocolId) : Json::Value();
        value["sdp"]               = signal.sdp;
        value["error"]             = Json::UInt(signal.error.AsInteger());
        value["reason"]            = static_cast<unsigned>(signal.reason);
        value["candidates"]        = Json::Value(Json::arrayValue);
        for (const auto & candidate : signal.candidates)
        {
            Json::Value entry;
            entry["candidate"]     = candidate.candidate;
            entry["sdpMid"]        = candidate.mid.has_value() ? Json::Value(*candidate.mid) : Json::Value();
            entry["sdpMLineIndex"] = candidate.lineIndex.has_value() ? Json::Value(*candidate.lineIndex) : Json::Value();
            value["candidates"].append(std::move(entry));
        }
        const std::string line = Json::writeString(writer, value) + "\n";
        if (std::fwrite(line.data(), 1, line.size(), mOutput.get()) != line.size())
        {
            ChipLogError(chipTool, "Failed to write the camera signals file.");
            return CHIP_ERROR_WRITE_FAILED;
        }
    }
    if (std::fflush(mOutput.get()) != 0)
    {
        ChipLogError(chipTool, "Failed to flush the camera signals file.");
        return CHIP_ERROR_WRITE_FAILED;
    }
    return CHIP_NO_ERROR;
}

void CameraSignalingCommand::Poll()
{
    VerifyOrReturn(mPhase != Phase::Complete);
    if (auto result = mClient->TakeResult(); result.has_value())
    {
        if (mPhase == Phase::Ending)
        {
            if (result->error != CHIP_NO_ERROR)
            {
                ChipLogError(chipTool, "Camera remote End failed: %" CHIP_ERROR_FORMAT, result->error.Format());
            }
            Finish(mFinalError == CHIP_NO_ERROR ? result->error : mFinalError);
            return;
        }
        if (result->error != CHIP_NO_ERROR)
        {
            BeginEnd(result->error);
        }
        else
        {
            ChipLogProgress(chipTool, "Camera offer response accepted.");
        }
    }
    while (mPhase != Phase::Complete)
    {
        auto signal = mRequestor->Signaling().TakeSignal();
        if (!signal.has_value())
        {
            break;
        }
        const auto type       = signal->type;
        const auto error      = signal->error;
        const auto reason     = signal->reason;
        CHIP_ERROR writeError = RecordSignal(std::move(*signal));
        if (writeError != CHIP_NO_ERROR)
        {
            if (mPhase == Phase::Ending)
            {
                if (mFinalError == CHIP_NO_ERROR)
                {
                    mFinalError = writeError;
                }
            }
            else
            {
                BeginEnd(writeError);
            }
        }
        if (mPhase != Phase::Ending && mPhase != Phase::Complete)
        {
            if (type == CameraSignalingCluster::SignalType::Failure)
            {
                BeginEnd(error);
            }
            else if (type == CameraSignalingCluster::SignalType::End)
            {
                ChipLogError(chipTool, "Camera ended before the signaling check completed (reason=%u).",
                             static_cast<unsigned>(reason));
                Finish(CHIP_ERROR_INCORRECT_STATE);
            }
        }
    }
    VerifyOrReturn(mPhase != Phase::Complete);
    if (mPhase != Phase::Ending && mRequestor->Signaling().IsNegotiated(mHandle))
    {
        ChipLogProgress(chipTool, "Camera signaling established (offer response and answer); attempting remote End.");
        BeginEnd(CHIP_NO_ERROR);
    }
    if (mPhase != Phase::Complete && System::SystemClock().GetMonotonicTimestamp() >= mDeadline)
    {
        if (mPhase == Phase::Ending)
        {
            ChipLogError(chipTool, "Camera remote End timed out.");
            Finish(mFinalError == CHIP_NO_ERROR ? CHIP_ERROR_TIMEOUT : mFinalError);
        }
        else
        {
            BeginEnd(CHIP_ERROR_TIMEOUT);
        }
    }
    if (mPhase != Phase::Complete)
    {
        CHIP_ERROR error = mSystem->StartTimer(System::Clock::Milliseconds32(20), OnPoll, this);
        if (error != CHIP_NO_ERROR)
        {
            ChipLogError(chipTool, "Camera polling failed; remote cleanup cannot be awaited: %" CHIP_ERROR_FORMAT, error.Format());
            Finish(mFinalError == CHIP_NO_ERROR ? error : mFinalError);
        }
    }
}

void CameraSignalingCommand::BeginEnd(CHIP_ERROR status)
{
    VerifyOrReturn(mPhase != Phase::Complete && mPhase != Phase::Ending);
    mFinalError = status;
    if (status != CHIP_NO_ERROR)
    {
        ChipLogError(chipTool, "Camera signaling failed: %" CHIP_ERROR_FORMAT, status.Format());
    }
    mConnected.Cancel();
    mConnectionFailed.Cancel();
    mClient->Cancel();
    (void) mClient->TakeResult();
    CameraSignalingCluster::SessionInfo info;
    if (mRequestor->Signaling().GetSessionInfoForEnd(mHandle, info) != CHIP_NO_ERROR || !info.protocolId.has_value())
    {
        ChipLogProgress(chipTool, "Remote End skipped: no authenticated protocol session identity.");
        Finish(status == CHIP_NO_ERROR ? CHIP_ERROR_INCORRECT_STATE : status);
        return;
    }
    if (!mSession || mExchanges == nullptr)
    {
        ChipLogError(chipTool, "Remote End cannot be sent: CASE transport is unavailable.");
        Finish(status == CHIP_NO_ERROR ? CHIP_ERROR_NOT_CONNECTED : status);
        return;
    }
    mPhase = Phase::Ending;
    CHIP_ERROR error =
        mClient->SendEnd(*mExchanges, mSession.Get().Value(), app::Clusters::Globals::WebRTCEndReasonEnum::kUserHangup);
    if (error != CHIP_NO_ERROR)
    {
        ChipLogError(chipTool, "Camera remote End could not be sent: %" CHIP_ERROR_FORMAT, error.Format());
        Finish(status == CHIP_NO_ERROR ? error : status);
        return;
    }
    mDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(30);
}

void CameraSignalingCommand::Finish(CHIP_ERROR status)
{
    mSystem->CancelTimer(OnPoll, this);
    mPhase = Phase::Complete;
    SetCommandExitStatus(status);
}

void CameraSignalingCommand::Cleanup()
{
    mConnected.Cancel();
    mConnectionFailed.Cancel();
    if (mSystem != nullptr)
    {
        mSystem->CancelTimer(OnPoll, this);
    }
    mClient.reset();
    mSession.Release();
    mExchanges = nullptr;
    if (mRequestor.has_value())
    {
        if (mHandle != 0)
        {
            LogErrorOnFailure(mRequestor->Signaling().CloseSession(mHandle));
        }
        LogErrorOnFailure(mRequestor->Unregister());
        mRequestor.reset();
    }
    mAccess.SetRequestor(nullptr, ScopedNodeId());
    if (mAccessOwned)
    {
        Access::GetAccessControl().Finish();
    }
    mCleanupDone  = true;
    mAccessOwned  = false;
    mOutputFailed = false;
    mSystem       = nullptr;
    mHandle       = 0;
    mPhase        = Phase::Complete;
    mFinalError   = CHIP_NO_ERROR;
}
