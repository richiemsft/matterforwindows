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

#include "commands/common/CHIPCommand.h"

#include <access/AccessControl.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <controller/webrtc/CameraProviderClient.h>
#include <controller/webrtc/CameraRequestorEndpoint.h>

#include <cstdio>
#include <istream>
#include <memory>
#include <optional>
#include <string>

class CameraSignalingAccess : public chip::Access::AccessControl::Delegate
{
public:
    void SetRequestor(chip::Controller::CameraSignalingCluster * requestor, chip::ScopedNodeId peer)
    {
        mRequestor = requestor;
        mPeer      = peer;
    }
    CHIP_ERROR Check(const chip::Access::SubjectDescriptor & subject, const chip::Access::RequestPath & path,
                     chip::Access::Privilege privilege) override;

private:
    chip::Controller::CameraSignalingCluster * mRequestor = nullptr;
    chip::ScopedNodeId mPeer;
};

class CameraSignalingCommand : public CHIPCommand
{
public:
    explicit CameraSignalingCommand(CredentialIssuerCommands * credentials);
    CHIP_ERROR Run() override;
    static CHIP_ERROR ReadOffer(std::istream & input, std::string & offer);

protected:
    CHIP_ERROR RunCommand() override;
    chip::System::Clock::Timeout GetWaitDuration() const override;
    bool NeedsOperationalAdvertising() override { return true; }
    CHIP_ERROR ConfigureFactory(chip::Controller::FactoryInitParams & params) override;
    void Cleanup() override;

private:
    class DeviceTypeResolver : public chip::Access::AccessControl::DeviceTypeResolver
    {
        bool IsDeviceTypeOnEndpoint(chip::DeviceTypeId, chip::EndpointId) override { return false; }
    };
    enum class Phase : uint8_t
    {
        Connecting,
        Offering,
        Ending,
        Complete,
    };

    chip::NodeId mNodeId     = chip::kUndefinedNodeId;
    chip::EndpointId mCamera = chip::kInvalidEndpointId;
    uint16_t mVideoStream    = 0;
    char * mOfferFile        = nullptr;
    chip::Optional<uint16_t> mTimeout;
    chip::Optional<uint16_t> mListenPort;
    chip::Optional<char *> mSignalsFile;
    std::string mOffer;
    std::vector<chip::Controller::CameraSignalingCluster::Signal> mTranscript;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> mOutput{ nullptr, &std::fclose };
    chip::app::DefaultAttributePersistenceProvider mAttributes;
    std::optional<chip::app::CodeDrivenDataModelProvider> mProvider;
    std::optional<chip::Controller::CameraRequestorEndpoint> mRequestor;
    std::unique_ptr<chip::Controller::CameraProviderClient> mClient;
    CameraSignalingAccess mAccess;
    DeviceTypeResolver mDeviceTypes;
    chip::System::Layer * mSystem                 = nullptr;
    chip::Messaging::ExchangeManager * mExchanges = nullptr;
    chip::SessionHolder mSession;
    chip::System::Clock::Timestamp mDeadline;
    uint64_t mHandle       = 0;
    bool mOwnFactory       = false;
    bool mCleanupDone      = false;
    bool mAccessOwned      = false;
    bool mOutputFailed     = false;
    Phase mPhase           = Phase::Complete;
    CHIP_ERROR mFinalError = CHIP_NO_ERROR;
    chip::Callback::Callback<chip::OnDeviceConnected> mConnected{ OnConnected, this };
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mConnectionFailed{ OnConnectionFailed, this };

    static void OnConnected(void * context, chip::Messaging::ExchangeManager & exchanges, const chip::SessionHandle & session);
    static void OnConnectionFailed(void * context, const chip::ScopedNodeId & peer, CHIP_ERROR error);
    static void OnPoll(chip::System::Layer * system, void * context);
    void Poll();
    void BeginEnd(CHIP_ERROR status);
    void Finish(CHIP_ERROR status);
    CHIP_ERROR RecordSignal(chip::Controller::CameraSignalingCluster::Signal && signal);
    CHIP_ERROR WriteTranscript();
};
