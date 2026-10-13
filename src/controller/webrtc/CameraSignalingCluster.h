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
#pragma once

#include <access/RequestPath.h>
#include <app/server-cluster/DefaultServerCluster.h>
#include <clusters/WebRTCTransportRequestor/Commands.h>
#include <clusters/WebRTCTransportRequestor/Metadata.h>
#include <credentials/FabricTable.h>
#include <lib/core/ScopedNodeId.h>
#include <system/SystemClock.h>
#include <system/SystemLayer.h>

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chip::Controller {

/// Client-offer signaling only. Stack-facing methods require the Matter stack lock.
/// TakeSignal is thread-safe and never calls application code from the stack.
class CameraSignalingCluster : public app::DefaultServerCluster, private FabricTable::Delegate
{
public:
    static constexpr size_t kMaxSessions       = 8;
    static constexpr size_t kMaxSignals        = 32;
    static constexpr size_t kMaxSdpBytes       = 16384;
    static constexpr size_t kMaxCandidates     = 16;
    static constexpr size_t kMaxCandidateBytes = 2048;
    static constexpr size_t kMaxMidBytes       = 256;

    static bool IsValidSignalingText(CharSpan text, size_t limit);

    enum class SignalType : uint8_t
    {
        Answer,
        Candidates,
        End,
        Failure,
    };

    enum class SessionState : uint8_t
    {
        Negotiating,
        Established,
        Closed,
        Failed,
    };

    class StreamSelection
    {
    public:
        enum class Kind : uint8_t
        {
            Absent,
            AutoSelect,
            ExplicitId,
        };

        StreamSelection() = default;
        static StreamSelection Absent() { return {}; }
        static StreamSelection AutoSelect() { return StreamSelection(Kind::AutoSelect, 0); }
        static StreamSelection ExplicitId(uint16_t id) { return StreamSelection(Kind::ExplicitId, id); }
        Kind GetKind() const { return mKind; }
        Optional<app::DataModel::Nullable<uint16_t>> AsProtocolField() const;
        bool IsValid() const;
        bool MatchesResponse(const Optional<app::DataModel::Nullable<uint16_t>> & response) const;

    private:
        StreamSelection(Kind kind, uint16_t id) : mKind(kind), mId(id) {}
        Kind mKind   = Kind::Absent;
        uint16_t mId = 0;
    };

    struct Candidate
    {
        std::string candidate;
        std::optional<std::string> mid;
        std::optional<uint16_t> lineIndex;
    };

    struct Signal
    {
        uint64_t handle = 0;
        ScopedNodeId peer;
        EndpointId cameraEndpoint = kInvalidEndpointId;
        std::optional<uint16_t> protocolId;
        SignalType type = SignalType::Failure;
        std::string sdp;
        std::vector<Candidate> candidates;
        app::Clusters::Globals::WebRTCEndReasonEnum reason = app::Clusters::Globals::WebRTCEndReasonEnum::kUnknownEnumValue;
        CHIP_ERROR error                                   = CHIP_NO_ERROR;
    };

    struct SessionInfo
    {
        app::Clusters::Globals::Structs::WebRTCSessionStruct::Type metadata;
        std::optional<uint16_t> protocolId;
        bool responseReceived = false;
        StreamSelection videoSelection;
        StreamSelection audioSelection;
    };

    CameraSignalingCluster(EndpointId endpoint, FabricTable & fabrics, System::Layer & system) :
        DefaultServerCluster({ endpoint, app::Clusters::WebRTCTransportRequestor::Id }), mFabrics(fabrics), mSystem(system)
    {}
    ~CameraSignalingCluster() override;

    CHIP_ERROR Startup(app::ServerClusterContext & context) override;
    void Shutdown(app::ClusterShutdownType type) override;
    app::DataModel::ActionReturnStatus ReadAttribute(const app::DataModel::ReadAttributeRequest & request,
                                                     app::AttributeValueEncoder & encoder) override;
    std::optional<app::DataModel::ActionReturnStatus> InvokeCommand(const app::DataModel::InvokeRequest & request,
                                                                    TLV::TLVReader & arguments,
                                                                    app::CommandHandler * handler) override;
    CHIP_ERROR AcceptedCommands(const app::ConcreteClusterPath & path,
                                ReadOnlyBufferBuilder<app::DataModel::AcceptedCommandEntry> & builder) override;
    CHIP_ERROR Attributes(const app::ConcreteClusterPath & path,
                          ReadOnlyBufferBuilder<app::DataModel::AttributeEntry> & builder) override;

    /// Allocate before ProvideOffer, so an early Answer can be correlated.
    CHIP_ERROR CreateSession(ScopedNodeId peer, EndpointId cameraEndpoint, const app::DataModel::Nullable<uint16_t> & videoStream,
                             const app::DataModel::Nullable<uint16_t> & audioStream, uint64_t & handle);
    /// Unlike the legacy overload, Absent omits a field and AutoSelect sends protocol null.
    CHIP_ERROR CreateSession(ScopedNodeId peer, EndpointId cameraEndpoint, StreamSelection videoStream, StreamSelection audioStream,
                             uint64_t & handle);
    CHIP_ERROR BindSession(uint64_t handle, uint16_t protocolId);
    CHIP_ERROR BindSession(uint64_t handle, uint16_t protocolId, const Optional<app::DataModel::Nullable<uint16_t>> & videoStream,
                           const Optional<app::DataModel::Nullable<uint16_t>> & audioStream);
    CHIP_ERROR GetSessionInfo(uint64_t handle, SessionInfo & info) const;
    /// Retain failed-session identity for a bounded End attempt without reopening
    /// inbound authorization. Remotely ended and released sessions are excluded.
    CHIP_ERROR GetSessionInfoForEnd(uint64_t handle, SessionInfo & info) const;
    CHIP_ERROR FailSession(uint64_t handle, CHIP_ERROR error);
    CHIP_ERROR CloseSession(uint64_t handle);
    /// Complete negotiation only after the provider response and Answer are both received.
    bool IsNegotiated(uint64_t handle) const;
    CHIP_ERROR GetSessionState(uint64_t handle, SessionState & state) const;
    void ExpireSessions(System::Clock::Timestamp now);
    [[nodiscard]] std::optional<Signal> TakeSignal();
    /// Call only for this owned cluster from the application's existing ACL
    /// delegate; unrelated paths must continue through its original policy.
    CHIP_ERROR CheckAccess(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                           Access::Privilege privilege) const;

private:
    struct Session
    {
        uint64_t handle = 0;
        app::Clusters::Globals::Structs::WebRTCSessionStruct::Type metadata;
        std::optional<uint16_t> protocolId;
        bool responseReceived = false;
        bool answerReceived   = false;
        bool closed           = false;
        CHIP_ERROR failure    = CHIP_NO_ERROR;
        System::Clock::Timestamp deadline;
        StreamSelection videoSelection;
        StreamSelection audioSelection;
    };

    // Retired identities are never reused on this requestor endpoint. Once the
    // table fills, callers must register a fresh endpoint rather than accepting
    // a stale message for a new session with the same on-wire identity.
    struct RetiredIdentity
    {
        ScopedNodeId peer;
        std::optional<uint16_t> protocolId;
    };
    std::array<std::optional<Session>, kMaxSessions> mSessions;
    std::array<std::optional<RetiredIdentity>, 128> mRetired;
    uint64_t mNextHandle = 1;
    FabricTable & mFabrics;
    System::Layer & mSystem;
    bool mRegistered = false;
    std::mutex mSignalMutex;
    std::array<std::optional<Signal>, kMaxSignals> mSignals;
    std::array<std::optional<Signal>, kMaxSessions> mFailures;
    size_t mSignalHead  = 0;
    size_t mSignalCount = 0;

    Session * FindSession(uint64_t handle);
    CHIP_ERROR ReadSessionInfo(uint64_t handle, SessionInfo & info, bool forEnd) const;
    Session * FindIncoming(const Access::SubjectDescriptor & subject, uint16_t protocolId);
    bool IsRetired(ScopedNodeId peer, uint16_t protocolId) const;
    CHIP_ERROR Retire(Session & session);
    CHIP_ERROR Queue(Session & session, Signal && signal);
    void Fail(Session & session, CHIP_ERROR error);
    void ClearSignals(uint64_t handle);
    void OnFabricRemoved(const FabricTable & fabrics, FabricIndex fabric) override;
    static void OnTimer(System::Layer * system, void * context);
};

} // namespace chip::Controller
