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
#pragma once

#include "StreamSelection.h"
#include "WebView2Receiver.h"
#include <commands/camera/CameraSignalingCommand.h>

#include <app/BufferedReadCallback.h>
#include <app/ReadClient.h>
#include <app/clusters/bindings/BindingManager.h>

#include <deque>

namespace CameraController {

class LiveViewCommand : public CHIPCommand,
                        private chip::app::ReadClient::Callback,
                        private chip::app::CommandSender::ExtendableCallback
{
public:
    explicit LiveViewCommand(CredentialIssuerCommands * credentials);
    CHIP_ERROR Run() override;

private:
    friend class LiveViewCommandTestAccess;
    friend class LiveViewFlowTestAccess;
    class DeviceTypes : public chip::Access::AccessControl::DeviceTypeResolver
    {
        bool IsDeviceTypeOnEndpoint(chip::DeviceTypeId type, chip::EndpointId endpoint) override
        {
            return type == 0x0147 && endpoint == requestor;
        }

    public:
        chip::EndpointId requestor = chip::kInvalidEndpointId;
    };
    class RequestorAccess : public chip::Access::AccessControl::Delegate
    {
    public:
        void SetRequestor(chip::Controller::CameraRequestorEndpoint * requestor, chip::ScopedNodeId peer)
        {
            mRequestor = requestor;
            mSignaling.SetRequestor(requestor == nullptr ? nullptr : &requestor->Signaling(), peer);
        }
        CHIP_ERROR Check(const chip::Access::SubjectDescriptor & subject, const chip::Access::RequestPath & path,
                         chip::Access::Privilege privilege) override
        {
            if (mRequestor != nullptr && path.endpoint == mRequestor->GetEndpointId() &&
                path.cluster == chip::app::Clusters::Binding::Id)
            {
                return mRequestor->CheckAccess(subject, path, privilege);
            }
            return mSignaling.Check(subject, path, privilege);
        }

    private:
        chip::Controller::CameraRequestorEndpoint * mRequestor = nullptr;
        CameraSignalingAccess mSignaling;
    };
    enum class ReadStep : uint8_t
    {
        Parts,
        Servers,
        Attributes,
        AvCommands,
        ProviderCommands,
        AvRevision,
        ProviderRevision,
        Features,
        Sensor,
        Viewport,
        Usages,
        SoftPrivacy,
        HardPrivacy,
        Allocated
    };
    enum class Invoke : uint8_t
    {
        None,
        Allocate,
        Deallocate
    };

    CHIP_ERROR RunCommand() override;
    CHIP_ERROR ConfigureFactory(chip::Controller::FactoryInitParams & params) override;
    bool NeedsOperationalAdvertising() override { return true; }
    chip::System::Clock::Timeout GetWaitDuration() const override;
    void Cleanup() override;
    CHIP_ERROR Read(ReadStep step);
    CHIP_ERROR NextRead();
    CHIP_ERROR DecodeRead(chip::TLV::TLVReader & reader);
    void OnAttributeData(const chip::app::ConcreteDataAttributePath & path, chip::TLV::TLVReader * data,
                         const chip::app::StatusIB & status) override;
    void OnError(CHIP_ERROR error) override { mReadError = error; }
    void OnDone(chip::app::ReadClient *) override { mReadDone = true; }
    void OnResponse(chip::app::CommandSender *, const chip::app::CommandSender::ResponseData & response) override;
    void OnError(const chip::app::CommandSender *, const chip::app::CommandSender::ErrorData & error) override
    {
        mInvokeError = error.error;
    }
    void OnNoResponse(chip::app::CommandSender *, const chip::app::CommandSender::NoResponseData &) override
    {
        mInvokeError = CHIP_ERROR_TIMEOUT;
    }
    void OnDone(chip::app::CommandSender *) override { mInvokeDone = true; }
    template <class Request>
    CHIP_ERROR Send(const Request & request, Invoke kind);
    static void Connected(void * context, chip::Messaging::ExchangeManager & exchanges, const chip::SessionHandle & session);
    static void ConnectionFailed(void * context, const chip::ScopedNodeId &, CHIP_ERROR error);
    static void OnPoll(chip::System::Layer *, void * context);
    void Poll();
    CHIP_ERROR MediaEvent(const Json::Value & event);
    CHIP_ERROR Offer();
    void Stop(CHIP_ERROR error);
    void AdvanceCleanup();
    void CleanupFailure(CHIP_ERROR error, const char * operation);
    void Finish();

    chip::NodeId mNode = chip::kUndefinedNodeId;
    chip::ScopedNodeId mPeer;
    chip::Optional<chip::EndpointId> mEndpoint;
    chip::Optional<uint16_t> mListenPort;
    chip::Optional<uint16_t> mDuration;
    chip::Optional<uint16_t> mMinWidth, mMinHeight, mMaxWidth, mMaxHeight, mMinFps, mMaxFps, mKeyFrameInterval;
    chip::Optional<uint32_t> mMinBitrate, mMaxBitrate;
    chip::EndpointId mCamera = chip::kInvalidEndpointId;
    Av::Commands::VideoStreamAllocate::Type mSelection;
    Av::Structs::VideoSensorParamsStruct::Type mSensor;
    Av::Structs::VideoResolutionStruct::Type mViewport;
    std::deque<chip::EndpointId> mEndpoints;
    std::vector<chip::EndpointId> mCameras;
    std::vector<chip::AttributeId> mAttributes;
    std::deque<ReadStep> mReads;
    std::deque<chip::Controller::CameraSignalingCluster::Candidate> mCandidates;
    WebView2Receiver mMedia;
    chip::app::DefaultAttributePersistenceProvider mPersistence;
    std::optional<chip::app::CodeDrivenDataModelProvider> mProvider;
    chip::app::Clusters::Binding::Manager mBindings;
    std::optional<chip::Controller::CameraRequestorEndpoint> mRequestor;
    std::unique_ptr<chip::Controller::CameraProviderClient> mClient;
    RequestorAccess mAccess;
    DeviceTypes mDeviceTypes;
    chip::System::Layer * mSystem                 = nullptr;
    chip::Messaging::ExchangeManager * mExchanges = nullptr;
    chip::SessionHolder mSession;
    chip::Callback::Callback<chip::OnDeviceConnected> mConnected{ Connected, this };
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mFailed{ ConnectionFailed, this };
    chip::app::BufferedReadCallback mBuffered{ *this, true };
    std::unique_ptr<chip::app::ReadClient> mReader;
    chip::app::AttributePathParams mReadPath;
    std::unique_ptr<chip::app::CommandSender> mSender;
    chip::System::Clock::Timestamp mStartupDeadline;
    chip::System::Clock::Timestamp mPlaybackDeadline;
    chip::System::Clock::Timestamp mFrameDeadline;
    chip::System::Clock::Timestamp mPrivacyDeadline;
    chip::System::Clock::Timestamp mCleanupDeadline;
    chip::System::Clock::Timestamp mReconnectDeadline;
    ReadStep mReadStep = ReadStep::Parts;
    Invoke mInvoke     = Invoke::None;
    std::optional<uint16_t> mStream;
    std::optional<Av::Structs::VideoStreamStruct::Type> mAllocated;
    std::string mOffer;
    std::string mAnswer;
    uint64_t mHandle          = 0;
    uint64_t mFrames          = 0;
    CHIP_ERROR mReadError     = CHIP_NO_ERROR;
    CHIP_ERROR mInvokeError   = CHIP_NO_ERROR;
    CHIP_ERROR mFinalError    = CHIP_NO_ERROR;
    CHIP_ERROR mCleanupError  = CHIP_NO_ERROR;
    bool mReadSeen            = false;
    bool mReadDone            = false;
    bool mInvokeSeen          = false;
    bool mInvokeDone          = false;
    bool mReady               = false;
    bool mOfferSent           = false;
    bool mSignalingBusy       = false;
    bool mAnswerApplied       = false;
    bool mPlaying             = false;
    bool mCheckingPrivacy     = false;
    bool mStopping            = false;
    bool mComplete            = true;
    bool mReconnectAttempted  = false;
    bool mConnecting          = false;
    bool mEndAttempted        = false;
    bool mDeallocateAttempted = false;
    bool mOwnFactory          = false;
    bool mCleaned             = false;
    bool mAccessOwned         = false;
};

} // namespace CameraController
