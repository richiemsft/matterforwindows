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

#include "CameraProviderClient.h"

#include <app/data-model/Decode.h>
#include <clusters/shared/EnumsCheck.h>
#include <cstring>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

namespace chip::Controller {
namespace {
using namespace app::Clusters::WebRTCTransportProvider;
using CandidateType = app::Clusters::Globals::Structs::ICECandidateStruct::Type;

} // namespace

CameraProviderClient::~CameraProviderClient()
{
    Cancel();
}

CHIP_ERROR CameraProviderClient::ValidateTransport(const CameraSignalingCluster::SessionInfo & info, const SessionHandle & session)
{
    const auto subject = session->GetSubjectDescriptor();
    VerifyOrReturnError(session->IsActiveSession() && session->IsSecureSession() && subject.authMode == Access::AuthMode::kCase &&
                            !subject.isCommissioning &&
                            session->GetPeer() == ScopedNodeId(info.metadata.peerNodeID, info.metadata.fabricIndex),
                        CHIP_ERROR_ACCESS_DENIED);
    VerifyOrReturnError(session->AllowsLargePayload(), CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraProviderClient::Prepare(const SessionHandle & session, Command command)
{
    VerifyOrReturnError(mSender == nullptr, CHIP_ERROR_BUSY);
    {
        std::lock_guard<std::mutex> lock(mResultMutex);
        VerifyOrReturnError(!mResult.has_value(), CHIP_ERROR_BUSY);
    }
    ReturnErrorOnFailure(command == Command::End ? mRequestor.GetSessionInfoForEnd(mHandle, mInfo)
                                                 : mRequestor.GetSessionInfo(mHandle, mInfo));
    return ValidateTransport(mInfo, session);
}

template <class Request>
CHIP_ERROR CameraProviderClient::Send(Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                                      const Request & request, Command command)
{
    mSender = std::make_unique<app::CommandSender>(static_cast<app::CommandSender::ExtendableCallback *>(this), &exchanges, false,
                                                   false, true);
    VerifyOrReturnError(mSender != nullptr, CHIP_ERROR_NO_MEMORY);
    mCommand          = command;
    mError            = CHIP_NO_ERROR;
    mResponseReceived = false;
    const app::CommandPathParams path{ mInfo.metadata.peerEndpointID, Id, Request::GetCommandId(),
                                       app::CommandPathFlags::kEndpointIdValid };
    CHIP_ERROR error = mSender->AddRequestData(path, request);
    if (error == CHIP_NO_ERROR)
    {
        error = mSender->SendCommandRequest(session, MakeOptional<System::Clock::Timeout>(System::Clock::Seconds16(30)));
    }
    if (error != CHIP_NO_ERROR)
    {
        mSender.reset();
    }
    return error;
}

CHIP_ERROR CameraProviderClient::SendOffer(Messaging::ExchangeManager & exchanges, const SessionHandle & session, CharSpan sdp)
{
    return SendOffer(exchanges, session, sdp, {});
}

CHIP_ERROR CameraProviderClient::ValidateIceServers(Span<const IceServer> iceServers)
{
    VerifyOrReturnError(iceServers.size() <= kMaxIceServers, CHIP_IM_GLOBAL_STATUS(ConstraintError));
    for (const auto & server : iceServers)
    {
        VerifyOrReturnError(server.URLs.size() > 0 && server.URLs.size() <= kMaxIceUrls, CHIP_IM_GLOBAL_STATUS(ConstraintError));
        for (const auto & url : server.URLs)
        {
            VerifyOrReturnError(CameraSignalingCluster::IsValidSignalingText(url, kMaxIceUrlBytes),
                                CHIP_IM_GLOBAL_STATUS(ConstraintError));
            const bool supported = (url.size() > 5 && memcmp(url.data(), "stun:", 5) == 0) ||
                (url.size() > 6 && memcmp(url.data(), "stuns:", 6) == 0) ||
                (url.size() > 5 && memcmp(url.data(), "turn:", 5) == 0) || (url.size() > 6 && memcmp(url.data(), "turns:", 6) == 0);
            VerifyOrReturnError(supported, CHIP_IM_GLOBAL_STATUS(ConstraintError));
            for (const char character : url)
            {
                VerifyOrReturnError(static_cast<unsigned char>(character) > 0x20 && character != 0x7f,
                                    CHIP_IM_GLOBAL_STATUS(ConstraintError));
            }
        }
        for (const auto * field : { &server.username, &server.credential })
        {
            VerifyOrReturnError(!field->HasValue() || field->Value().empty() ||
                                    CameraSignalingCluster::IsValidSignalingText(field->Value(), kMaxIceCredentialBytes),
                                CHIP_IM_GLOBAL_STATUS(ConstraintError));
        }
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraProviderClient::SendOffer(Messaging::ExchangeManager & exchanges, const SessionHandle & session, CharSpan sdp,
                                           Span<const IceServer> iceServers)
{
    ReturnErrorOnFailure(ValidateIceServers(iceServers));
    ReturnErrorOnFailure(Prepare(session, Command::Offer));
    Commands::ProvideOffer::Type request;
    ReturnErrorOnFailure(BuildOffer(mInfo, mRequestor.GetPaths()[0].mEndpointId, sdp, iceServers, request));
    return Send(exchanges, session, request, Command::Offer);
}

CHIP_ERROR CameraProviderClient::BuildOffer(const CameraSignalingCluster::SessionInfo & info, EndpointId requestorEndpoint,
                                            CharSpan sdp, Span<const IceServer> iceServers, Commands::ProvideOffer::Type & request)
{
    ReturnErrorOnFailure(ValidateIceServers(iceServers));
    VerifyOrReturnError(!info.responseReceived && !info.protocolId.has_value(), CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(requestorEndpoint != kInvalidEndpointId && info.videoSelection.IsValid() && info.audioSelection.IsValid() &&
                            (info.videoSelection.GetKind() != CameraSignalingCluster::StreamSelection::Kind::Absent ||
                             info.audioSelection.GetKind() != CameraSignalingCluster::StreamSelection::Kind::Absent),
                        CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(CameraSignalingCluster::IsValidSignalingText(sdp, CameraSignalingCluster::kMaxSdpBytes),
                        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    request = Commands::ProvideOffer::Type{};
    request.webRTCSessionID.SetNull();
    request.sdp                   = sdp;
    request.streamUsage           = info.metadata.streamUsage;
    request.originatingEndpointID = requestorEndpoint;
    request.videoStreamID         = info.videoSelection.AsProtocolField();
    request.audioStreamID         = info.audioSelection.AsProtocolField();
    if (!iceServers.empty())
    {
        request.ICEServers.SetValue(app::DataModel::List<const IceServer>(iceServers.data(), iceServers.size()));
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraProviderClient::SendCandidates(Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                                                Span<const CameraSignalingCluster::Candidate> candidates)
{
    ReturnErrorOnFailure(Prepare(session, Command::Candidates));
    VerifyOrReturnError(mInfo.responseReceived && mInfo.protocolId.has_value(), CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(!candidates.empty() && candidates.size() <= CameraSignalingCluster::kMaxCandidates,
                        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    std::array<CandidateType, CameraSignalingCluster::kMaxCandidates> values;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto & source = candidates[i];
        auto & value        = values[i];
        value.candidate     = CharSpan(source.candidate.data(), source.candidate.size());
        VerifyOrReturnError(
            CameraSignalingCluster::IsValidSignalingText(value.candidate, CameraSignalingCluster::kMaxCandidateBytes),
            CHIP_IM_GLOBAL_STATUS(ConstraintError));
        if (source.mid.has_value())
        {
            CharSpan mid(source.mid->data(), source.mid->size());
            VerifyOrReturnError(CameraSignalingCluster::IsValidSignalingText(mid, CameraSignalingCluster::kMaxMidBytes),
                                CHIP_IM_GLOBAL_STATUS(ConstraintError));
            value.SDPMid.SetNonNull(mid);
        }
        if (source.lineIndex.has_value())
        {
            value.SDPMLineIndex.SetNonNull(*source.lineIndex);
            VerifyOrReturnError(value.SDPMLineIndex.ExistingValueInEncodableRange(), CHIP_IM_GLOBAL_STATUS(ConstraintError));
        }
    }
    Commands::ProvideICECandidates::Type request;
    request.webRTCSessionID = *mInfo.protocolId;
    request.ICECandidates   = app::DataModel::List<const CandidateType>(values.data(), candidates.size());
    // AddRequestData encodes spans synchronously into the sender-owned buffer.
    return Send(exchanges, session, request, Command::Candidates);
}

CHIP_ERROR CameraProviderClient::SendEnd(Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                                         app::Clusters::Globals::WebRTCEndReasonEnum reason)
{
    ReturnErrorOnFailure(Prepare(session, Command::End));
    VerifyOrReturnError(mInfo.protocolId.has_value(), CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(app::Clusters::EnsureKnownEnumValue(reason) !=
                            app::Clusters::Globals::WebRTCEndReasonEnum::kUnknownEnumValue,
                        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    Commands::EndSession::Type request;
    request.webRTCSessionID = *mInfo.protocolId;
    request.reason          = reason;
    return Send(exchanges, session, request, Command::End);
}

CHIP_ERROR CameraProviderClient::ApplyOfferResponse(CameraSignalingCluster & requestor, uint64_t handle, TLV::TLVReader & data)
{
    CameraSignalingCluster::SessionInfo info;
    ReturnErrorOnFailure(requestor.GetSessionInfo(handle, info));
    VerifyOrReturnError(!info.responseReceived, CHIP_ERROR_INCORRECT_STATE);
    Commands::ProvideOfferResponse::DecodableType response;
    ReturnErrorOnFailure(app::DataModel::Decode(data, response));
    return requestor.BindSession(handle, response.webRTCSessionID, response.videoStreamID, response.audioStreamID);
}

void CameraProviderClient::OnResponse(app::CommandSender *, const app::CommandSender::ResponseData & response)
{
    if (mResponseReceived || mError != CHIP_NO_ERROR)
    {
        mError = CHIP_ERROR_INCORRECT_STATE;
        return;
    }
    mResponseReceived            = true;
    const CHIP_ERROR status      = response.statusIB.ToChipError();
    const CommandId expected     = mCommand == Command::Offer ? Commands::ProvideOfferResponse::Id
            : mCommand == Command::Candidates                 ? Commands::ProvideICECandidates::Id
                                                              : Commands::EndSession::Id;
    const CommandId expectedPath = mCommand == Command::Offer && status != CHIP_NO_ERROR ? Commands::ProvideOffer::Id : expected;
    if (response.path.mEndpointId != mInfo.metadata.peerEndpointID || response.path.mClusterId != Id ||
        response.path.mCommandId != expectedPath)
    {
        mError = CHIP_ERROR_INVALID_ARGUMENT;
        return;
    }
    mError = status;
    if (mError != CHIP_NO_ERROR)
    {
        return;
    }
    if (mCommand == Command::Offer)
    {
        mError = response.data != nullptr ? ApplyOfferResponse(mRequestor, mHandle, *response.data) : CHIP_ERROR_INVALID_ARGUMENT;
    }
    else if (response.data != nullptr)
    {
        mError = CHIP_ERROR_INVALID_ARGUMENT;
    }
}

void CameraProviderClient::OnError(const app::CommandSender *, const app::CommandSender::ErrorData & error)
{
    mError = error.error;
}

void CameraProviderClient::OnNoResponse(app::CommandSender *, const app::CommandSender::NoResponseData &)
{
    mError = CHIP_ERROR_TIMEOUT;
}

void CameraProviderClient::OnDone(app::CommandSender *)
{
    const CHIP_ERROR error = mError != CHIP_NO_ERROR ? mError : (mResponseReceived ? CHIP_NO_ERROR : CHIP_ERROR_TIMEOUT);
    mSender.reset();
    Finish(error);
}

void CameraProviderClient::Finish(CHIP_ERROR error)
{
    if (error != CHIP_NO_ERROR)
    {
        LogErrorOnFailure(mRequestor.FailSession(mHandle, error));
    }
    else if (mCommand == Command::End)
    {
        error = mRequestor.CloseSession(mHandle);
        LogErrorOnFailure(error);
    }
    std::lock_guard<std::mutex> lock(mResultMutex);
    mResult.emplace(Result{ mHandle, mCommand, error });
}

void CameraProviderClient::Cancel()
{
    if (mSender != nullptr)
    {
        mSender.reset();
        Finish(CHIP_ERROR_CANCELLED);
    }
}

std::optional<CameraProviderClient::Result> CameraProviderClient::TakeResult()
{
    std::lock_guard<std::mutex> lock(mResultMutex);
    auto result = std::move(mResult);
    mResult.reset();
    return result;
}

} // namespace chip::Controller
