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

#include <pw_unit_test/framework.h>

#include <inet/InetConfig.h>
#include <system/SystemConfig.h>

#if CHIP_SYSTEM_CONFIG_USE_SOCKETS && INET_CONFIG_ENABLE_TCP_ENDPOINT

#include <app/MessageDef/InvokeRequestMessage.h>
#include <app/MessageDef/InvokeResponseMessage.h>
#include <app/data-model/Decode.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/server-cluster/testing/FabricTestFixture.h>
#include <controller/webrtc/CameraProviderClient.h>
#include <controller/webrtc/CameraRequestorEndpoint.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/tests/CHIPCert_test_vectors.h>
#include <crypto/DefaultSessionKeystore.h>
#include <crypto/RandUtils.h>
#include <inet/TCPEndPointImpl.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <messaging/ExchangeMgr.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/CASESession.h>
#include <protocols/secure_channel/MessageCounterManager.h>
#include <system/SystemLayerImpl.h>
#include <system/TLVPacketBufferBackingStore.h>
#include <transport/SessionManager.h>
#include <transport/TransportMgr.h>
#include <transport/raw/TCP.h>
#include <transport/raw/tests/TCPBaseTestAccess.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {
using namespace chip;
using namespace chip::app;
namespace Provider  = Clusters::WebRTCTransportProvider;
namespace Requestor = Clusters::WebRTCTransportRequestor;
using Cluster       = Controller::CameraSignalingCluster;
using Client        = Controller::CameraProviderClient;
using TCP           = Transport::TCP<2, 4>;
using TCPAccess     = Transport::TCPBaseTestAccess<2, 4>;

constexpr NodeId kControllerNode     = 0xDEDEDEDE00010002;
constexpr NodeId kCameraNode         = 0xDEDEDEDE00010001;
constexpr EndpointId kCameraEndpoint = 3;

// Shared injected keys exercise encryption and real TCP, not the CASE handshake or a media backend.
class CameraTransportTests : public ::testing::Test,
                             public Messaging::UnsolicitedMessageHandler,
                             public Messaging::ExchangeDelegate,
                             public DataModel::EventsGenerator,
                             public DataModel::ActionContext,
                             public Access::AccessControl::Delegate,
                             public Access::AccessControl::DeviceTypeResolver
{
public:
    static void SetUpTestSuite() { ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }

    void SetUp() override
    {
        ASSERT_TRUE(system.Init() == CHIP_NO_ERROR);
        ASSERT_TRUE(endpoints.Init(system) == CHIP_NO_ERROR);
        ASSERT_TRUE(fabrics.SetUpTestFabric(fabricIndex) == CHIP_NO_ERROR);
        ASSERT_TRUE(transports.Init(&tcp) == CHIP_NO_ERROR);
        ASSERT_TRUE(sessions.Init(&system, &transports, &counters, &storage, &fabrics.GetFabricTable(), keystore) == CHIP_NO_ERROR);
        ASSERT_TRUE(exchanges.Init(&sessions) == CHIP_NO_ERROR);
        ASSERT_TRUE(counters.Init(&exchanges) == CHIP_NO_ERROR);
        ASSERT_TRUE(exchanges.RegisterUnsolicitedMessageHandlerForType(Protocols::InteractionModel::MsgType::InvokeCommandRequest,
                                                                       this) == CHIP_NO_ERROR);
        ASSERT_TRUE(attributeStorage.Init(&storage) == CHIP_NO_ERROR);
        ASSERT_TRUE(access.Init(this, *this) == CHIP_NO_ERROR);
        ASSERT_TRUE(provider.Startup(context) == CHIP_NO_ERROR);
        requestor = std::make_unique<Controller::CameraRequestorEndpoint>(7, Span<const DataModel::DeviceTypeEntry>(types),
                                                                          fabrics.GetFabricTable(), system);
        ASSERT_TRUE(requestor->Register(provider, access) == CHIP_NO_ERROR);
        cluster = &requestor->Signaling();
    }

    void TearDown() override
    {
        client.reset();
        if (requestor)
        {
            EXPECT_TRUE(requestor->Unregister() == CHIP_NO_ERROR);
            requestor.reset();
        }
        EXPECT_TRUE(provider.Shutdown() == CHIP_NO_ERROR);
        access.Finish();
        controllerSession.Release();
        cameraSession.Release();
        connection = nullptr;
        counters.Shutdown();
        exchanges.Shutdown();
        sessions.Shutdown();
        transports.Close();
        tcp.Close();
        EXPECT_TRUE(fabrics.TearDownTestFabric(fabricIndex) == CHIP_NO_ERROR);
        endpoints.Shutdown();
        system.Shutdown();
    }

    template <class Predicate>
    bool PumpUntil(Predicate predicate)
    {
        bool timedOut    = false;
        const auto error = system.StartTimer(System::Clock::Seconds16(5), Timeout, &timedOut);
        if (error != CHIP_NO_ERROR)
        {
            ADD_FAILURE() << "Could not start transport test deadline";
            return false;
        }
        while (!predicate() && !timedOut)
        {
            system.PrepareEvents();
            system.WaitForEvents();
            system.HandleEvents();
        }
        system.CancelTimer(Timeout, &timedOut);
        return predicate();
    }

    CHIP_ERROR ConnectSocket(Inet::IPAddressType type, bool establishConnection = true)
    {
        uint16_t port    = 0;
        CHIP_ERROR error = CHIP_ERROR_INCORRECT_STATE;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            port            = static_cast<uint16_t>(20000 + Crypto::GetRandU16() % 40000);
            auto parameters = Transport::TcpListenParameters(&endpoints).SetAddressType(type).SetListenPort(port);
            error           = tcp.Init(parameters);
#if defined(_WIN32)
            if (error != CHIP_ERROR_WINDOWS(WSAEADDRINUSE))
#else
            if (error != CHIP_ERROR_POSIX(EADDRINUSE))
#endif
            {
                break;
            }
        }
        ReturnErrorOnFailure(error);
        peer = Transport::PeerAddress::TCP(Inet::IPAddress::Loopback(type), port);
        if (establishConnection)
        {
            ReturnErrorOnFailure(tcp.TCPConnect(peer, nullptr, connection));
            VerifyOrReturnError(PumpUntil([&] { return connection && connection->IsConnected(); }), CHIP_ERROR_TIMEOUT);
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Connect(Inet::IPAddressType type)
    {
        ReturnErrorOnFailure(ConnectSocket(type));
        ReturnErrorOnFailure(sessions.InjectCaseSessionWithTestKey(controllerSession, 1, 2, kControllerNode, kCameraNode,
                                                                   fabricIndex, peer, CryptoContext::SessionRole::kInitiator));
        ReturnErrorOnFailure(sessions.InjectCaseSessionWithTestKey(cameraSession, 2, 1, kCameraNode, kControllerNode, fabricIndex,
                                                                   peer, CryptoContext::SessionRole::kResponder));
        controllerSession->AsSecureSession()->SetTCPConnection(connection);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR Create()
    {
        ReturnErrorOnFailure(cluster->CreateSession(ScopedNodeId(kCameraNode, fabricIndex), kCameraEndpoint,
                                                    DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(), handle));
        client = std::make_unique<Client>(*cluster, handle);
        return CHIP_NO_ERROR;
    }

    std::optional<Client::Result> AwaitResult()
    {
        std::optional<Client::Result> result;
        EXPECT_TRUE(PumpUntil([&] {
            if (!result.has_value())
            {
                result = client->TakeResult();
            }
            return result.has_value();
        }));
        return result;
    }

    class IncomingResponse : public CommandSender::ExtendableCallback
    {
    public:
        explicit IncomingResponse(const ConcreteCommandPath & path) : expectedPath(path) {}
        void OnResponse(CommandSender *, const CommandSender::ResponseData & response) override
        {
            EXPECT_TRUE(response.path == expectedPath);
            EXPECT_EQ(response.data, nullptr);
            error = response.statusIB.ToChipError();
        }
        void OnError(const CommandSender *, const CommandSender::ErrorData & response) override { error = response.error; }
        void OnNoResponse(CommandSender *, const CommandSender::NoResponseData &) override { error = CHIP_ERROR_TIMEOUT; }
        void OnDone(CommandSender *) override { done = true; }
        ConcreteCommandPath expectedPath;
        CHIP_ERROR error = CHIP_ERROR_INCORRECT_STATE;
        bool done        = false;
    };

    template <class Command>
    CHIP_ERROR SendIncoming(const Command & command)
    {
        IncomingResponse response({ 7, Requestor::Id, Command::GetCommandId() });
        CommandSender sender(&response, &exchanges, false, false, true);
        const CommandPathParams path{ 7, Requestor::Id, Command::GetCommandId(), CommandPathFlags::kEndpointIdValid };
        ReturnErrorOnFailure(sender.AddRequestData(path, command));
        ReturnErrorOnFailure(sender.SendCommandRequest(cameraSession.Get().Value(),
                                                       MakeOptional<System::Clock::Timeout>(System::Clock::Seconds16(5))));
        VerifyOrReturnError(PumpUntil([&] { return response.done; }), CHIP_ERROR_TIMEOUT);
        return response.error;
    }

    CHIP_ERROR OnUnsolicitedMessageReceived(const PayloadHeader &, const SessionHandle & session,
                                            Messaging::ExchangeDelegate *& delegate) override
    {
        VerifyOrReturnError(session->IsSecureSession() && session->AllowsLargePayload() &&
                                (session->GetPeer() == ScopedNodeId(kControllerNode, cameraFabricIndex) ||
                                 session->GetPeer() == ScopedNodeId(kCameraNode, fabricIndex)),
                            CHIP_ERROR_ACCESS_DENIED);
        delegate = this;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR OnMessageReceived(Messaging::ExchangeContext * exchange, const PayloadHeader & header,
                                 System::PacketBufferHandle && payload) override
    {
        const auto error = Receive(exchange, header, std::move(payload));
        EXPECT_TRUE(error == CHIP_NO_ERROR);
        return error;
    }

    void OnResponseTimeout(Messaging::ExchangeContext *) override { ADD_FAILURE() << "Unexpected provider timeout"; }
    Messaging::ExchangeContext * CurrentExchange() override { return nullptr; }
    void ScheduleUrgentEventDeliverySync(std::optional<FabricIndex>) override {}
    CHIP_ERROR GenerateEvent(EventLoggingDelegate *, const EventOptions &, EventNumber &) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    System::Clock::Milliseconds64 GetMonotonicStartupTime() const override { return System::Clock::Milliseconds64(0); }
    bool IsDeviceTypeOnEndpoint(DeviceTypeId type, EndpointId endpoint) override
    {
        return endpoint == 7 && type == types[0].deviceTypeId;
    }
    CHIP_ERROR Check(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                     Access::Privilege privilege) override
    {
        return requestor ? requestor->CheckAccess(subject, path, privilege) : CHIP_ERROR_ACCESS_DENIED;
    }

    CHIP_ERROR Receive(Messaging::ExchangeContext * exchange, const PayloadHeader & header, System::PacketBufferHandle && payload)
    {
        VerifyOrReturnError(header.HasMessageType(Protocols::InteractionModel::MsgType::InvokeCommandRequest),
                            CHIP_ERROR_INVALID_MESSAGE_TYPE);
        lastMessageBytes = payload->TotalLength();
        System::PacketBufferTLVReader reader;
        reader.Init(std::move(payload));
        InvokeRequestMessage::Parser invoke;
        ReturnErrorOnFailure(invoke.Init(reader));
        InvokeRequests::Parser requests;
        ReturnErrorOnFailure(invoke.GetInvokeRequests(&requests));
        TLV::TLVReader list;
        requests.GetReader(&list);
        ReturnErrorOnFailure(list.Next());
        CommandDataIB::Parser command;
        ReturnErrorOnFailure(command.Init(list));
        CommandPathIB::Parser path;
        ReturnErrorOnFailure(command.GetPath(&path));
        EndpointId endpoint = kInvalidEndpointId;
        ClusterId clusterId = kInvalidClusterId;
        CommandId commandId = kInvalidCommandId;
        ReturnErrorOnFailure(path.GetEndpointId(&endpoint));
        ReturnErrorOnFailure(path.GetClusterId(&clusterId));
        ReturnErrorOnFailure(path.GetCommandId(&commandId));
        TLV::TLVReader fields;
        ReturnErrorOnFailure(command.GetFields(&fields));
        const ConcreteCommandPath commandPath(endpoint, clusterId, commandId);
        if (clusterId == Requestor::Id)
        {
            const auto subject = exchange->GetSessionHandle()->GetSubjectDescriptor();
            const Access::RequestPath accessPath{ clusterId, endpoint, Access::RequestType::kCommandInvokeRequest, commandId };
            StatusIB status(Protocols::InteractionModel::Status::UnsupportedAccess);
            if (access.Check(subject, accessPath, Access::Privilege::kOperate) == CHIP_NO_ERROR)
            {
                DataModel::InvokeRequest request(commandPath, subject);
                const auto result = provider.InvokeCommand(request, fields, nullptr);
                VerifyOrReturnError(result.has_value(), CHIP_ERROR_INCORRECT_STATE);
                status = StatusIB(result->GetStatusCode());
            }
            ++receivedRequestorCommands;
            return Respond(exchange, commandPath, status);
        }
        VerifyOrReturnError(endpoint == kCameraEndpoint && clusterId == Provider::Id &&
                                exchange->GetSessionHandle()->GetPeer() == ScopedNodeId(kControllerNode, cameraFabricIndex),
                            CHIP_ERROR_INVALID_ARGUMENT);
        if (commandId == Provider::Commands::ProvideOffer::Id)
        {
            Provider::Commands::ProvideOffer::DecodableType offer;
            ReturnErrorOnFailure(offer.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(offer.sdp.data_equal(CharSpan(expectedSdp.data(), expectedSdp.size())) &&
                                    offer.originatingEndpointID == 7 && offer.webRTCSessionID.IsNull(),
                                CHIP_ERROR_INVALID_ARGUMENT);
        }
        else if (commandId == Provider::Commands::ProvideICECandidates::Id)
        {
            Provider::Commands::ProvideICECandidates::DecodableType candidates;
            ReturnErrorOnFailure(candidates.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(candidates.webRTCSessionID == protocolId, CHIP_ERROR_INVALID_ARGUMENT);
            auto iterator = candidates.ICECandidates.begin();
            size_t index  = 0;
            while (iterator.Next())
            {
                VerifyOrReturnError(index < expectedCandidates.size(), CHIP_ERROR_INVALID_ARGUMENT);
                const auto & value    = iterator.GetValue();
                const auto & expected = expectedCandidates[index++];
                VerifyOrReturnError(value.candidate.data_equal(CharSpan(expected.candidate.data(), expected.candidate.size())),
                                    CHIP_ERROR_INVALID_ARGUMENT);
            }
            ReturnErrorOnFailure(iterator.GetStatus());
            VerifyOrReturnError(index == expectedCandidates.size(), CHIP_ERROR_INVALID_ARGUMENT);
        }
        else if (commandId == Provider::Commands::EndSession::Id)
        {
            Provider::Commands::EndSession::DecodableType end;
            ReturnErrorOnFailure(end.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(end.webRTCSessionID == protocolId &&
                                    end.reason == Clusters::Globals::WebRTCEndReasonEnum::kUserHangup,
                                CHIP_ERROR_INVALID_ARGUMENT);
        }
        else
        {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        VerifyOrReturnError(list.Next() == CHIP_END_OF_TLV, CHIP_ERROR_INVALID_ARGUMENT);
        ++receivedCommands;
        if (!reply)
        {
            return CHIP_NO_ERROR;
        }
        return Respond(exchange, commandPath);
    }

    CHIP_ERROR Respond(Messaging::ExchangeContext * exchange, const ConcreteCommandPath & path,
                       const StatusIB & statusCode = StatusIB(Protocols::InteractionModel::Status::Success))
    {
        auto payload = System::PacketBufferHandle::New(System::PacketBuffer::kMaxSize);
        VerifyOrReturnError(!payload.IsNull(), CHIP_ERROR_NO_MEMORY);
        System::PacketBufferTLVWriter writer;
        writer.Init(std::move(payload));
        InvokeResponseMessage::Builder message;
        ReturnErrorOnFailure(message.Init(&writer));
        message.SuppressResponse(true);
        auto & responses = message.CreateInvokeResponses();
        auto & response  = responses.CreateInvokeResponse();
        if (path.mClusterId == Provider::Id && path.mCommandId == Provider::Commands::ProvideOffer::Id)
        {
            auto & data = response.CreateCommand();
            ReturnErrorOnFailure(data.CreatePath()
                                     .EndpointId(kCameraEndpoint)
                                     .ClusterId(Provider::Id)
                                     .CommandId(Provider::Commands::ProvideOfferResponse::Id)
                                     .EndOfCommandPathIB());
            Provider::Commands::ProvideOfferResponse::Type offerResponse;
            offerResponse.webRTCSessionID = protocolId;
            DataModel::FabricAwareTLVWriter fabricWriter(*data.GetWriter(), exchange->GetSessionHandle()->GetFabricIndex());
            ReturnErrorOnFailure(offerResponse.Encode(fabricWriter, TLV::ContextTag(to_underlying(CommandDataIB::Tag::kFields))));
            ReturnErrorOnFailure(data.EndOfCommandDataIB());
        }
        else
        {
            auto & status = response.CreateStatus();
            ReturnErrorOnFailure(status.CreatePath()
                                     .EndpointId(path.mEndpointId)
                                     .ClusterId(path.mClusterId)
                                     .CommandId(path.mCommandId)
                                     .EndOfCommandPathIB());
            status.CreateErrorStatus().EncodeStatusIB(statusCode);
            ReturnErrorOnFailure(status.EndOfCommandStatusIB());
        }
        ReturnErrorOnFailure(response.EndOfInvokeResponseIB());
        ReturnErrorOnFailure(responses.EndOfInvokeResponses());
        ReturnErrorOnFailure(message.EndOfInvokeResponseMessage());
        ReturnErrorOnFailure(writer.Finalize(&payload));
        return exchange->SendMessage(Protocols::InteractionModel::MsgType::InvokeCommandResponse, std::move(payload));
    }

    static void Timeout(System::Layer *, void * context) { *static_cast<bool *>(context) = true; }

    TestPersistentStorageDelegate storage;
    Testing::FabricTestFixture fabrics{ &storage };
    FabricIndex fabricIndex       = 1;
    FabricIndex cameraFabricIndex = 1;
    System::LayerImpl system;
    Inet::TCPEndPointManagerImpl endpoints;
    TCP tcp;
    TransportMgrBase transports;
    Crypto::DefaultSessionKeystore keystore;
    SessionManager sessions;
    secure_channel::MessageCounterManager counters;
    Messaging::ExchangeManager exchanges;
    Transport::ActiveTCPConnectionHandle connection;
    Transport::PeerAddress peer;
    SessionHolder controllerSession;
    SessionHolder cameraSession;
    DefaultAttributePersistenceProvider attributeStorage;
    DataModel::InteractionModelContext context{ *this, *this };
    CodeDrivenDataModelProvider provider{ storage, attributeStorage };
    Access::AccessControl access;
    const DataModel::DeviceTypeEntry types[1]{ { 0x0147, 1 } };
    std::unique_ptr<Controller::CameraRequestorEndpoint> requestor;
    Cluster * cluster = nullptr;
    std::unique_ptr<Client> client;
    uint64_t handle                  = 0;
    uint16_t protocolId              = 1;
    size_t receivedCommands          = 0;
    size_t receivedRequestorCommands = 0;
    size_t lastMessageBytes          = 0;
    bool reply                       = true;
    std::string expectedSdp;
    std::array<Cluster::Candidate, Cluster::kMaxCandidates> expectedCandidates;
};

class CameraCaseTransportTests : public CameraTransportTests
{
public:
    static void SetUpTestSuite()
    {
        CameraTransportTests::SetUpTestSuite();
#if defined(_WIN32)
        std::error_code error;
        storageDirectory = std::filesystem::temp_directory_path(error);
        ASSERT_FALSE(error);
        storageDirectory /= "matter-camera-case-" + std::to_string(Crypto::GetRandU64());
        ASSERT_TRUE(std::filesystem::create_directory(storageDirectory, error));
        ASSERT_FALSE(error);
        ASSERT_TRUE(DeviceLayer::ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(
                        storageDirectory.string().c_str()) == CHIP_NO_ERROR);
#endif
        ASSERT_TRUE(DeviceLayer::PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    }

    static void TearDownTestSuite()
    {
        DeviceLayer::PlatformMgr().Shutdown();
#if defined(_WIN32)
        std::error_code error;
        std::filesystem::remove_all(storageDirectory, error);
        EXPECT_FALSE(error);
#endif
        CameraTransportTests::TearDownTestSuite();
    }

    void SetUp() override
    {
        CameraTransportTests::SetUp();
        DeviceLayer::SetSystemLayerForTesting(&system);
        groups.SetStorageDelegate(&storage);
        groups.SetSessionKeystore(&keystore);
        ASSERT_TRUE(groups.Init() == CHIP_NO_ERROR);
    }

    void TearDown() override
    {
        groups.Finish();
        DeviceLayer::SetSystemLayerForTesting(nullptr);
        CameraTransportTests::TearDown();
    }

    CHIP_ERROR AddIdentity(ByteSpan noc, ByteSpan icac, ByteSpan publicKey, ByteSpan privateKey, FabricIndex & index,
                           uint8_t ipkByte)
    {
        Crypto::P256SerializedKeypair key;
        VerifyOrReturnError(publicKey.size() + privateKey.size() <= key.Capacity(), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(key.Bytes(), publicKey.data(), publicKey.size());
        memcpy(key.Bytes() + publicKey.size(), privateKey.data(), privateKey.size());
        ReturnErrorOnFailure(key.SetLength(publicKey.size() + privateKey.size()));
        auto & table = fabrics.GetFabricTable();
        ReturnErrorOnFailure(table.AddNewFabricForTestIgnoringCollisions(TestCerts::sTestCert_Root01_Chip, icac, noc,
                                                                         ByteSpan(key.ConstBytes(), key.Length()), &index));
        ReturnErrorOnFailure(table.CommitPendingFabricData());
        const auto * identity = table.FindFabricWithIndex(index);
        VerifyOrReturnError(identity != nullptr, CHIP_ERROR_INVALID_FABRIC_INDEX);
        uint8_t compressedId[sizeof(uint64_t)];
        MutableByteSpan compressedIdSpan(compressedId);
        ReturnErrorOnFailure(identity->GetCompressedFabricIdBytes(compressedIdSpan));
        Credentials::GroupDataProvider::KeySet keys(Credentials::GroupDataProvider::kIdentityProtectionKeySetId,
                                                    Credentials::GroupDataProvider::SecurityPolicy::kTrustFirst, 1);
        keys.epoch_keys[0].start_time = 0;
        memset(keys.epoch_keys[0].key, ipkByte, sizeof(keys.epoch_keys[0].key));
        return groups.SetKeySet(index, compressedIdSpan, keys);
    }

    struct HandshakeProgress
    {
        size_t completed = 0;
        CHIP_ERROR error = CHIP_NO_ERROR;
        bool timedOut    = false;
    };

    class Completion : public SessionEstablishmentDelegate
    {
    public:
        Completion(SessionHolder & session, HandshakeProgress & progress) : mSession(session), mProgress(progress) {}
        void OnSessionEstablishmentError(CHIP_ERROR error) override
        {
            mProgress.error = error;
            LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
        }
        void OnSessionEstablished(const SessionHandle & session) override
        {
            mSession.Grab(session);
            if (++mProgress.completed == 2)
            {
                LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
            }
        }

    private:
        SessionHolder & mSession;
        HandshakeProgress & mProgress;
    };

    CHIP_ERROR ConnectAuthenticated(Inet::IPAddressType type, bool matchingIpk = true)
    {
        ReturnErrorOnFailure(AddIdentity(TestCerts::sTestCert_Node01_02_Chip, ByteSpan(), TestCerts::sTestCert_Node01_02_PublicKey,
                                         TestCerts::sTestCert_Node01_02_PrivateKey, fabricIndex, 0));
        ReturnErrorOnFailure(AddIdentity(TestCerts::sTestCert_Node01_01_Chip, TestCerts::sTestCert_ICA01_Chip,
                                         TestCerts::sTestCert_Node01_01_PublicKey, TestCerts::sTestCert_Node01_01_PrivateKey,
                                         cameraFabricIndex, matchingIpk ? 0 : 1));
        ReturnErrorOnFailure(ConnectSocket(type, false));
        HandshakeProgress progress;
        Completion controllerCompletion(controllerSession, progress);
        Completion cameraCompletion(cameraSession, progress);
        CASESession initiator;
        CASESession responder;
        initiator.SetGroupDataProvider(&groups);
        responder.SetGroupDataProvider(&groups);
        ReturnErrorOnFailure(
            exchanges.RegisterUnsolicitedMessageHandlerForType(Protocols::SecureChannel::MsgType::CASE_Sigma1, &responder));
        auto unregister = ScopeExit([&] {
            LogErrorOnFailure(exchanges.UnregisterUnsolicitedMessageHandlerForType(Protocols::SecureChannel::MsgType::CASE_Sigma1));
        });
        ReturnErrorOnFailure(responder.PrepareForSessionEstablishment(sessions, &fabrics.GetFabricTable(), nullptr, nullptr,
                                                                      &cameraCompletion, ScopedNodeId(), NullOptional));
        auto unauthenticated = sessions.CreateUnauthenticatedSession(peer, GetDefaultMRPConfig());
        VerifyOrReturnError(unauthenticated.HasValue(), CHIP_ERROR_NO_MEMORY);
        auto * exchange = exchanges.NewContext(unauthenticated.Value(), &initiator);
        VerifyOrReturnError(exchange != nullptr, CHIP_ERROR_NO_MEMORY);
        ReturnErrorOnFailure(initiator.EstablishSession(sessions, &fabrics.GetFabricTable(), ScopedNodeId(kCameraNode, fabricIndex),
                                                        exchange, nullptr, nullptr, &controllerCompletion, NullOptional));
        ReturnErrorOnFailure(system.StartTimer(System::Clock::Seconds16(5), HandshakeTimeout, &progress));
        DeviceLayer::PlatformMgr().RunEventLoop();
        system.CancelTimer(HandshakeTimeout, &progress);
        initiator.Clear();
        responder.Clear();
        VerifyOrReturnError(!progress.timedOut, CHIP_ERROR_TIMEOUT);
        ReturnErrorOnFailure(progress.error);
        VerifyOrReturnError(progress.completed == 2 && controllerSession && cameraSession, CHIP_ERROR_INCORRECT_STATE);
        return CHIP_NO_ERROR;
    }

    static void HandshakeTimeout(System::Layer *, void * context)
    {
        static_cast<HandshakeProgress *>(context)->timedOut = true;
        LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
    }

    Credentials::GroupDataProviderImpl groups;
#if defined(_WIN32)
    static inline std::filesystem::path storageDirectory;
#endif
};

TEST_F(CameraCaseTransportTests, AuthenticatedLargeOfferAnswerAndEndOverIPv6)
{
    ASSERT_TRUE(ConnectAuthenticated(Inet::IPAddressType::kIPv6) == CHIP_NO_ERROR);
    ASSERT_TRUE(controllerSession->GetPeer() == ScopedNodeId(kCameraNode, fabricIndex));
    ASSERT_TRUE(cameraSession->GetPeer() == ScopedNodeId(kControllerNode, cameraFabricIndex));
    ASSERT_TRUE(controllerSession->AllowsLargePayload());
    ASSERT_TRUE(cameraSession->AllowsLargePayload());
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = std::string(Cluster::kMaxSdpBytes, 'a');
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    auto result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->error == CHIP_NO_ERROR);
    EXPECT_GT(lastMessageBytes, Cluster::kMaxSdpBytes);
    Requestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = protocolId;
    answer.sdp             = CharSpan(expectedSdp.data(), expectedSdp.size());
    ASSERT_TRUE(SendIncoming(answer) == CHIP_NO_ERROR);
    ASSERT_TRUE(cluster->IsNegotiated(handle));
    const auto signal = cluster->TakeSignal();
    ASSERT_TRUE(signal.has_value());
    EXPECT_EQ(signal->sdp, expectedSdp);
    ASSERT_TRUE(client->SendEnd(exchanges, controllerSession.Get().Value(), Clusters::Globals::WebRTCEndReasonEnum::kUserHangup) ==
                CHIP_NO_ERROR);
    result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_NO_ERROR);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraCaseTransportTests, MismatchedIdentityProtectionKeyRejectsHandshake)
{
    EXPECT_TRUE(ConnectAuthenticated(Inet::IPAddressType::kIPv4, false) == CHIP_ERROR_KEY_NOT_FOUND);
    EXPECT_FALSE(controllerSession);
    EXPECT_FALSE(cameraSession);
    EXPECT_EQ(receivedCommands, 0u);
    EXPECT_EQ(receivedRequestorCommands, 0u);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraCaseTransportTests, AuthenticatedConnectionLossCompletesAnOutstandingCommandOverIPv4)
{
    ASSERT_TRUE(ConnectAuthenticated(Inet::IPAddressType::kIPv4) == CHIP_NO_ERROR);
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\n";
    reply       = false;
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    ASSERT_TRUE(PumpUntil([&] { return receivedCommands == 1; }));
    auto remotePeer       = cameraSession->AsSecureSession()->GetPeerAddress();
    auto remoteConnection = TCPAccess::FindActiveConnection(tcp, remotePeer);
    ASSERT_TRUE(remoteConnection);
    TCPAccess::GetEndpoint(remoteConnection)->Abort();
    const auto result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_ERROR_TIMEOUT);
    Cluster::SessionState state;
    ASSERT_TRUE(cluster->GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, Cluster::SessionState::Failed);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraTransportTests, LargeOfferAndCandidateBatchRoundTripOverIPv6)
{
    ASSERT_TRUE(Connect(Inet::IPAddressType::kIPv6) == CHIP_NO_ERROR);
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\no=- 1 1 IN IP6 ::1\r\ns=-\r\nt=0 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n";
    while (expectedSdp.size() < Cluster::kMaxSdpBytes)
    {
        expectedSdp += "a=candidate:1 1 UDP 2130706431 ::1 5000 typ host\r\n";
    }
    expectedSdp.resize(Cluster::kMaxSdpBytes);
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    auto result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_NO_ERROR);
    EXPECT_GT(lastMessageBytes, Cluster::kMaxSdpBytes);
    for (auto & candidate : expectedCandidates)
    {
        candidate.candidate = "candidate:1 1 UDP 2130706431 ::1 5000 typ host";
        candidate.candidate.resize(Cluster::kMaxCandidateBytes, 'a');
        candidate.mid       = "video";
        candidate.lineIndex = 0;
    }
    ASSERT_TRUE(client->SendCandidates(exchanges, controllerSession.Get().Value(),
                                       Span<const Cluster::Candidate>(expectedCandidates)) == CHIP_NO_ERROR);
    result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_NO_ERROR);
    EXPECT_GT(lastMessageBytes, Cluster::kMaxCandidateBytes * Cluster::kMaxCandidates);
    std::string answerSdp = expectedSdp;
    Requestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = protocolId;
    answer.sdp             = CharSpan(answerSdp.data(), answerSdp.size());
    ASSERT_TRUE(SendIncoming(answer) == CHIP_NO_ERROR);
    EXPECT_GT(lastMessageBytes, Cluster::kMaxSdpBytes);
    answerSdp.assign(answerSdp.size(), 'x');
    auto signal = cluster->TakeSignal();
    ASSERT_TRUE(signal.has_value());
    EXPECT_EQ(signal->sdp, expectedSdp);
    EXPECT_EQ(signal->handle, handle);
    EXPECT_TRUE(cluster->IsNegotiated(handle));

    std::array<Clusters::Globals::Structs::ICECandidateStruct::Type, Cluster::kMaxCandidates> candidates;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        candidates[i].candidate = CharSpan(expectedCandidates[i].candidate.data(), expectedCandidates[i].candidate.size());
        candidates[i].SDPMid.SetNonNull("video"_span);
        candidates[i].SDPMLineIndex.SetNonNull(0);
    }
    Requestor::Commands::ICECandidates::Type incoming;
    incoming.webRTCSessionID = protocolId;
    incoming.ICECandidates   = DataModel::List<const Clusters::Globals::Structs::ICECandidateStruct::Type>(candidates);
    ASSERT_TRUE(SendIncoming(incoming) == CHIP_NO_ERROR);
    EXPECT_GT(lastMessageBytes, Cluster::kMaxCandidateBytes * Cluster::kMaxCandidates);
    signal = cluster->TakeSignal();
    ASSERT_TRUE(signal.has_value());
    ASSERT_EQ(signal->candidates.size(), expectedCandidates.size());
    for (size_t i = 0; i < signal->candidates.size(); ++i)
    {
        EXPECT_EQ(signal->candidates[i].candidate, expectedCandidates[i].candidate);
        EXPECT_EQ(signal->candidates[i].mid, expectedCandidates[i].mid);
        EXPECT_EQ(signal->candidates[i].lineIndex, expectedCandidates[i].lineIndex);
    }
    ASSERT_TRUE(client->SendEnd(exchanges, controllerSession.Get().Value(), Clusters::Globals::WebRTCEndReasonEnum::kUserHangup) ==
                CHIP_NO_ERROR);
    result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_NO_ERROR);
    EXPECT_EQ(receivedCommands, 3u);
    EXPECT_EQ(receivedRequestorCommands, 2u);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraTransportTests, RepeatedOfferAndEndOverIPv4DoNotLeakExchanges)
{
    ASSERT_TRUE(Connect(Inet::IPAddressType::kIPv4) == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\n";
    for (uint16_t i = 0; i < 100; ++i)
    {
        protocolId = i;
        ASSERT_TRUE(Create() == CHIP_NO_ERROR);
        ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(),
                                      CharSpan(expectedSdp.data(), expectedSdp.size())) == CHIP_NO_ERROR);
        auto result = AwaitResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->error == CHIP_NO_ERROR);
        Requestor::Commands::Answer::Type answer;
        answer.webRTCSessionID = protocolId;
        answer.sdp             = "v=0\r\n"_span;
        ASSERT_TRUE(SendIncoming(answer) == CHIP_NO_ERROR);
        ASSERT_TRUE(cluster->IsNegotiated(handle));
        auto signal = cluster->TakeSignal();
        ASSERT_TRUE(signal.has_value());
        EXPECT_EQ(signal->handle, handle);
        ASSERT_TRUE(client->SendEnd(exchanges, controllerSession.Get().Value(),
                                    Clusters::Globals::WebRTCEndReasonEnum::kUserHangup) == CHIP_NO_ERROR);
        result = AwaitResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->error == CHIP_NO_ERROR);
        EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
        client.reset();
    }
    EXPECT_EQ(receivedCommands, 200u);
    EXPECT_EQ(receivedRequestorCommands, 100u);
}

TEST_F(CameraTransportTests, IncomingOversizedAnswerAndStaleMessagesAreRejectedOverTcp)
{
    ASSERT_TRUE(Connect(Inet::IPAddressType::kIPv6) == CHIP_NO_ERROR);
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\n";
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    const auto result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->error == CHIP_NO_ERROR);
    const std::string oversized(Cluster::kMaxSdpBytes + 1, 'a');
    Requestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = protocolId;
    answer.sdp             = CharSpan(oversized.data(), oversized.size());
    EXPECT_TRUE(SendIncoming(answer) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    EXPECT_FALSE(cluster->IsNegotiated(handle));
    EXPECT_FALSE(cluster->TakeSignal().has_value());
    answer.sdp = "v=0\r\n"_span;
    ASSERT_TRUE(SendIncoming(answer) == CHIP_NO_ERROR);
    ASSERT_TRUE(cluster->IsNegotiated(handle));
    ASSERT_TRUE(cluster->CloseSession(handle) == CHIP_NO_ERROR);
    EXPECT_TRUE(SendIncoming(answer) == CHIP_IM_GLOBAL_STATUS(UnsupportedAccess));
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraTransportTests, ConnectionLossCompletesAnOutstandingCommand)
{
    ASSERT_TRUE(Connect(Inet::IPAddressType::kIPv6) == CHIP_NO_ERROR);
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\n";
    reply       = false;
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    ASSERT_TRUE(PumpUntil([&] { return receivedCommands == 1; }));
    auto peer             = cameraSession->AsSecureSession()->GetPeerAddress();
    auto remoteConnection = TCPAccess::FindActiveConnection(tcp, peer);
    ASSERT_TRUE(remoteConnection);
    TCPAccess::GetEndpoint(remoteConnection)->Abort();
    const auto result = AwaitResult();
    ASSERT_TRUE(result.has_value());
    // Shared ExchangeContext reports session loss through CommandSender's timeout callback.
    // AwaitResult's five-second deadline ensures this completes before the 30-second command timer.
    EXPECT_TRUE(result->error == CHIP_ERROR_TIMEOUT);
    Cluster::SessionState state;
    ASSERT_TRUE(cluster->GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, Cluster::SessionState::Failed);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(CameraTransportTests, CancellationReleasesAnOutstandingTransportExchange)
{
    ASSERT_TRUE(Connect(Inet::IPAddressType::kIPv6) == CHIP_NO_ERROR);
    ASSERT_TRUE(Create() == CHIP_NO_ERROR);
    expectedSdp = "v=0\r\n";
    reply       = false;
    ASSERT_TRUE(client->SendOffer(exchanges, controllerSession.Get().Value(), CharSpan(expectedSdp.data(), expectedSdp.size())) ==
                CHIP_NO_ERROR);
    ASSERT_TRUE(PumpUntil([&] { return receivedCommands == 1; }));
    client->Cancel();
    auto result = client->TakeResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->error == CHIP_ERROR_CANCELLED);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

} // namespace
#endif // CHIP_SYSTEM_CONFIG_USE_SOCKETS && INET_CONFIG_ENABLE_TCP_ENDPOINT
