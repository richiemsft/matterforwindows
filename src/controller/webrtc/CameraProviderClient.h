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

#include <app/CommandSender.h>
#include <clusters/WebRTCTransportProvider/Commands.h>
#include <controller/webrtc/CameraSignalingCluster.h>

#include <memory>
#include <mutex>
#include <optional>

namespace chip::Controller {

/// One client per local session. Stack-facing calls and destruction require the
/// Matter stack lock; TakeResult only takes the result mutex. The requestor must
/// outlive this client. Callers establish CASE with kLargePayload before sending.
class CameraProviderClient : private app::CommandSender::ExtendableCallback
{
public:
    static constexpr size_t kMaxIceServers         = 4;
    static constexpr size_t kMaxIceUrls            = 4;
    static constexpr size_t kMaxIceUrlBytes        = 512;
    static constexpr size_t kMaxIceCredentialBytes = 256;
    using IceServer                                = app::Clusters::Globals::Structs::ICEServerStruct::Type;
    enum class Command : uint8_t
    {
        Offer,
        Candidates,
        End,
    };

    struct Result
    {
        uint64_t handle;
        Command command;
        CHIP_ERROR error;
    };

    CameraProviderClient(CameraSignalingCluster & requestor, uint64_t handle) : mRequestor(requestor), mHandle(handle) {}
    ~CameraProviderClient() override;

    CHIP_ERROR SendOffer(Messaging::ExchangeManager & exchanges, const SessionHandle & session, CharSpan sdp);
    CHIP_ERROR SendOffer(Messaging::ExchangeManager & exchanges, const SessionHandle & session, CharSpan sdp,
                         Span<const IceServer> iceServers);
    static CHIP_ERROR ValidateIceServers(Span<const IceServer> iceServers);
    CHIP_ERROR SendCandidates(Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                              Span<const CameraSignalingCluster::Candidate> candidates);
    CHIP_ERROR SendEnd(Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                       app::Clusters::Globals::WebRTCEndReasonEnum reason);
    void Cancel();
    [[nodiscard]] std::optional<Result> TakeResult();

    /// Validate a connected transport before allocating a command buffer.
    static CHIP_ERROR ValidateTransport(const CameraSignalingCluster::SessionInfo & info, const SessionHandle & session);
    /// Validate and bind a provider response without exposing TLV to consumers.
    static CHIP_ERROR ApplyOfferResponse(CameraSignalingCluster & requestor, uint64_t handle, TLV::TLVReader & data);

private:
    CameraSignalingCluster & mRequestor;
    const uint64_t mHandle;
    std::unique_ptr<app::CommandSender> mSender;
    CameraSignalingCluster::SessionInfo mInfo;
    Command mCommand       = Command::Offer;
    CHIP_ERROR mError      = CHIP_NO_ERROR;
    bool mResponseReceived = false;
    std::mutex mResultMutex;
    std::optional<Result> mResult;

    CHIP_ERROR Prepare(const SessionHandle & session, Command command);
    template <class Request>
    CHIP_ERROR Send(Messaging::ExchangeManager & exchanges, const SessionHandle & session, const Request & request,
                    Command command);
    void Finish(CHIP_ERROR error);
    void OnResponse(app::CommandSender * sender, const app::CommandSender::ResponseData & response) override;
    void OnError(const app::CommandSender * sender, const app::CommandSender::ErrorData & error) override;
    void OnNoResponse(app::CommandSender * sender, const app::CommandSender::NoResponseData & response) override;
    void OnDone(app::CommandSender * sender) override;
};

} // namespace chip::Controller
