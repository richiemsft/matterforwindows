/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "LiveViewCommand.h"

#include <Windows.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadPrepareParams.h>
#include <app/data-model/Decode.h>
#include <clusters/CameraAvStreamManagement/Attributes.h>
#include <clusters/Descriptor/Attributes.h>
#include <clusters/WebRTCTransportProvider/Attributes.h>
#include <clusters/WebRTCTransportProvider/CommandIds.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <system/SystemClock.h>

#include <algorithm>
#include <atomic>

namespace CameraController {
using namespace chip;
using Controller::CameraProviderClient;
using Controller::CameraSignalingCluster;
namespace Descriptor = app::Clusters::Descriptor;
namespace Provider   = app::Clusters::WebRTCTransportProvider;
namespace {
constexpr app::DataModel::DeviceTypeEntry kDeviceTypes[] = { { 0x0147, 1 } };
std::atomic<bool> sInterrupted{ false };
BOOL WINAPI Interrupt(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT)
    {
        sInterrupted.store(true);
        return TRUE;
    }
    return FALSE;
}
template <class T>
bool Contains(const std::vector<T> & values, T value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}
template <class T>
CHIP_ERROR DecodeList(TLV::TLVReader & reader, std::vector<T> & output)
{
    app::DataModel::DecodableList<T> list;
    ReturnErrorOnFailure(app::DataModel::Decode(reader, list));
    output.clear();
    auto iterator = list.begin();
    while (iterator.Next())
    {
        VerifyOrReturnError(output.size() < 128, CHIP_ERROR_BUFFER_TOO_SMALL);
        output.push_back(iterator.GetValue());
    }
    return iterator.GetStatus();
}
} // namespace

LiveViewCommand::LiveViewCommand(CredentialIssuerCommands * credentials) :
    CHIPCommand("start", credentials, "Allocate and receive an H.264 LiveView stream; close its window or press Ctrl+C to stop.")
{
    mSelection.streamUsage         = app::Clusters::Globals::StreamUsageEnum::kLiveView;
    mSelection.videoCodec          = Av::VideoCodecEnum::kH264;
    mSelection.minResolution.width = mSelection.maxResolution.width = 640;
    mSelection.minResolution.height = mSelection.maxResolution.height = 480;
    mSelection.minFrameRate = mSelection.maxFrameRate = 30;
    mSelection.minBitRate                             = 500000;
    mSelection.maxBitRate                             = 2000000;
    mSelection.keyFrameInterval                       = 1000;
    AddArgument("node-id", 1, UINT64_MAX, &mNode);
    AddArgument("endpoint-id", 1, kInvalidEndpointId - 1, &mEndpoint,
                "Required only when multiple camera endpoints are discovered.");
    AddArgument("min-res-width", 1, UINT16_MAX, &mMinWidth);
    AddArgument("min-res-height", 1, UINT16_MAX, &mMinHeight);
    AddArgument("max-res-width", 1, UINT16_MAX, &mMaxWidth);
    AddArgument("max-res-height", 1, UINT16_MAX, &mMaxHeight);
    AddArgument("min-framerate", 1, UINT16_MAX, &mMinFps);
    AddArgument("max-framerate", 1, UINT16_MAX, &mMaxFps);
    AddArgument("min-bitrate", 1, UINT32_MAX, &mMinBitrate);
    AddArgument("max-bitrate", 1, UINT32_MAX, &mMaxBitrate);
    AddArgument("key-frame-interval", 1, UINT16_MAX, &mKeyFrameInterval);
    AddArgument("duration", 1, 3600, &mDuration, "Playback limit in seconds after the first decoded frame (default 300).");
    AddArgument("listen-port", 1, UINT16_MAX, &mListenPort, "Routable UDP/TCP requestor port (default configured port or 5540).");
}

CHIP_ERROR LiveViewCommand::Run()
{
    VerifyOrReturnError(!IsInteractive() && IsOperationalNodeId(mNode), CHIP_ERROR_INVALID_ARGUMENT);
    mSelection.minResolution.width  = mMinWidth.ValueOr(640);
    mSelection.minResolution.height = mMinHeight.ValueOr(480);
    mSelection.maxResolution.width  = mMaxWidth.ValueOr(640);
    mSelection.maxResolution.height = mMaxHeight.ValueOr(480);
    mSelection.minFrameRate         = mMinFps.ValueOr(30);
    mSelection.maxFrameRate         = mMaxFps.ValueOr(30);
    mSelection.minBitRate           = mMinBitrate.ValueOr(500000);
    mSelection.maxBitRate           = mMaxBitrate.ValueOr(2000000);
    mSelection.keyFrameInterval     = mKeyFrameInterval.ValueOr(1000);
    ReturnErrorOnFailure(ValidateSelection(mSelection));
    sInterrupted.store(false);
    VerifyOrReturnError(SetConsoleCtrlHandler(Interrupt, TRUE), CHIP_ERROR_INTERNAL);
    CHIP_ERROR error = mMedia.Start();
    if (error == CHIP_NO_ERROR)
    {
        error = CHIPCommand::Run();
    }
    if (mOwnFactory)
    {
        if (!mCleaned)
        {
            Cleanup();
            MaybeTearDownStack();
        }
        Controller::DeviceControllerFactory::GetInstance().Shutdown();
        mProvider.reset();
        mOwnFactory = false;
    }
    mMedia.RequestStop();
    const CHIP_ERROR closeError = mMedia.Join();
    if (error == CHIP_NO_ERROR)
    {
        error = closeError;
    }
    while (auto event = mMedia.TakeEvent())
    {
        if ((*event)["type"] == "error" && error == CHIP_NO_ERROR)
        {
            ChipLogError(chipTool, "Native camera receiver shutdown reported an error.");
            error = CHIP_ERROR_INTERNAL;
        }
    }
    if (!SetConsoleCtrlHandler(Interrupt, FALSE))
    {
        ChipLogError(chipTool, "Could not remove the native live-view console handler.");
        if (error == CHIP_NO_ERROR)
        {
            error = CHIP_ERROR_INTERNAL;
        }
    }
    return error;
}

System::Clock::Timeout LiveViewCommand::GetWaitDuration() const
{
    return System::Clock::Seconds32(mDuration.ValueOr(300) + 220);
}

CHIP_ERROR LiveViewCommand::ConfigureFactory(Controller::FactoryInitParams & params)
{
    VerifyOrReturnError(Controller::DeviceControllerFactory::GetInstance().GetSystemState() == nullptr, CHIP_ERROR_INCORRECT_STATE);
    ReturnErrorOnFailure(mPersistence.Init(&mDefaultStorage));
    mProvider.emplace(mDefaultStorage, mPersistence);
    params.dataModelProvider = &*mProvider;
    params.enableTCPServer   = true;
    params.listenPort        = mListenPort.ValueOr(params.listenPort != 0 ? params.listenPort : CHIP_PORT);
    mOwnFactory              = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR LiveViewCommand::RunCommand()
{
    auto * state = Controller::DeviceControllerFactory::GetInstance().GetSystemState();
    VerifyOrReturnError(state != nullptr && mProvider.has_value() && state->SystemLayer() != nullptr &&
                            state->Fabrics() != nullptr && state->CASESessionMgr() != nullptr,
                        CHIP_ERROR_INCORRECT_STATE);
    mSystem = state->SystemLayer();
    app::Clusters::Binding::ManagerInitParams binding;
    binding.mFabricTable               = state->Fabrics();
    binding.mCASESessionManager        = state->CASESessionMgr();
    binding.mStorage                   = &mDefaultStorage;
    binding.mEstablishConnectionOnInit = false;
    ReturnErrorOnFailure(mBindings.Init(binding));
    EndpointId endpoint;
    ReturnErrorOnFailure(Controller::CameraRequestorEndpoint::FindAvailableEndpoint(*mProvider, endpoint));
    mRequestor.emplace(endpoint, Span<const app::DataModel::DeviceTypeEntry>(kDeviceTypes), *state->Fabrics(), *mSystem, mBindings,
                       DeviceLayer::PlatformMgr());
    mDeviceTypes.requestor = endpoint;
    mPeer                  = CurrentCommissioner().GetPeerScopedId(mNode);
    mAccess.SetRequestor(&*mRequestor, mPeer);
    ReturnErrorOnFailure(Access::GetAccessControl().Init(&mAccess, mDeviceTypes));
    mAccessOwned = true;
    ReturnErrorOnFailure(mRequestor->Register(*mProvider, Access::GetAccessControl()));
    mComplete        = false;
    mStartupDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(90);
    ReturnErrorOnFailure(mSystem->StartTimer(System::Clock::Milliseconds32(20), OnPoll, this));
    mConnecting = true;
    return CurrentCommissioner().GetConnectedDevice(mNode, &mConnected, &mFailed, TransportPayloadCapability::kLargePayload);
}

void LiveViewCommand::Connected(void * context, Messaging::ExchangeManager & exchanges, const SessionHandle & session)
{
    auto & self = *static_cast<LiveViewCommand *>(context);
    VerifyOrReturn(!self.mComplete);
    self.mConnecting = false;
    if (!self.mSession.Grab(session) || !session->AllowsLargePayload() || session->GetPeer() != self.mPeer ||
        session->GetSubjectDescriptor().authMode != Access::AuthMode::kCase)
    {
        self.Stop(CHIP_ERROR_ACCESS_DENIED);
        return;
    }
    self.mExchanges = &exchanges;
    if (!self.mStopping)
    {
        CHIP_ERROR error;
        if (self.mEndpoint.HasValue())
        {
            self.mCamera = self.mEndpoint.Value();
            error        = self.Read(ReadStep::Servers);
        }
        else
        {
            error = self.Read(ReadStep::Parts);
        }
        if (error != CHIP_NO_ERROR)
        {
            self.Stop(error);
        }
    }
}

void LiveViewCommand::ConnectionFailed(void * context, const ScopedNodeId &, CHIP_ERROR error)
{
    auto & self      = *static_cast<LiveViewCommand *>(context);
    self.mConnecting = false;
    if (self.mStopping)
    {
        self.CleanupFailure(error, "CASE reconnection");
    }
    else
    {
        self.Stop(error);
    }
}

CHIP_ERROR LiveViewCommand::Read(ReadStep step)
{
    VerifyOrReturnError(mSession && mExchanges != nullptr && !mReader, CHIP_ERROR_INCORRECT_STATE);
    mReadStep             = step;
    ClusterId cluster     = Av::Id;
    AttributeId attribute = 0;
    switch (step)
    {
    case ReadStep::Parts:
        cluster   = Descriptor::Id;
        attribute = Descriptor::Attributes::PartsList::Id;
        break;
    case ReadStep::Servers:
        cluster   = Descriptor::Id;
        attribute = Descriptor::Attributes::ServerList::Id;
        break;
    case ReadStep::Attributes:
        attribute = Av::Attributes::AttributeList::Id;
        break;
    case ReadStep::AvCommands:
        attribute = Av::Attributes::AcceptedCommandList::Id;
        break;
    case ReadStep::ProviderCommands:
        cluster   = Provider::Id;
        attribute = Provider::Attributes::AcceptedCommandList::Id;
        break;
    case ReadStep::AvRevision:
        attribute = Av::Attributes::ClusterRevision::Id;
        break;
    case ReadStep::ProviderRevision:
        cluster   = Provider::Id;
        attribute = Provider::Attributes::ClusterRevision::Id;
        break;
    case ReadStep::Features:
        attribute = Av::Attributes::FeatureMap::Id;
        break;
    case ReadStep::Sensor:
        attribute = Av::Attributes::VideoSensorParams::Id;
        break;
    case ReadStep::Viewport:
        attribute = Av::Attributes::MinViewportResolution::Id;
        break;
    case ReadStep::Usages:
        attribute = Av::Attributes::SupportedStreamUsages::Id;
        break;
    case ReadStep::SoftPrivacy:
        attribute = Av::Attributes::SoftLivestreamPrivacyModeEnabled::Id;
        break;
    case ReadStep::HardPrivacy:
        attribute = Av::Attributes::HardPrivacyModeOn::Id;
        break;
    case ReadStep::Allocated:
        attribute = Av::Attributes::AllocatedVideoStreams::Id;
        break;
    }
    mReadPath = app::AttributePathParams(step == ReadStep::Parts ? 0 : mCamera, cluster, attribute);
    mReadDone = mReadSeen = false;
    mReadError            = CHIP_NO_ERROR;
    mReader               = std::make_unique<app::ReadClient>(app::InteractionModelEngine::GetInstance(), mExchanges, mBuffered,
                                                app::ReadClient::InteractionType::Read);
    app::ReadPrepareParams params(mSession.Get().Value());
    params.mpAttributePathParamsList    = &mReadPath;
    params.mAttributePathParamsListSize = 1;
    params.mTimeout                     = System::Clock::Seconds16(20);
    return mReader->SendRequest(params);
}

void LiveViewCommand::OnAttributeData(const app::ConcreteDataAttributePath & path, TLV::TLVReader * data,
                                      const app::StatusIB & status)
{
    if (mReadSeen || path.mEndpointId != mReadPath.mEndpointId || path.mClusterId != mReadPath.mClusterId ||
        path.mAttributeId != mReadPath.mAttributeId)
    {
        mReadError = CHIP_ERROR_INVALID_ARGUMENT;
        return;
    }
    mReadSeen  = true;
    mReadError = status.ToChipError();
    if (mReadError == CHIP_NO_ERROR)
    {
        mReadError = data == nullptr ? CHIP_ERROR_INVALID_ARGUMENT : DecodeRead(*data);
    }
}

CHIP_ERROR LiveViewCommand::DecodeRead(TLV::TLVReader & reader)
{
    switch (mReadStep)
    {
    case ReadStep::Parts: {
        std::vector<EndpointId> endpoints;
        ReturnErrorOnFailure(DecodeList(reader, endpoints));
        VerifyOrReturnError(!endpoints.empty(), CHIP_ERROR_NOT_FOUND);
        for (EndpointId endpoint : endpoints)
        {
            VerifyOrReturnError(endpoint != 0 && endpoint != kInvalidEndpointId, CHIP_ERROR_INVALID_ARGUMENT);
            if (std::find(mEndpoints.begin(), mEndpoints.end(), endpoint) == mEndpoints.end())
            {
                mEndpoints.push_back(endpoint);
            }
        }
        return CHIP_NO_ERROR;
    }
    case ReadStep::Servers: {
        std::vector<ClusterId> clusters;
        ReturnErrorOnFailure(DecodeList(reader, clusters));
        const bool camera = Contains(clusters, Av::Id) && Contains(clusters, Provider::Id);
        if (mEndpoint.HasValue())
        {
            VerifyOrReturnError(camera, CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        }
        if (camera)
        {
            mCameras.push_back(mCamera);
        }
        return CHIP_NO_ERROR;
    }
    case ReadStep::Attributes:
        ReturnErrorOnFailure(DecodeList(reader, mAttributes));
        for (AttributeId required :
             { Av::Attributes::FeatureMap::Id, Av::Attributes::VideoSensorParams::Id, Av::Attributes::MinViewportResolution::Id,
               Av::Attributes::SupportedStreamUsages::Id, Av::Attributes::AllocatedVideoStreams::Id })
        {
            VerifyOrReturnError(Contains(mAttributes, required), CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        }
        return CHIP_NO_ERROR;
    case ReadStep::AvCommands:
    case ReadStep::ProviderCommands: {
        std::vector<CommandId> commands;
        ReturnErrorOnFailure(DecodeList(reader, commands));
        if (mReadStep == ReadStep::AvCommands)
        {
            VerifyOrReturnError(Contains(commands, Av::Commands::VideoStreamAllocate::Id) &&
                                    Contains(commands, Av::Commands::VideoStreamDeallocate::Id),
                                CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        }
        else
        {
            VerifyOrReturnError(Contains(commands, Provider::Commands::ProvideOffer::Id) &&
                                    Contains(commands, Provider::Commands::ProvideICECandidates::Id) &&
                                    Contains(commands, Provider::Commands::EndSession::Id),
                                CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        }
        return CHIP_NO_ERROR;
    }
    case ReadStep::AvRevision:
    case ReadStep::ProviderRevision: {
        uint16_t revision;
        ReturnErrorOnFailure(app::DataModel::Decode(reader, revision));
        VerifyOrReturnError(revision != 0, CHIP_ERROR_INVALID_ARGUMENT);
        ChipLogProgress(chipTool, "Camera cluster 0x%08lx revision %u (SDK revision compatibility still needs device testing).",
                        static_cast<unsigned long>(mReadPath.mClusterId), revision);
        return CHIP_NO_ERROR;
    }
    case ReadStep::Features: {
        uint32_t features;
        ReturnErrorOnFailure(app::DataModel::Decode(reader, features));
        VerifyOrReturnError((features & static_cast<uint32_t>(Av::Feature::kVideo)) != 0, CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        return CHIP_NO_ERROR;
    }
    case ReadStep::Sensor:
        return app::DataModel::Decode(reader, mSensor);
    case ReadStep::Viewport:
        return app::DataModel::Decode(reader, mViewport);
    case ReadStep::Usages: {
        std::vector<app::Clusters::Globals::StreamUsageEnum> usages;
        ReturnErrorOnFailure(DecodeList(reader, usages));
        VerifyOrReturnError(Contains(usages, app::Clusters::Globals::StreamUsageEnum::kLiveView),
                            CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        return CHIP_NO_ERROR;
    }
    case ReadStep::SoftPrivacy:
    case ReadStep::HardPrivacy: {
        bool enabled;
        ReturnErrorOnFailure(app::DataModel::Decode(reader, enabled));
        if (enabled)
        {
            ChipLogError(chipTool, "Camera %s privacy is enabled.",
                         mReadStep == ReadStep::SoftPrivacy ? "software livestream" : "hardware");
        }
        VerifyOrReturnError(!enabled, CHIP_ERROR_ACCESS_DENIED);
        return CHIP_NO_ERROR;
    }
    case ReadStep::Allocated: {
        app::DataModel::DecodableList<Av::Structs::VideoStreamStruct::DecodableType> streams;
        ReturnErrorOnFailure(app::DataModel::Decode(reader, streams));
        auto iterator = streams.begin();
        bool found    = false;
        size_t count  = 0;
        while (iterator.Next())
        {
            VerifyOrReturnError(++count <= 128, CHIP_ERROR_BUFFER_TOO_SMALL);
            const auto & stream = iterator.GetValue();
            if (mStream.has_value() && stream.videoStreamID == *mStream)
            {
                VerifyOrReturnError(!found, CHIP_ERROR_INVALID_ARGUMENT);
                ReturnErrorOnFailure(ValidateAllocatedStream(mSelection, stream));
                mAllocated = stream;
                found      = true;
            }
        }
        ReturnErrorOnFailure(iterator.GetStatus());
        VerifyOrReturnError(found, CHIP_ERROR_NOT_FOUND);
        return CHIP_NO_ERROR;
    }
    }
    return CHIP_ERROR_INTERNAL;
}

CHIP_ERROR LiveViewCommand::NextRead()
{
    if (mReadStep == ReadStep::Parts || (mReadStep == ReadStep::Servers && !mEndpoint.HasValue()))
    {
        if (!mEndpoints.empty())
        {
            mCamera = mEndpoints.front();
            mEndpoints.pop_front();
            return Read(ReadStep::Servers);
        }
        VerifyOrReturnError(mCameras.size() == 1, CHIP_ERROR_INVALID_ARGUMENT);
        mCamera = mCameras.front();
    }
    if (mReadStep == ReadStep::Servers)
    {
        ChipLogProgress(chipTool, "Discovered camera endpoint %u; validating capabilities and privacy.", mCamera);
        mReads = { ReadStep::Attributes, ReadStep::AvCommands,       ReadStep::ProviderCommands,
                   ReadStep::AvRevision, ReadStep::ProviderRevision, ReadStep::Features,
                   ReadStep::Sensor,     ReadStep::Viewport,         ReadStep::Usages };
    }
    if (mReadStep == ReadStep::Attributes)
    {
        if (Contains(mAttributes, Av::Attributes::SoftLivestreamPrivacyModeEnabled::Id))
        {
            mReads.push_back(ReadStep::SoftPrivacy);
        }
        if (Contains(mAttributes, Av::Attributes::HardPrivacyModeOn::Id))
        {
            mReads.push_back(ReadStep::HardPrivacy);
        }
    }
    if (!mReads.empty())
    {
        auto next = mReads.front();
        mReads.pop_front();
        return Read(next);
    }
    if (mCheckingPrivacy)
    {
        mCheckingPrivacy = false;
        mPrivacyDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(1);
        return CHIP_NO_ERROR;
    }
    if (mReadStep == ReadStep::Allocated)
    {
        mReady           = true;
        mPrivacyDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(1);
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(ValidateSensor(mSelection, mSensor, mViewport));
    return Send(mSelection, Invoke::Allocate);
}

template <class Request>
CHIP_ERROR LiveViewCommand::Send(const Request & request, Invoke kind)
{
    VerifyOrReturnError(mSession && mExchanges != nullptr && !mSender, CHIP_ERROR_NOT_CONNECTED);
    mInvoke      = kind;
    mInvokeError = CHIP_NO_ERROR;
    mInvokeDone = mInvokeSeen = false;
    mSender = std::make_unique<app::CommandSender>(static_cast<app::CommandSender::ExtendableCallback *>(this), mExchanges, false,
                                                   false, true);
    app::CommandPathParams path(mCamera, Av::Id, Request::GetCommandId(), app::CommandPathFlags::kEndpointIdValid);
    CHIP_ERROR error = mSender->AddRequestData(path, request);
    if (error == CHIP_NO_ERROR)
    {
        error =
            mSender->SendCommandRequest(mSession.Get().Value(), MakeOptional<System::Clock::Timeout>(System::Clock::Seconds16(30)));
    }
    if (error != CHIP_NO_ERROR)
    {
        mSender.reset();
        mInvoke = Invoke::None;
    }
    return error;
}

void LiveViewCommand::OnResponse(app::CommandSender *, const app::CommandSender::ResponseData & response)
{
    if (mInvokeSeen || response.path.mEndpointId != mCamera || response.path.mClusterId != Av::Id)
    {
        mInvokeError = CHIP_ERROR_INVALID_ARGUMENT;
        return;
    }
    mInvokeSeen  = true;
    mInvokeError = response.statusIB.ToChipError();
    if (mInvokeError != CHIP_NO_ERROR)
    {
        return;
    }
    if (mInvoke == Invoke::Allocate)
    {
        if (response.data == nullptr || response.path.mCommandId != Av::Commands::VideoStreamAllocateResponse::Id)
        {
            mInvokeError = CHIP_ERROR_INVALID_ARGUMENT;
            return;
        }
        Av::Commands::VideoStreamAllocateResponse::DecodableType allocated;
        mInvokeError = app::DataModel::Decode(*response.data, allocated);
        if (mInvokeError == CHIP_NO_ERROR)
        {
            mStream = allocated.videoStreamID;
            ChipLogProgress(chipTool, "Owned camera LiveView stream %u allocated.", *mStream);
        }
    }
    else if (response.path.mCommandId != Av::Commands::VideoStreamDeallocate::Id || response.data != nullptr)
    {
        mInvokeError = CHIP_ERROR_INVALID_ARGUMENT;
    }
}

CHIP_ERROR LiveViewCommand::MediaEvent(const Json::Value & event)
{
    const auto type = event["type"].asString();
    if (type == "offer")
    {
        VerifyOrReturnError(mOffer.empty() && !mOfferSent && event["sdp"].isString(), CHIP_ERROR_INVALID_ARGUMENT);
        auto sdp = event["sdp"].asString();
        VerifyOrReturnError(
            CameraSignalingCluster::IsValidSignalingText(CharSpan(sdp.data(), sdp.size()), CameraSignalingCluster::kMaxSdpBytes),
            CHIP_ERROR_INVALID_ARGUMENT);
        mOffer = std::move(sdp);
    }
    else if (type == "candidate")
    {
        const auto & value = event["candidate"];
        VerifyOrReturnError(value.isObject() && value["candidate"].isString() &&
                                (value["sdpMid"].isNull() || value["sdpMid"].isString()) &&
                                (value["sdpMLineIndex"].isNull() ||
                                 (value["sdpMLineIndex"].isUInt() && value["sdpMLineIndex"].asUInt() <= UINT16_MAX)) &&
                                mCandidates.size() < 128,
                            CHIP_ERROR_INVALID_ARGUMENT);
        CameraSignalingCluster::Candidate candidate;
        candidate.candidate = value["candidate"].asString();
        VerifyOrReturnError(
            CameraSignalingCluster::IsValidSignalingText(CharSpan(candidate.candidate.data(), candidate.candidate.size()),
                                                         CameraSignalingCluster::kMaxCandidateBytes),
            CHIP_ERROR_INVALID_ARGUMENT);
        if (!value["sdpMid"].isNull())
        {
            candidate.mid = value["sdpMid"].asString();
            VerifyOrReturnError(CameraSignalingCluster::IsValidSignalingText(CharSpan(candidate.mid->data(), candidate.mid->size()),
                                                                             CameraSignalingCluster::kMaxMidBytes),
                                CHIP_ERROR_INVALID_ARGUMENT);
        }
        if (!value["sdpMLineIndex"].isNull())
        {
            candidate.lineIndex = static_cast<uint16_t>(value["sdpMLineIndex"].asUInt());
        }
        mCandidates.push_back(std::move(candidate));
    }
    else if (type == "decoded")
    {
        VerifyOrReturnError(mAnswerApplied && mAllocated.has_value() && event["codec"] == "video/H264" &&
                                event["frames"].isUInt64() && event["frames"].asUInt64() > 0 && event["width"].isUInt() &&
                                event["height"].isUInt() && event["width"].asUInt() >= mAllocated->minResolution.width &&
                                event["width"].asUInt() <= mAllocated->maxResolution.width &&
                                event["height"].asUInt() >= mAllocated->minResolution.height &&
                                event["height"].asUInt() <= mAllocated->maxResolution.height,
                            CHIP_ERROR_INVALID_ARGUMENT);
        if (!mPlaying)
        {
            mPlaying          = true;
            mPlaybackDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(mDuration.ValueOr(300));
            ChipLogProgress(chipTool, "Camera playback verified: H.264, %u x %u, decoded frames=%llu.", event["width"].asUInt(),
                            event["height"].asUInt(), static_cast<unsigned long long>(event["frames"].asUInt64()));
        }
        VerifyOrReturnError(event["frames"].asUInt64() >= mFrames, CHIP_ERROR_INVALID_ARGUMENT);
        if (event["frames"].asUInt64() > mFrames)
        {
            mFrameDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(10);
        }
        mFrames = event["frames"].asUInt64();
    }
    else if (type == "closed")
    {
        Stop(mPlaying ? CHIP_NO_ERROR : CHIP_ERROR_CANCELLED);
    }
    else if (type == "error" || type == "disconnected")
    {
        // Do not log SDP, ICE candidates, or arbitrary browser message content.
        ChipLogError(chipTool, "Native camera media engine reported %s.", type.c_str());
        return CHIP_ERROR_NOT_CONNECTED;
    }
    else
    {
        VerifyOrReturnError(type == "connected", CHIP_ERROR_INVALID_ARGUMENT);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR LiveViewCommand::Offer()
{
    using Selection = CameraSignalingCluster::StreamSelection;
    ReturnErrorOnFailure(
        mRequestor->Signaling().CreateSession(mPeer, mCamera, Selection::ExplicitId(*mStream), Selection::Absent(), mHandle));
    mClient = std::make_unique<CameraProviderClient>(mRequestor->Signaling(), mHandle);
    ReturnErrorOnFailure(mClient->SendOffer(*mExchanges, mSession.Get().Value(), CharSpan(mOffer.data(), mOffer.size())));
    mOffer.clear();
    mOfferSent     = true;
    mSignalingBusy = true;
    return CHIP_NO_ERROR;
}

void LiveViewCommand::OnPoll(System::Layer *, void * context)
{
    static_cast<LiveViewCommand *>(context)->Poll();
}

void LiveViewCommand::Poll()
{
    VerifyOrReturn(!mComplete);
    while (auto event = mMedia.TakeEvent())
    {
        if (!mStopping)
        {
            CHIP_ERROR error = MediaEvent(*event);
            if (error != CHIP_NO_ERROR)
            {
                ChipLogError(chipTool, "Native camera media event failed: %" CHIP_ERROR_FORMAT, error.Format());
                Stop(error);
            }
        }
    }
    if (sInterrupted.load() && !mStopping)
    {
        Stop(mPlaying ? CHIP_NO_ERROR : CHIP_ERROR_CANCELLED);
    }
    if (!mStopping && mReadDone)
    {
        mReader.reset();
        mReadDone        = false;
        CHIP_ERROR error = mReadError;
        if (error == CHIP_NO_ERROR)
        {
            error = mReadSeen ? NextRead() : CHIP_ERROR_NOT_FOUND;
        }
        if (error != CHIP_NO_ERROR)
        {
            Stop(error);
        }
    }
    if (mInvokeDone)
    {
        const Invoke kind      = mInvoke;
        const CHIP_ERROR error = mInvokeSeen || mInvokeError != CHIP_NO_ERROR ? mInvokeError : CHIP_ERROR_TIMEOUT;
        mSender.reset();
        mInvokeDone = false;
        mInvoke     = Invoke::None;
        if (kind == Invoke::Deallocate)
        {
            if (error == CHIP_NO_ERROR)
            {
                ChipLogProgress(chipTool, "Owned camera stream deallocated.");
                mStream.reset();
            }
            else
            {
                CleanupFailure(error, "VideoStreamDeallocate");
            }
            Finish();
        }
        else if (error != CHIP_NO_ERROR)
        {
            if (!mStream.has_value())
            {
                ChipLogError(chipTool, "Allocation response unavailable; cannot safely identify or release a remote stream.");
            }
            Stop(error);
        }
        else if (!mStopping)
        {
            const CHIP_ERROR readError = Read(ReadStep::Allocated);
            if (readError != CHIP_NO_ERROR)
            {
                Stop(readError);
            }
        }
    }
    VerifyOrReturn(!mComplete);
    if (mClient)
    {
        if (auto result = mClient->TakeResult(); result.has_value())
        {
            mSignalingBusy = false;
            if (result->error != CHIP_NO_ERROR)
            {
                if (mStopping)
                {
                    CleanupFailure(result->error, "camera signaling completion");
                }
                else
                {
                    Stop(result->error);
                }
            }
        }
    }
    while (auto signal = mRequestor->Signaling().TakeSignal())
    {
        if (mStopping)
        {
            continue;
        }
        if (signal->type == CameraSignalingCluster::SignalType::Answer)
        {
            mAnswer = std::move(signal->sdp);
        }
        else if (signal->type == CameraSignalingCluster::SignalType::Candidates)
        {
            for (const auto & candidate : signal->candidates)
            {
                Json::Value message;
                message["type"]                   = "candidate";
                message["candidate"]["candidate"] = candidate.candidate;
                message["candidate"]["sdpMid"]    = candidate.mid.has_value() ? Json::Value(*candidate.mid) : Json::Value();
                message["candidate"]["sdpMLineIndex"] =
                    candidate.lineIndex.has_value() ? Json::Value(*candidate.lineIndex) : Json::Value();
                const CHIP_ERROR error = mMedia.Send(std::move(message));
                if (error != CHIP_NO_ERROR)
                {
                    Stop(error);
                    break;
                }
            }
        }
        else
        {
            Stop(signal->type == CameraSignalingCluster::SignalType::Failure ? signal->error : CHIP_ERROR_NOT_CONNECTED);
        }
    }
    const auto now = System::SystemClock().GetMonotonicTimestamp();
    if (!mStopping && mReady && !mReader && now >= mPrivacyDeadline)
    {
        if (Contains(mAttributes, Av::Attributes::SoftLivestreamPrivacyModeEnabled::Id))
        {
            mReads.push_back(ReadStep::SoftPrivacy);
        }
        if (Contains(mAttributes, Av::Attributes::HardPrivacyModeOn::Id))
        {
            mReads.push_back(ReadStep::HardPrivacy);
        }
        mCheckingPrivacy       = true;
        const CHIP_ERROR error = NextRead();
        if (error != CHIP_NO_ERROR)
        {
            Stop(error);
        }
    }
    if (!mStopping)
    {
        if (!mSession && !mConnecting)
        {
            Stop(CHIP_ERROR_NOT_CONNECTED);
        }
        else if (mPlaying && now >= mFrameDeadline)
        {
            ChipLogError(chipTool, "Camera decoded-frame progress stalled for ten seconds.");
            Stop(CHIP_ERROR_TIMEOUT);
        }
        else if ((!mPlaying && now >= mStartupDeadline) || (mPlaying && now >= mPlaybackDeadline))
        {
            Stop(mPlaying ? CHIP_NO_ERROR : CHIP_ERROR_TIMEOUT);
        }
        else if (mReady && !mOffer.empty() && !mOfferSent)
        {
            const CHIP_ERROR error = Offer();
            if (error != CHIP_NO_ERROR)
            {
                Stop(error);
            }
        }
        else if (mClient && mRequestor->Signaling().IsNegotiated(mHandle))
        {
            if (!mAnswerApplied && !mAnswer.empty())
            {
                Json::Value message;
                message["type"]        = "answer";
                message["sdp"]         = std::move(mAnswer);
                const CHIP_ERROR error = mMedia.Send(std::move(message));
                if (error != CHIP_NO_ERROR)
                {
                    Stop(error);
                }
                else
                {
                    mAnswerApplied = true;
                }
            }
            if (!mStopping && !mSignalingBusy && !mCandidates.empty())
            {
                std::vector<CameraSignalingCluster::Candidate> batch;
                while (!mCandidates.empty() && batch.size() < CameraSignalingCluster::kMaxCandidates)
                {
                    batch.push_back(std::move(mCandidates.front()));
                    mCandidates.pop_front();
                }
                const CHIP_ERROR error = mClient->SendCandidates(
                    *mExchanges, mSession.Get().Value(), Span<const CameraSignalingCluster::Candidate>(batch.data(), batch.size()));
                if (error != CHIP_NO_ERROR)
                {
                    Stop(error);
                }
                else
                {
                    mSignalingBusy = true;
                }
            }
        }
    }
    if (mStopping)
    {
        AdvanceCleanup();
    }
    if (!mComplete)
    {
        const CHIP_ERROR error = mSystem->StartTimer(System::Clock::Milliseconds32(20), OnPoll, this);
        if (error != CHIP_NO_ERROR)
        {
            CleanupFailure(error, "poll timer; remote cleanup could not be awaited");
            Finish();
        }
    }
}

void LiveViewCommand::Stop(CHIP_ERROR error)
{
    VerifyOrReturn(!mStopping && !mComplete);
    mStopping        = true;
    mFinalError      = error;
    mCleanupDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(120);
    mMedia.RequestStop();
    mReader.reset();
    mConnected.Cancel();
    mFailed.Cancel();
    mConnecting = false;
    ChipLogProgress(chipTool, "Stopping camera live view: %" CHIP_ERROR_FORMAT "; awaiting owned remote cleanup.", error.Format());
    // Keep a pending Allocate/Offer alive: its late response identifies the resource to release.
}

void LiveViewCommand::CleanupFailure(CHIP_ERROR error, const char * operation)
{
    ChipLogError(chipTool, "Native camera cleanup %s failed: %" CHIP_ERROR_FORMAT, operation, error.Format());
    if (mCleanupError == CHIP_NO_ERROR)
    {
        mCleanupError = error;
    }
}

void LiveViewCommand::AdvanceCleanup()
{
    if (System::SystemClock().GetMonotonicTimestamp() >= mCleanupDeadline)
    {
        CleanupFailure(CHIP_ERROR_TIMEOUT, "deadline (remote resources may remain)");
        Finish();
        return;
    }
    if (mSender || mSignalingBusy)
    {
        return;
    }
    if (!mStream.has_value() && mHandle == 0)
    {
        Finish();
        return;
    }
    if (!mSession)
    {
        if (mConnecting)
        {
            if (System::SystemClock().GetMonotonicTimestamp() >= mReconnectDeadline)
            {
                mConnected.Cancel();
                mFailed.Cancel();
                mConnecting = false;
                CleanupFailure(CHIP_ERROR_TIMEOUT, "CASE reconnection");
                Finish();
            }
            return;
        }
        if (!mReconnectAttempted)
        {
            mReconnectAttempted = true;
            mConnecting         = true;
            mReconnectDeadline  = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(10);
            const CHIP_ERROR error =
                CurrentCommissioner().GetConnectedDevice(mNode, &mConnected, &mFailed, TransportPayloadCapability::kLargePayload);
            if (error != CHIP_NO_ERROR)
            {
                mConnecting = false;
                CleanupFailure(error, "CASE reconnection start");
            }
            return;
        }
        CleanupFailure(CHIP_ERROR_NOT_CONNECTED, "remote End/deallocation unavailable");
        Finish();
        return;
    }
    if (!mEndAttempted && mClient)
    {
        mEndAttempted = true;
        CameraSignalingCluster::SessionInfo info;
        if (mRequestor->Signaling().GetSessionInfoForEnd(mHandle, info) == CHIP_NO_ERROR && info.protocolId.has_value())
        {
            const CHIP_ERROR error =
                mClient->SendEnd(*mExchanges, mSession.Get().Value(), app::Clusters::Globals::WebRTCEndReasonEnum::kUserHangup);
            if (error == CHIP_NO_ERROR)
            {
                mSignalingBusy = true;
                return;
            }
            CleanupFailure(error, "EndSession");
        }
        else if (mOfferSent)
        {
            CameraSignalingCluster::SessionState state;
            if (mRequestor->Signaling().GetSessionState(mHandle, state) != CHIP_NO_ERROR ||
                state != CameraSignalingCluster::SessionState::Closed)
            {
                CleanupFailure(CHIP_ERROR_INCORRECT_STATE, "EndSession identity unavailable");
            }
        }
    }
    if (mStream.has_value() && !mDeallocateAttempted)
    {
        mDeallocateAttempted = true;
        Av::Commands::VideoStreamDeallocate::Type request;
        request.videoStreamID  = *mStream;
        const CHIP_ERROR error = Send(request, Invoke::Deallocate);
        if (error == CHIP_NO_ERROR)
        {
            return;
        }
        CleanupFailure(error, "VideoStreamDeallocate start");
    }
    Finish();
}

void LiveViewCommand::Finish()
{
    mSystem->CancelTimer(OnPoll, this);
    mComplete = true;
    SetCommandExitStatus(mFinalError != CHIP_NO_ERROR ? mFinalError : mCleanupError);
}

void LiveViewCommand::Cleanup()
{
    mConnected.Cancel();
    mFailed.Cancel();
    if (mSystem)
    {
        mSystem->CancelTimer(OnPoll, this);
    }
    mReader.reset();
    mSender.reset();
    mClient.reset();
    mSession.Release();
    if (mRequestor)
    {
        if (mHandle != 0)
        {
            LogErrorOnFailure(mRequestor->Signaling().CloseSession(mHandle));
        }
        LogErrorOnFailure(mRequestor->Unregister());
        mRequestor.reset();
    }
    mBindings.Shutdown();
    mAccess.SetRequestor(nullptr, ScopedNodeId());
    if (mAccessOwned)
    {
        Access::GetAccessControl().Finish();
        mAccessOwned = false;
    }
    mMedia.RequestStop();
    mComplete = mCleaned = true;
    mSystem              = nullptr;
    mExchanges           = nullptr;
}

} // namespace CameraController
