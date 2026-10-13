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

#include "CameraSignalingCluster.h"

#include <app/server-cluster/AttributeListBuilder.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <algorithm>

namespace chip::Controller {
namespace {
using namespace app;
using namespace app::Clusters;
using Protocols::InteractionModel::Status;

constexpr DataModel::AcceptedCommandEntry kCommands[] = {
    WebRTCTransportRequestor::Commands::Offer::kMetadataEntry,
    WebRTCTransportRequestor::Commands::Answer::kMetadataEntry,
    WebRTCTransportRequestor::Commands::ICECandidates::kMetadataEntry,
    WebRTCTransportRequestor::Commands::End::kMetadataEntry,
};

} // namespace

Optional<app::DataModel::Nullable<uint16_t>> CameraSignalingCluster::StreamSelection::AsProtocolField() const
{
    Optional<app::DataModel::Nullable<uint16_t>> result;
    if (mKind == Kind::AutoSelect)
    {
        result.Emplace().SetNull();
    }
    else if (mKind == Kind::ExplicitId)
    {
        result.Emplace().SetNonNull(mId);
    }
    return result;
}

bool CameraSignalingCluster::StreamSelection::IsValid() const
{
    return mKind != Kind::ExplicitId || app::DataModel::Nullable<uint16_t>(mId).ExistingValueInEncodableRange();
}

bool CameraSignalingCluster::StreamSelection::MatchesResponse(const Optional<app::DataModel::Nullable<uint16_t>> & response) const
{
    if (!response.HasValue())
    {
        return true;
    }
    if (response.Value().IsNull())
    {
        return mKind != Kind::ExplicitId;
    }
    return response.Value().ExistingValueInEncodableRange() &&
        (mKind == Kind::AutoSelect || (mKind == Kind::ExplicitId && response.Value().Value() == mId));
}

bool CameraSignalingCluster::IsValidSignalingText(CharSpan text, size_t limit)
{
    if (text.empty() || text.size() > limit)
    {
        return false;
    }
    return std::find(text.begin(), text.end(), '\0') == text.end();
}

CameraSignalingCluster::~CameraSignalingCluster()
{
    if (mRegistered)
    {
        Shutdown(app::ClusterShutdownType::kClusterShutdown);
    }
}

CHIP_ERROR CameraSignalingCluster::Startup(app::ServerClusterContext & context)
{
    ReturnErrorOnFailure(DefaultServerCluster::Startup(context));
    CHIP_ERROR error = mFabrics.AddFabricDelegate(this);
    if (error != CHIP_NO_ERROR)
    {
        DefaultServerCluster::Shutdown(app::ClusterShutdownType::kClusterShutdown);
        return error;
    }
    mRegistered = true;
    error       = mSystem.StartTimer(System::Clock::Seconds32(1), OnTimer, this);
    if (error != CHIP_NO_ERROR)
    {
        Shutdown(app::ClusterShutdownType::kClusterShutdown);
        return error;
    }
    return CHIP_NO_ERROR;
}

void CameraSignalingCluster::Shutdown(app::ClusterShutdownType type)
{
    if (mRegistered)
    {
        mSystem.CancelTimer(OnTimer, this);
        mFabrics.RemoveFabricDelegate(this);
        mRegistered = false;
    }
    for (auto & session : mSessions)
    {
        if (session.has_value() && !session->closed)
        {
            Fail(*session, CHIP_ERROR_INCORRECT_STATE);
        }
    }
    DefaultServerCluster::Shutdown(type);
}

void CameraSignalingCluster::OnTimer(System::Layer *, void * context)
{
    auto & self = *static_cast<CameraSignalingCluster *>(context);
    self.ExpireSessions(System::SystemClock().GetMonotonicTimestamp());
    if (self.mRegistered)
    {
        CHIP_ERROR error = self.mSystem.StartTimer(System::Clock::Seconds32(1), OnTimer, context);
        if (error != CHIP_NO_ERROR)
        {
            for (auto & session : self.mSessions)
            {
                if (session.has_value())
                {
                    self.Fail(*session, error);
                }
            }
            self.Shutdown(app::ClusterShutdownType::kClusterShutdown);
        }
    }
}

CHIP_ERROR CameraSignalingCluster::CreateSession(ScopedNodeId peer, EndpointId cameraEndpoint,
                                                 const app::DataModel::Nullable<uint16_t> & videoStream,
                                                 const app::DataModel::Nullable<uint16_t> & audioStream, uint64_t & handle)
{
    // Preserve the legacy non-null video requirement and present-null audio semantics.
    VerifyOrReturnError(!videoStream.IsNull(), CHIP_ERROR_INVALID_ARGUMENT);
    return CreateSession(peer, cameraEndpoint, StreamSelection::ExplicitId(videoStream.Value()),
                         audioStream.IsNull() ? StreamSelection::AutoSelect() : StreamSelection::ExplicitId(audioStream.Value()),
                         handle);
}

CHIP_ERROR CameraSignalingCluster::CreateSession(ScopedNodeId peer, EndpointId cameraEndpoint, StreamSelection videoStream,
                                                 StreamSelection audioStream, uint64_t & handle)
{
    VerifyOrReturnError(mRegistered, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(IsOperationalNodeId(peer.GetNodeId()) && peer.GetFabricIndex() != kUndefinedFabricIndex &&
                            cameraEndpoint != kInvalidEndpointId,
                        CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(mFabrics.FindFabricWithIndex(peer.GetFabricIndex()) != nullptr, CHIP_ERROR_INVALID_FABRIC_INDEX);
    VerifyOrReturnError(
        videoStream.IsValid() && audioStream.IsValid() &&
            (videoStream.GetKind() != StreamSelection::Kind::Absent || audioStream.GetKind() != StreamSelection::Kind::Absent),
        CHIP_ERROR_INVALID_ARGUMENT);
    for (const auto & identity : mRetired)
    {
        VerifyOrReturnError(!identity.has_value() || identity->peer != peer || identity->protocolId.has_value(),
                            CHIP_ERROR_DUPLICATE_KEY_ID);
    }

    // Pending commands have no protocol identifier yet. Do not create an
    // ambiguous association when two endpoints on a peer answer early.
    for (const auto & session : mSessions)
    {
        VerifyOrReturnError(!session.has_value() || session->closed || session->responseReceived ||
                                session->metadata.peerNodeID != peer.GetNodeId() ||
                                session->metadata.fabricIndex != peer.GetFabricIndex(),
                            CHIP_ERROR_BUSY);
    }
    VerifyOrReturnError(mNextHandle != 0, CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    for (auto & slot : mSessions)
    {
        if (!slot.has_value())
        {
            slot.emplace();
            auto & session                  = *slot;
            session.handle                  = mNextHandle++;
            session.metadata.peerNodeID     = peer.GetNodeId();
            session.metadata.fabricIndex    = peer.GetFabricIndex();
            session.metadata.peerEndpointID = cameraEndpoint;
            session.metadata.streamUsage    = app::Clusters::Globals::StreamUsageEnum::kLiveView;
            session.videoSelection          = videoStream;
            session.audioSelection          = audioStream;
            const auto videoField           = videoStream.AsProtocolField();
            const auto audioField           = audioStream.AsProtocolField();
            session.metadata.videoStreamID  = videoField.HasValue() ? videoField.Value() : app::DataModel::Nullable<uint16_t>();
            session.metadata.audioStreamID  = audioField.HasValue() ? audioField.Value() : app::DataModel::Nullable<uint16_t>();
            session.deadline                = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(30);
            handle                          = session.handle;
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
}

CameraSignalingCluster::Session * CameraSignalingCluster::FindSession(uint64_t handle)
{
    for (auto & session : mSessions)
    {
        if (session.has_value() && session->handle == handle)
        {
            return &*session;
        }
    }
    return nullptr;
}

bool CameraSignalingCluster::IsRetired(ScopedNodeId peer, uint16_t protocolId) const
{
    for (const auto & identity : mRetired)
    {
        if (identity.has_value() && identity->peer == peer &&
            (!identity->protocolId.has_value() || identity->protocolId == protocolId))
        {
            return true;
        }
    }
    return false;
}

CHIP_ERROR CameraSignalingCluster::BindSession(uint64_t handle, uint16_t protocolId,
                                               const Optional<app::DataModel::Nullable<uint16_t>> & videoStream,
                                               const Optional<app::DataModel::Nullable<uint16_t>> & audioStream)
{
    Session * session = FindSession(handle);
    VerifyOrReturnError(session != nullptr && !session->closed && !session->responseReceived, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(session->videoSelection.MatchesResponse(videoStream) &&
                            session->audioSelection.MatchesResponse(audioStream),
                        CHIP_ERROR_INVALID_ARGUMENT);
    ReturnErrorOnFailure(BindSession(handle, protocolId));
    if (videoStream.HasValue())
    {
        session->metadata.videoStreamID = videoStream.Value();
    }
    if (audioStream.HasValue())
    {
        session->metadata.audioStreamID = audioStream.Value();
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraSignalingCluster::BindSession(uint64_t handle, uint16_t protocolId)
{
    Session * session = FindSession(handle);
    VerifyOrReturnError(session != nullptr && !session->closed, CHIP_ERROR_INCORRECT_STATE);
    ScopedNodeId peer(session->metadata.peerNodeID, session->metadata.fabricIndex);
    VerifyOrReturnError(!IsRetired(peer, protocolId), CHIP_ERROR_DUPLICATE_KEY_ID);
    for (const auto & other : mSessions)
    {
        VerifyOrReturnError(!other.has_value() || other->handle == handle || other->closed ||
                                other->metadata.peerNodeID != peer.GetNodeId() ||
                                other->metadata.fabricIndex != peer.GetFabricIndex() || other->protocolId != protocolId,
                            CHIP_ERROR_DUPLICATE_KEY_ID);
    }
    if (session->protocolId.has_value() && session->protocolId != protocolId)
    {
        // Both the early callback identity and the contradictory response are
        // suspect. Retire the peer, not just one of the two protocol IDs.
        session->protocolId.reset();
        Fail(*session, CHIP_ERROR_INVALID_ARGUMENT);
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    session->protocolId       = protocolId;
    session->metadata.id      = protocolId;
    session->responseReceived = true;
    NotifyAttributeChanged(app::Clusters::WebRTCTransportRequestor::Attributes::CurrentSessions::Id);
    return CHIP_NO_ERROR;
}

CameraSignalingCluster::Session * CameraSignalingCluster::FindIncoming(const Access::SubjectDescriptor & subject,
                                                                       uint16_t protocolId)
{
    if (subject.authMode != Access::AuthMode::kCase || !IsOperationalNodeId(subject.subject) || subject.isCommissioning ||
        IsRetired(ScopedNodeId(subject.subject, subject.fabricIndex), protocolId))
    {
        return nullptr;
    }
    Session * pending = nullptr;
    for (auto & slot : mSessions)
    {
        if (!slot.has_value() || slot->closed || slot->metadata.peerNodeID != subject.subject ||
            slot->metadata.fabricIndex != subject.fabricIndex)
        {
            continue;
        }
        if (slot->protocolId == protocolId)
        {
            return &*slot;
        }
        if (!slot->protocolId.has_value())
        {
            pending = &*slot;
        }
    }
    return pending;
}

bool CameraSignalingCluster::IsNegotiated(uint64_t handle) const
{
    for (const auto & session : mSessions)
    {
        if (session.has_value() && session->handle == handle)
        {
            return !session->closed && session->responseReceived && session->answerReceived;
        }
    }
    return false;
}

CHIP_ERROR CameraSignalingCluster::GetSessionInfo(uint64_t handle, SessionInfo & info) const
{
    return ReadSessionInfo(handle, info, false);
}

CHIP_ERROR CameraSignalingCluster::GetSessionState(uint64_t handle, SessionState & state) const
{
    for (const auto & session : mSessions)
    {
        if (session.has_value() && session->handle == handle)
        {
            state = session->failure != CHIP_NO_ERROR ? SessionState::Failed
                : session->closed                     ? SessionState::Closed
                : IsNegotiated(handle)                ? SessionState::Established
                                                      : SessionState::Negotiating;
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR CameraSignalingCluster::GetSessionInfoForEnd(uint64_t handle, SessionInfo & info) const
{
    return ReadSessionInfo(handle, info, true);
}

CHIP_ERROR CameraSignalingCluster::ReadSessionInfo(uint64_t handle, SessionInfo & info, bool forEnd) const
{
    VerifyOrReturnError(mRegistered, CHIP_ERROR_INCORRECT_STATE);
    for (const auto & session : mSessions)
    {
        if (session.has_value() && session->handle == handle && (!session->closed || (forEnd && session->failure != CHIP_NO_ERROR)))
        {
            info.metadata         = session->metadata;
            info.protocolId       = session->protocolId;
            info.responseReceived = session->responseReceived;
            info.videoSelection   = session->videoSelection;
            info.audioSelection   = session->audioSelection;
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_NOT_FOUND;
}

CHIP_ERROR CameraSignalingCluster::FailSession(uint64_t handle, CHIP_ERROR error)
{
    VerifyOrReturnError(error != CHIP_NO_ERROR, CHIP_ERROR_INVALID_ARGUMENT);
    Session * session = FindSession(handle);
    VerifyOrReturnError(session != nullptr, CHIP_ERROR_NOT_FOUND);
    Fail(*session, error);
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraSignalingCluster::CheckAccess(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                                               Access::Privilege privilege) const
{
    VerifyOrReturnError(mRegistered && path.endpoint == mPath.mEndpointId && path.cluster == mPath.mClusterId &&
                            subject.authMode == Access::AuthMode::kCase && !subject.isCommissioning &&
                            (privilege == Access::Privilege::kView || privilege == Access::Privilege::kOperate),
                        CHIP_ERROR_ACCESS_DENIED);
    if (privilege == Access::Privilege::kOperate)
    {
        using namespace app::Clusters::WebRTCTransportRequestor;
        VerifyOrReturnError(path.requestType == Access::RequestType::kCommandInvokeRequest && path.entityId.has_value() &&
                                (*path.entityId == Commands::Answer::Id || *path.entityId == Commands::ICECandidates::Id ||
                                 *path.entityId == Commands::End::Id),
                            CHIP_ERROR_ACCESS_DENIED);
    }
    else
    {
        VerifyOrReturnError(path.requestType == Access::RequestType::kAttributeReadRequest, CHIP_ERROR_ACCESS_DENIED);
    }
    for (const auto & session : mSessions)
    {
        if (session.has_value() && !session->closed && session->metadata.peerNodeID == subject.subject &&
            session->metadata.fabricIndex == subject.fabricIndex)
        {
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_ACCESS_DENIED;
}

CHIP_ERROR CameraSignalingCluster::Retire(Session & session)
{
    if (session.closed)
    {
        return CHIP_NO_ERROR;
    }
    session.closed = true;
    NotifyAttributeChanged(app::Clusters::WebRTCTransportRequestor::Attributes::CurrentSessions::Id);
    ScopedNodeId peer(session.metadata.peerNodeID, session.metadata.fabricIndex);
    if (session.protocolId.has_value() && IsRetired(peer, *session.protocolId))
    {
        return CHIP_NO_ERROR;
    }
    for (auto & identity : mRetired)
    {
        if (!identity.has_value())
        {
            identity.emplace(RetiredIdentity{ peer, session.protocolId });
            return CHIP_NO_ERROR;
        }
    }
    // Fail closed if we cannot remember an identity. The caller may shut down
    // this endpoint and create a new one; never erase tombstones to make room.
    Shutdown(app::ClusterShutdownType::kClusterShutdown);
    return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
}

void CameraSignalingCluster::ClearSignals(uint64_t handle)
{
    std::lock_guard<std::mutex> lock(mSignalMutex);
    size_t kept = 0;
    for (size_t i = 0; i < mSignalCount; ++i)
    {
        const size_t source = (mSignalHead + i) % mSignals.size();
        auto & signal       = mSignals[source];
        if (!signal.has_value() || signal->handle == handle)
        {
            signal.reset();
            continue;
        }
        const size_t destination = (mSignalHead + kept) % mSignals.size();
        if (destination != source)
        {
            mSignals[destination] = std::move(signal);
            signal.reset();
        }
        ++kept;
    }
    mSignalCount = kept;
    for (auto & signal : mFailures)
    {
        if (signal.has_value() && signal->handle == handle)
        {
            signal.reset();
        }
    }
}

CHIP_ERROR CameraSignalingCluster::CloseSession(uint64_t handle)
{
    Session * session = FindSession(handle);
    if (session == nullptr)
    {
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(Retire(*session));
    ClearSignals(handle);
    for (auto & slot : mSessions)
    {
        if (slot.has_value() && slot->handle == handle)
        {
            slot.reset();
            break;
        }
    }
    return CHIP_NO_ERROR;
}

void CameraSignalingCluster::Fail(Session & session, CHIP_ERROR error)
{
    if (session.closed)
    {
        return;
    }
    session.failure = error;
    LogErrorOnFailure(Retire(session));
    ClearSignals(session.handle);
    {
        std::lock_guard<std::mutex> lock(mSignalMutex);
        for (size_t i = 0; i < mSessions.size(); ++i)
        {
            if (mSessions[i].has_value() && &*mSessions[i] == &session)
            {
                Signal signal;
                signal.handle         = session.handle;
                signal.peer           = ScopedNodeId(session.metadata.peerNodeID, session.metadata.fabricIndex);
                signal.cameraEndpoint = session.metadata.peerEndpointID;
                signal.protocolId     = session.protocolId;
                signal.type           = SignalType::Failure;
                signal.error          = error;
                mFailures[i].emplace(std::move(signal));
                break;
            }
        }
    }
    ChipLogError(Camera, "Camera signaling session failed: %" CHIP_ERROR_FORMAT, error.Format());
}

CHIP_ERROR CameraSignalingCluster::Queue(Session & session, Signal && signal)
{
    {
        std::lock_guard<std::mutex> lock(mSignalMutex);
        // Leave failure delivery separate from the bounded media-signaling queue.
        if (mSignalCount < mSignals.size())
        {
            const size_t tail     = (mSignalHead + mSignalCount) % mSignals.size();
            signal.handle         = session.handle;
            signal.peer           = ScopedNodeId(session.metadata.peerNodeID, session.metadata.fabricIndex);
            signal.cameraEndpoint = session.metadata.peerEndpointID;
            signal.protocolId     = session.protocolId;
            mSignals[tail].emplace(std::move(signal));
            ++mSignalCount;
            return CHIP_NO_ERROR;
        }
    }
    Fail(session, CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
}

std::optional<CameraSignalingCluster::Signal> CameraSignalingCluster::TakeSignal()
{
    // This operation only takes the queue mutex. Stack-owned session data is
    // not accessed here; failure notifications are queued by the producer.
    std::lock_guard<std::mutex> lock(mSignalMutex);
    for (auto & failure : mFailures)
    {
        if (failure.has_value())
        {
            auto signal = std::move(failure);
            failure.reset();
            return signal;
        }
    }
    while (mSignalCount != 0)
    {
        auto signal = std::move(mSignals[mSignalHead]);
        mSignals[mSignalHead].reset();
        mSignalHead = (mSignalHead + 1) % mSignals.size();
        --mSignalCount;
        if (signal.has_value())
        {
            return signal;
        }
    }
    return std::nullopt;
}

void CameraSignalingCluster::ExpireSessions(System::Clock::Timestamp now)
{
    for (auto & session : mSessions)
    {
        if (session.has_value() && !session->closed && !(session->responseReceived && session->answerReceived) &&
            now >= session->deadline)
        {
            Fail(*session, CHIP_ERROR_TIMEOUT);
        }
    }
}

void CameraSignalingCluster::OnFabricRemoved(const FabricTable &, FabricIndex fabric)
{
    for (auto & session : mSessions)
    {
        if (session.has_value() && !session->closed && session->metadata.fabricIndex == fabric)
        {
            Fail(*session, CHIP_ERROR_INVALID_FABRIC_INDEX);
        }
    }
}

app::DataModel::ActionReturnStatus CameraSignalingCluster::ReadAttribute(const app::DataModel::ReadAttributeRequest & request,
                                                                         app::AttributeValueEncoder & encoder)
{
    using namespace app::Clusters;
    switch (request.path.mAttributeId)
    {
    case WebRTCTransportRequestor::Attributes::CurrentSessions::Id:
        return encoder.EncodeList([this](const auto & list) -> CHIP_ERROR {
            for (const auto & session : mSessions)
            {
                if (session.has_value() && !session->closed && session->protocolId.has_value())
                {
                    ReturnErrorOnFailure(list.Encode(session->metadata));
                }
            }
            return CHIP_NO_ERROR;
        });
    case Globals::Attributes::ClusterRevision::Id:
        return encoder.Encode(WebRTCTransportRequestor::kRevision);
    case Globals::Attributes::FeatureMap::Id:
        return encoder.Encode<uint32_t>(0);
    default:
        return Status::UnsupportedAttribute;
    }
}

CHIP_ERROR CameraSignalingCluster::AcceptedCommands(const app::ConcreteClusterPath &,
                                                    ReadOnlyBufferBuilder<app::DataModel::AcceptedCommandEntry> & builder)
{
    return builder.ReferenceExisting(kCommands);
}

CHIP_ERROR CameraSignalingCluster::Attributes(const app::ConcreteClusterPath &,
                                              ReadOnlyBufferBuilder<app::DataModel::AttributeEntry> & builder)
{
    app::AttributeListBuilder list(builder);
    return list.Append(Span(app::Clusters::WebRTCTransportRequestor::Attributes::kMandatoryMetadata), {}, {});
}

std::optional<app::DataModel::ActionReturnStatus>
CameraSignalingCluster::InvokeCommand(const app::DataModel::InvokeRequest & request, TLV::TLVReader & arguments,
                                      app::CommandHandler *)
{
    using namespace app::Clusters::WebRTCTransportRequestor;
    VerifyOrReturnError(mRegistered, Status::InvalidInState);
    const auto & subject = request.subjectDescriptor;
    VerifyOrReturnError(subject.authMode == Access::AuthMode::kCase && IsOperationalNodeId(subject.subject) &&
                            !subject.isCommissioning && mFabrics.FindFabricWithIndex(subject.fabricIndex) != nullptr,
                        Status::UnsupportedAccess);
    ExpireSessions(System::SystemClock().GetMonotonicTimestamp());
    Signal signal;
    uint16_t protocolId = 0;
    switch (request.path.mCommandId)
    {
    case Commands::Offer::Id:
        return Status::UnsupportedCommand;
    case Commands::Answer::Id: {
        Commands::Answer::DecodableType decoded;
        VerifyOrReturnError(app::DataModel::Decode(arguments, decoded) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(IsValidSignalingText(decoded.sdp, kMaxSdpBytes), Status::ConstraintError);
        protocolId  = decoded.webRTCSessionID;
        signal.type = SignalType::Answer;
        signal.sdp.assign(decoded.sdp.data(), decoded.sdp.size());
        break;
    }
    case Commands::ICECandidates::Id: {
        Commands::ICECandidates::DecodableType decoded;
        VerifyOrReturnError(app::DataModel::Decode(arguments, decoded) == CHIP_NO_ERROR, Status::InvalidCommand);
        protocolId    = decoded.webRTCSessionID;
        signal.type   = SignalType::Candidates;
        auto iterator = decoded.ICECandidates.begin();
        while (iterator.Next())
        {
            const auto & candidate = iterator.GetValue();
            VerifyOrReturnError(signal.candidates.size() < kMaxCandidates &&
                                    IsValidSignalingText(candidate.candidate, kMaxCandidateBytes),
                                Status::ConstraintError);
            Candidate owned;
            owned.candidate.assign(candidate.candidate.data(), candidate.candidate.size());
            if (!candidate.SDPMid.IsNull())
            {
                VerifyOrReturnError(IsValidSignalingText(candidate.SDPMid.Value(), kMaxMidBytes), Status::ConstraintError);
                owned.mid.emplace(candidate.SDPMid.Value().data(), candidate.SDPMid.Value().size());
            }
            if (!candidate.SDPMLineIndex.IsNull())
            {
                owned.lineIndex = candidate.SDPMLineIndex.Value();
            }
            signal.candidates.push_back(std::move(owned));
        }
        VerifyOrReturnError(iterator.GetStatus() == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(!signal.candidates.empty(), Status::ConstraintError);
        break;
    }
    case Commands::End::Id: {
        Commands::End::DecodableType decoded;
        VerifyOrReturnError(app::DataModel::Decode(arguments, decoded) == CHIP_NO_ERROR, Status::InvalidCommand);
        VerifyOrReturnError(decoded.reason != app::Clusters::Globals::WebRTCEndReasonEnum::kUnknownEnumValue,
                            Status::ConstraintError);
        protocolId    = decoded.webRTCSessionID;
        signal.type   = SignalType::End;
        signal.reason = decoded.reason;
        break;
    }
    default:
        return Status::UnsupportedCommand;
    }
    Session * session = FindIncoming(request.subjectDescriptor, protocolId);
    VerifyOrReturnError(session != nullptr, Status::UnsupportedAccess);
    if (signal.type == SignalType::Answer && session->answerReceived)
    {
        return Status::InvalidInState;
    }
    if (!session->protocolId.has_value())
    {
        session->protocolId  = protocolId;
        session->metadata.id = protocolId;
    }
    const SignalType type = signal.type;
    ReturnErrorOnFailure(Queue(*session, std::move(signal)));
    if (type == SignalType::Answer)
    {
        session->answerReceived = true;
    }
    if (type == SignalType::End)
    {
        ReturnErrorOnFailure(Retire(*session));
    }
    return CHIP_NO_ERROR;
}

} // namespace chip::Controller
