/*
 * Copyright (c) 2026 Project CHIP Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "LiveViewCommand.h"

#include <gtest/gtest.h>

#include <app/MessageDef/InvokeRequestMessage.h>
#include <app/MessageDef/InvokeResponseMessage.h>
#include <app/MessageDef/ReadRequestMessage.h>
#include <app/MessageDef/ReportDataMessage.h>
#include <app/data-model/Decode.h>
#include <app/data-model/Encode.h>
#include <app/server-cluster/testing/FabricTestFixture.h>
#include <clusters/CameraAvStreamManagement/Attributes.h>
#include <clusters/Descriptor/Attributes.h>
#include <clusters/WebRTCTransportProvider/Attributes.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/tests/CHIPCert_test_vectors.h>
#include <crypto/DefaultSessionKeystore.h>
#include <crypto/RandUtils.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/secure_channel/CASESession.h>
#include <protocols/secure_channel/MessageCounterManager.h>
#include <system/SystemLayerImpl.h>
#include <system/TLVPacketBufferBackingStore.h>
#include <transport/TransportMgr.h>
#include <transport/raw/TCP.h>

#include <cstring>
#include <filesystem>

namespace CameraController {
using namespace chip;
using namespace chip::app;
namespace Provider                      = Clusters::WebRTCTransportProvider;
namespace Requestor                     = Clusters::WebRTCTransportRequestor;
namespace Descriptor                    = Clusters::Descriptor;
constexpr NodeId kController            = 0xDEDEDEDE00010002;
constexpr NodeId kCamera                = 0xDEDEDEDE00010001;
constexpr EndpointId kCameraEndpoint    = 3;
constexpr EndpointId kRequestorEndpoint = 7;
constexpr uint16_t kVideoStream         = 17;
constexpr uint16_t kProtocolSession     = 23;

class LiveViewFlowTestAccess
{
public:
    static CHIP_ERROR Start(LiveViewCommand & command, PersistentStorageDelegate & storage, FabricTable & fabrics,
                            System::Layer & system, Messaging::ExchangeManager & exchanges, const SessionHandle & session,
                            DataModel::InteractionModelContext & context, uint16_t duration = 1)
    {
        command.mPeer     = session->GetPeer();
        command.mSystem   = &system;
        command.mComplete = false;
        command.mDuration.SetValue(duration);
        command.mStartupDeadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(30);
        ReturnErrorOnFailure(command.mPersistence.Init(&storage));
        command.mProvider.emplace(storage, command.mPersistence);
        ReturnErrorOnFailure(command.mProvider->Startup(context));
        static constexpr DataModel::DeviceTypeEntry types[] = { { 0x0147, 1 } };
        command.mRequestor.emplace(kRequestorEndpoint, Span<const DataModel::DeviceTypeEntry>(types), fabrics, system);
        command.mDeviceTypes.requestor = kRequestorEndpoint;
        Access::AccessControl & access = Access::GetAccessControl();
        command.mAccess.SetRequestor(&*command.mRequestor, command.mPeer);
        ReturnErrorOnFailure(access.Init(&command.mAccess, command.mDeviceTypes));
        command.mAccessOwned = true;
        ReturnErrorOnFailure(command.mRequestor->Register(*command.mProvider, access));
        ReturnErrorOnFailure(command.mMedia.Start());
        ReturnErrorOnFailure(system.StartTimer(System::Clock::Milliseconds32(20), LiveViewCommand::OnPoll, &command));
        LiveViewCommand::Connected(&command, exchanges, session);
        return CHIP_NO_ERROR;
    }
    static bool Complete(const LiveViewCommand & command) { return command.mComplete; }
    static bool Played(const LiveViewCommand & command) { return command.mPlaying && command.mFrames > 0; }
    static CHIP_ERROR Result(const LiveViewCommand & command)
    {
        return command.mFinalError != CHIP_NO_ERROR ? command.mFinalError : command.mCleanupError;
    }
    static CHIP_ERROR Invoke(LiveViewCommand & command, const DataModel::InvokeRequest & request, TLV::TLVReader & data)
    {
        const Access::RequestPath path{ request.path.mClusterId, request.path.mEndpointId,
                                        Access::RequestType::kCommandInvokeRequest, request.path.mCommandId };
        ReturnErrorOnFailure(command.mRequestor->CheckAccess(request.subjectDescriptor, path, Access::Privilege::kOperate));
        const auto response = command.mProvider->InvokeCommand(request, data, nullptr);
        VerifyOrReturnError(response.has_value(), CHIP_ERROR_INCORRECT_STATE);
        return StatusIB(response->GetStatusCode()).ToChipError();
    }
    static void Cleanup(LiveViewCommand & command)
    {
        command.Cleanup();
        if (command.mProvider)
        {
            LogErrorOnFailure(command.mProvider->Shutdown());
            command.mProvider.reset();
        }
        LogErrorOnFailure(command.mMedia.Join());
    }
};

namespace {
// This peer implements the SDK wire contracts, not a conformant or physical camera.
class LiveViewFlowTests : public testing::Test,
                          public Messaging::UnsolicitedMessageHandler,
                          public Messaging::ExchangeDelegate,
                          public DataModel::EventsGenerator,
                          public DataModel::ActionContext
{
public:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR);
        std::error_code error;
        testStorage = std::filesystem::temp_directory_path(error);
        ASSERT_FALSE(error);
        testStorage /= "matter-native-liveview-" + std::to_string(Crypto::GetRandU64());
        ASSERT_TRUE(std::filesystem::create_directory(testStorage, error));
        ASSERT_FALSE(error);
        ASSERT_TRUE(DeviceLayer::ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(
                        testStorage.string().c_str()) == CHIP_NO_ERROR);
        ASSERT_TRUE(DeviceLayer::PlatformMgr().InitChipStack() == CHIP_NO_ERROR);
    }
    static void TearDownTestSuite()
    {
        DeviceLayer::PlatformMgr().Shutdown();
        std::error_code error;
        std::filesystem::remove_all(testStorage, error);
        EXPECT_FALSE(error);
        Platform::MemoryShutdown();
    }
    void SetUp() override
    {
        ASSERT_TRUE(system.Init() == CHIP_NO_ERROR);
        DeviceLayer::SetSystemLayerForTesting(&system);
        ASSERT_TRUE(endpoints.Init(system) == CHIP_NO_ERROR);
        ASSERT_TRUE(fabrics.SetUpTestFabric(dummyFabric) == CHIP_NO_ERROR);
        ASSERT_TRUE(transports.Init(&tcp) == CHIP_NO_ERROR);
        ASSERT_TRUE(sessions.Init(&system, &transports, &counters, &storage, &fabrics.GetFabricTable(), keystore) == CHIP_NO_ERROR);
        ASSERT_TRUE(exchanges.Init(&sessions) == CHIP_NO_ERROR);
        ASSERT_TRUE(counters.Init(&exchanges) == CHIP_NO_ERROR);
        groups.SetStorageDelegate(&storage);
        groups.SetSessionKeystore(&keystore);
        ASSERT_TRUE(groups.Init() == CHIP_NO_ERROR);
        ASSERT_TRUE(exchanges.RegisterUnsolicitedMessageHandlerForType(Protocols::InteractionModel::MsgType::InvokeCommandRequest,
                                                                       this) == CHIP_NO_ERROR);
        ASSERT_TRUE(exchanges.RegisterUnsolicitedMessageHandlerForType(Protocols::InteractionModel::MsgType::ReadRequest, this) ==
                    CHIP_NO_ERROR);
    }
    void TearDown() override
    {
        requestorSender.reset();
        if (command)
        {
            LiveViewFlowTestAccess::Cleanup(*command);
            command.reset();
        }
        providerMedia.RequestStop();
        EXPECT_TRUE(providerMedia.Join() == CHIP_NO_ERROR);
        controllerSession.Release();
        cameraSession.Release();
        groups.Finish();
        counters.Shutdown();
        exchanges.Shutdown();
        sessions.Shutdown();
        transports.Close();
        tcp.Close();
        EXPECT_TRUE(fabrics.TearDownTestFabric(dummyFabric) == CHIP_NO_ERROR);
        endpoints.Shutdown();
        DeviceLayer::SetSystemLayerForTesting(nullptr);
        system.Shutdown();
    }

    CHIP_ERROR AddIdentity(ByteSpan noc, ByteSpan icac, ByteSpan publicKey, ByteSpan privateKey, FabricIndex & index)
    {
        Crypto::P256SerializedKeypair key;
        VerifyOrReturnError(publicKey.size() + privateKey.size() <= key.Capacity(), CHIP_ERROR_BUFFER_TOO_SMALL);
        std::memcpy(key.Bytes(), publicKey.data(), publicKey.size());
        std::memcpy(key.Bytes() + publicKey.size(), privateKey.data(), privateKey.size());
        ReturnErrorOnFailure(key.SetLength(publicKey.size() + privateKey.size()));
        auto & table = fabrics.GetFabricTable();
        ReturnErrorOnFailure(table.AddNewFabricForTestIgnoringCollisions(TestCerts::sTestCert_Root01_Chip, icac, noc,
                                                                         ByteSpan(key.ConstBytes(), key.Length()), &index));
        ReturnErrorOnFailure(table.CommitPendingFabricData());
        uint8_t id[sizeof(uint64_t)];
        MutableByteSpan span(id);
        ReturnErrorOnFailure(table.FindFabricWithIndex(index)->GetCompressedFabricIdBytes(span));
        Credentials::GroupDataProvider::KeySet keys(Credentials::GroupDataProvider::kIdentityProtectionKeySetId,
                                                    Credentials::GroupDataProvider::SecurityPolicy::kTrustFirst, 1);
        keys.epoch_keys[0].start_time = 0;
        std::memset(keys.epoch_keys[0].key, 0, sizeof(keys.epoch_keys[0].key));
        return groups.SetKeySet(index, span, keys);
    }
    struct Handshake
    {
        size_t complete  = 0;
        CHIP_ERROR error = CHIP_NO_ERROR;
    };
    class Established : public SessionEstablishmentDelegate
    {
    public:
        Established(SessionHolder & session, Handshake & progress) : mSession(session), mProgress(progress) {}
        void OnSessionEstablished(const SessionHandle & session) override
        {
            mSession.Grab(session);
            if (++mProgress.complete == 2)
            {
                LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
            }
        }
        void OnSessionEstablishmentError(CHIP_ERROR error) override
        {
            mProgress.error = error;
            LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
        }

    private:
        SessionHolder & mSession;
        Handshake & mProgress;
    };
    static void CaseTimeout(System::Layer *, void * context)
    {
        static_cast<Handshake *>(context)->error = CHIP_ERROR_TIMEOUT;
        LogErrorOnFailure(DeviceLayer::PlatformMgr().StopEventLoopTask());
    }
    CHIP_ERROR Connect()
    {
        ReturnErrorOnFailure(AddIdentity(TestCerts::sTestCert_Node01_02_Chip, ByteSpan(), TestCerts::sTestCert_Node01_02_PublicKey,
                                         TestCerts::sTestCert_Node01_02_PrivateKey, controllerFabric));
        ReturnErrorOnFailure(AddIdentity(TestCerts::sTestCert_Node01_01_Chip, TestCerts::sTestCert_ICA01_Chip,
                                         TestCerts::sTestCert_Node01_01_PublicKey, TestCerts::sTestCert_Node01_01_PrivateKey,
                                         cameraFabric));
        CHIP_ERROR error = CHIP_ERROR_INCORRECT_STATE;
        uint16_t port    = 0;
        for (size_t attempt = 0; attempt < 10; ++attempt)
        {
            port = static_cast<uint16_t>(20000 + Crypto::GetRandU16() % 40000);
            error =
                tcp.Init(Transport::TcpListenParameters(&endpoints).SetAddressType(Inet::IPAddressType::kIPv6).SetListenPort(port));
            if (error != CHIP_ERROR_WINDOWS(WSAEADDRINUSE) && error != CHIP_ERROR_WINDOWS(WSAEACCES))
            {
                break;
            }
        }
        ReturnErrorOnFailure(error);
        Handshake progress;
        Established controllerDone(controllerSession, progress);
        Established cameraDone(cameraSession, progress);
        CASESession initiator, responder;
        initiator.SetGroupDataProvider(&groups);
        responder.SetGroupDataProvider(&groups);
        ReturnErrorOnFailure(
            exchanges.RegisterUnsolicitedMessageHandlerForType(Protocols::SecureChannel::MsgType::CASE_Sigma1, &responder));
        auto unregister = ScopeExit([&] {
            LogErrorOnFailure(exchanges.UnregisterUnsolicitedMessageHandlerForType(Protocols::SecureChannel::MsgType::CASE_Sigma1));
        });
        ReturnErrorOnFailure(responder.PrepareForSessionEstablishment(sessions, &fabrics.GetFabricTable(), nullptr, nullptr,
                                                                      &cameraDone, ScopedNodeId(), NullOptional));
        auto unauthenticated = sessions.CreateUnauthenticatedSession(
            Transport::PeerAddress::TCP(Inet::IPAddress::Loopback(Inet::IPAddressType::kIPv6), port), GetDefaultMRPConfig());
        VerifyOrReturnError(unauthenticated.HasValue(), CHIP_ERROR_NO_MEMORY);
        auto * exchange = exchanges.NewContext(unauthenticated.Value(), &initiator);
        VerifyOrReturnError(exchange != nullptr, CHIP_ERROR_NO_MEMORY);
        ReturnErrorOnFailure(initiator.EstablishSession(sessions, &fabrics.GetFabricTable(),
                                                        ScopedNodeId(kCamera, controllerFabric), exchange, nullptr, nullptr,
                                                        &controllerDone, NullOptional));
        ReturnErrorOnFailure(system.StartTimer(System::Clock::Seconds16(5), CaseTimeout, &progress));
        DeviceLayer::PlatformMgr().RunEventLoop();
        system.CancelTimer(CaseTimeout, &progress);
        initiator.Clear();
        responder.Clear();
        ReturnErrorOnFailure(progress.error);
        VerifyOrReturnError(progress.complete == 2 && controllerSession && cameraSession, CHIP_ERROR_INCORRECT_STATE);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR OnUnsolicitedMessageReceived(const PayloadHeader &, const SessionHandle & session,
                                            Messaging::ExchangeDelegate *& delegate) override
    {
        VerifyOrReturnError(session->IsSecureSession() && session->AllowsLargePayload() &&
                                (session->GetPeer() == ScopedNodeId(kController, cameraFabric) ||
                                 session->GetPeer() == ScopedNodeId(kCamera, controllerFabric)),
                            CHIP_ERROR_ACCESS_DENIED);
        delegate = this;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR OnMessageReceived(Messaging::ExchangeContext * exchange, const PayloadHeader & header,
                                 System::PacketBufferHandle && payload) override
    {
        CHIP_ERROR error = header.HasMessageType(Protocols::InteractionModel::MsgType::ReadRequest)
            ? Read(exchange, std::move(payload))
            : Invoke(exchange, std::move(payload));
        EXPECT_TRUE(error == CHIP_NO_ERROR);
        if (error != CHIP_NO_ERROR)
        {
            wireError = error;
        }
        return error;
    }
    void OnResponseTimeout(Messaging::ExchangeContext *) override { wireError = CHIP_ERROR_TIMEOUT; }
    Messaging::ExchangeContext * CurrentExchange() override { return nullptr; }
    void ScheduleUrgentEventDeliverySync(std::optional<FabricIndex>) override {}
    CHIP_ERROR GenerateEvent(EventLoggingDelegate *, const EventOptions &, EventNumber &) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    System::Clock::Milliseconds64 GetMonotonicStartupTime() const override { return System::Clock::Milliseconds64(0); }

    template <class Value>
    CHIP_ERROR Encode(TLV::TLVWriter & writer, TLV::Tag tag, const Value & value)
    {
        return DataModel::Encode(writer, tag, value);
    }
    template <class Value>
    CHIP_ERROR EncodeList(TLV::TLVWriter & writer, TLV::Tag tag, std::initializer_list<Value> values)
    {
        return Encode(writer, tag, DataModel::List<const Value>(values.begin(), values.size()));
    }
    CHIP_ERROR Attribute(EndpointId endpoint, ClusterId cluster, AttributeId attribute, TLV::TLVWriter & writer, TLV::Tag tag)
    {
        if (cluster == Descriptor::Id)
        {
            if (endpoint == 0 && attribute == Descriptor::Attributes::PartsList::Id)
            {
                if (bridge || multipleCameras)
                {
                    return EncodeList<EndpointId>(writer, tag, { 1, cameraEndpoint });
                }
                return EncodeList<EndpointId>(writer, tag, { cameraEndpoint });
            }
            if (endpoint == 1 && bridge && attribute == Descriptor::Attributes::ServerList::Id)
            {
                return EncodeList<ClusterId>(writer, tag, { Descriptor::Id });
            }
            if ((endpoint == cameraEndpoint || (multipleCameras && endpoint == 1)) &&
                attribute == Descriptor::Attributes::ServerList::Id)
            {
                return EncodeList<ClusterId>(writer, tag, { Descriptor::Id, Av::Id, Provider::Id });
            }
        }
        VerifyOrReturnError(endpoint == cameraEndpoint, CHIP_ERROR_INVALID_ARGUMENT);
        if (attribute == Av::Attributes::ClusterRevision::Id && (cluster == Av::Id || cluster == Provider::Id))
        {
            return Encode(writer, tag, uint16_t(1));
        }
        if (cluster == Provider::Id && attribute == Provider::Attributes::AcceptedCommandList::Id)
        {
            return EncodeList<CommandId>(writer, tag,
                                         { Provider::Commands::ProvideOffer::Id, Provider::Commands::ProvideICECandidates::Id,
                                           Provider::Commands::EndSession::Id });
        }
        VerifyOrReturnError(cluster == Av::Id, CHIP_ERROR_INVALID_ARGUMENT);
        switch (attribute)
        {
        case Av::Attributes::AttributeList::Id:
            return EncodeList<AttributeId>(
                writer, tag,
                { Av::Attributes::FeatureMap::Id, Av::Attributes::VideoSensorParams::Id, Av::Attributes::MinViewportResolution::Id,
                  Av::Attributes::SupportedStreamUsages::Id, Av::Attributes::AllocatedVideoStreams::Id,
                  Av::Attributes::SoftLivestreamPrivacyModeEnabled::Id, Av::Attributes::HardPrivacyModeOn::Id });
        case Av::Attributes::AcceptedCommandList::Id:
            return EncodeList<CommandId>(writer, tag,
                                         { Av::Commands::VideoStreamAllocate::Id, Av::Commands::VideoStreamDeallocate::Id });
        case Av::Attributes::FeatureMap::Id:
            return Encode(writer, tag, static_cast<uint32_t>(Av::Feature::kVideo));
        case Av::Attributes::VideoSensorParams::Id: {
            Av::Structs::VideoSensorParamsStruct::Type sensor;
            sensor.sensorWidth  = 640;
            sensor.sensorHeight = 480;
            sensor.maxFPS       = 30;
            return Encode(writer, tag, sensor);
        }
        case Av::Attributes::MinViewportResolution::Id: {
            Av::Structs::VideoResolutionStruct::Type resolution;
            resolution.width  = 640;
            resolution.height = 480;
            return Encode(writer, tag, resolution);
        }
        case Av::Attributes::SupportedStreamUsages::Id:
            return EncodeList<Clusters::Globals::StreamUsageEnum>(writer, tag, { Clusters::Globals::StreamUsageEnum::kLiveView });
        case Av::Attributes::SoftLivestreamPrivacyModeEnabled::Id:
            return Encode(writer, tag, privacy);
        case Av::Attributes::HardPrivacyModeOn::Id:
            return Encode(writer, tag, hardPrivacy);
        case Av::Attributes::AllocatedVideoStreams::Id:
            VerifyOrReturnError(allocated, CHIP_ERROR_INCORRECT_STATE);
            return EncodeList<Av::Structs::VideoStreamStruct::Type>(writer, tag, { stream });
        default:
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
    }
    CHIP_ERROR Read(Messaging::ExchangeContext * exchange, System::PacketBufferHandle && payload)
    {
        VerifyOrReturnError(exchange->GetSessionHandle()->GetPeer() == ScopedNodeId(kController, cameraFabric),
                            CHIP_ERROR_ACCESS_DENIED);
        System::PacketBufferTLVReader reader;
        reader.Init(std::move(payload));
        ReadRequestMessage::Parser request;
        ReturnErrorOnFailure(request.Init(reader));
        AttributePathIBs::Parser paths;
        ReturnErrorOnFailure(request.GetAttributeRequests(&paths));
        TLV::TLVReader items;
        paths.GetReader(&items);
        ReturnErrorOnFailure(items.Next());
        AttributePathIB::Parser path;
        ReturnErrorOnFailure(path.Init(items));
        EndpointId endpoint;
        ClusterId cluster;
        AttributeId attribute;
        ReturnErrorOnFailure(path.GetEndpoint(&endpoint));
        ReturnErrorOnFailure(path.GetCluster(&cluster));
        ReturnErrorOnFailure(path.GetAttribute(&attribute));
        VerifyOrReturnError(items.Next() == CHIP_END_OF_TLV, CHIP_ERROR_INVALID_ARGUMENT);
        auto output = System::PacketBufferHandle::New(System::PacketBuffer::kMaxSize);
        VerifyOrReturnError(!output.IsNull(), CHIP_ERROR_NO_MEMORY);
        System::PacketBufferTLVWriter writer;
        writer.Init(std::move(output));
        ReportDataMessage::Builder report;
        ReturnErrorOnFailure(report.Init(&writer));
        auto & reports = report.CreateAttributeReportIBs();
        auto & item    = reports.CreateAttributeReport();
        auto & data    = item.CreateAttributeData();
        data.DataVersion(1);
        ReturnErrorOnFailure(data.CreatePath().Endpoint(endpoint).Cluster(cluster).Attribute(attribute).EndOfAttributePathIB());
        ReturnErrorOnFailure(Attribute(endpoint, cluster, attribute, *data.GetWriter(),
                                       TLV::ContextTag(to_underlying(AttributeDataIB::Tag::kData))));
        ReturnErrorOnFailure(data.EndOfAttributeDataIB());
        ReturnErrorOnFailure(item.EndOfAttributeReportIB());
        ReturnErrorOnFailure(reports.EndOfAttributeReportIBs());
        report.SuppressResponse(true);
        ReturnErrorOnFailure(report.EndOfReportDataMessage());
        ReturnErrorOnFailure(writer.Finalize(&output));
        ++reads;
        return exchange->SendMessage(Protocols::InteractionModel::MsgType::ReportData, std::move(output));
    }

    template <class Response>
    CHIP_ERROR Respond(Messaging::ExchangeContext * exchange, const ConcreteCommandPath & path, const Response * response,
                       StatusIB statusIB = StatusIB())
    {
        auto output = System::PacketBufferHandle::New(System::PacketBuffer::kMaxSize);
        VerifyOrReturnError(!output.IsNull(), CHIP_ERROR_NO_MEMORY);
        System::PacketBufferTLVWriter writer;
        writer.Init(std::move(output));
        InvokeResponseMessage::Builder message;
        ReturnErrorOnFailure(message.Init(&writer));
        message.SuppressResponse(true);
        auto & responses = message.CreateInvokeResponses();
        auto & entry     = responses.CreateInvokeResponse();
        if (response != nullptr)
        {
            auto & data = entry.CreateCommand();
            ReturnErrorOnFailure(data.CreatePath()
                                     .EndpointId(path.mEndpointId)
                                     .ClusterId(path.mClusterId)
                                     .CommandId(Response::GetCommandId())
                                     .EndOfCommandPathIB());
            DataModel::FabricAwareTLVWriter fabricWriter(*data.GetWriter(), exchange->GetSessionHandle()->GetFabricIndex());
            ReturnErrorOnFailure(response->Encode(fabricWriter, TLV::ContextTag(to_underlying(CommandDataIB::Tag::kFields))));
            ReturnErrorOnFailure(data.EndOfCommandDataIB());
        }
        else
        {
            auto & status = entry.CreateStatus();
            ReturnErrorOnFailure(status.CreatePath()
                                     .EndpointId(path.mEndpointId)
                                     .ClusterId(path.mClusterId)
                                     .CommandId(path.mCommandId)
                                     .EndOfCommandPathIB());
            status.CreateErrorStatus().EncodeStatusIB(statusIB);
            ReturnErrorOnFailure(status.EndOfCommandStatusIB());
        }
        ReturnErrorOnFailure(entry.EndOfInvokeResponseIB());
        ReturnErrorOnFailure(responses.EndOfInvokeResponses());
        ReturnErrorOnFailure(message.EndOfInvokeResponseMessage());
        ReturnErrorOnFailure(writer.Finalize(&output));
        return exchange->SendMessage(Protocols::InteractionModel::MsgType::InvokeCommandResponse, std::move(output));
    }
    CHIP_ERROR Status(Messaging::ExchangeContext * exchange, const ConcreteCommandPath & path, StatusIB status = StatusIB())
    {
        return Respond<Av::Commands::VideoStreamAllocateResponse::Type>(exchange, path, nullptr, status);
    }

    CHIP_ERROR Invoke(Messaging::ExchangeContext * exchange, System::PacketBufferHandle && payload)
    {
        System::PacketBufferTLVReader reader;
        reader.Init(std::move(payload));
        InvokeRequestMessage::Parser request;
        ReturnErrorOnFailure(request.Init(reader));
        InvokeRequests::Parser requests;
        ReturnErrorOnFailure(request.GetInvokeRequests(&requests));
        TLV::TLVReader items;
        requests.GetReader(&items);
        ReturnErrorOnFailure(items.Next());
        CommandDataIB::Parser data;
        ReturnErrorOnFailure(data.Init(items));
        CommandPathIB::Parser path;
        ReturnErrorOnFailure(data.GetPath(&path));
        EndpointId endpoint;
        ClusterId cluster;
        CommandId id;
        ReturnErrorOnFailure(path.GetEndpointId(&endpoint));
        ReturnErrorOnFailure(path.GetClusterId(&cluster));
        ReturnErrorOnFailure(path.GetCommandId(&id));
        TLV::TLVReader fields;
        ReturnErrorOnFailure(data.GetFields(&fields));
        VerifyOrReturnError(items.Next() == CHIP_END_OF_TLV, CHIP_ERROR_INVALID_ARGUMENT);
        const ConcreteCommandPath commandPath(endpoint, cluster, id);
        if (cluster == Requestor::Id && endpoint == kRequestorEndpoint)
        {
            DataModel::InvokeRequest invocation(commandPath, exchange->GetSessionHandle()->GetSubjectDescriptor());
            ReturnErrorOnFailure(LiveViewFlowTestAccess::Invoke(*command, invocation, fields));
            return Status(exchange, commandPath);
        }
        VerifyOrReturnError(endpoint == cameraEndpoint &&
                                exchange->GetSessionHandle()->GetPeer() == ScopedNodeId(kController, cameraFabric),
                            CHIP_ERROR_ACCESS_DENIED);
        if (cluster == Av::Id && id == Av::Commands::VideoStreamAllocate::Id)
        {
            VerifyOrReturnError(!allocated, CHIP_ERROR_INCORRECT_STATE);
            Av::Commands::VideoStreamAllocate::DecodableType allocation;
            ReturnErrorOnFailure(DataModel::Decode(fields, allocation));
            VerifyOrReturnError(allocation.streamUsage == Clusters::Globals::StreamUsageEnum::kLiveView &&
                                    allocation.videoCodec == Av::VideoCodecEnum::kH264,
                                CHIP_ERROR_INVALID_ARGUMENT);
            stream.videoStreamID    = kVideoStream;
            stream.streamUsage      = allocation.streamUsage;
            stream.videoCodec       = allocation.videoCodec;
            stream.minFrameRate     = allocation.minFrameRate;
            stream.maxFrameRate     = allocation.maxFrameRate;
            stream.minResolution    = allocation.minResolution;
            stream.maxResolution    = allocation.maxResolution;
            stream.minBitRate       = allocation.minBitRate;
            stream.maxBitRate       = allocation.maxBitRate;
            stream.keyFrameInterval = allocation.keyFrameInterval;
            stream.referenceCount   = 0;
            if (invalidMetadata)
            {
                stream.maxResolution.width = 1280;
            }
            allocated = true;
            ++allocations;
            Av::Commands::VideoStreamAllocateResponse::Type response;
            response.videoStreamID = kVideoStream;
            return Respond(exchange, commandPath, &response);
        }
        if (cluster == Av::Id && id == Av::Commands::VideoStreamDeallocate::Id)
        {
            Av::Commands::VideoStreamDeallocate::DecodableType deallocation;
            ReturnErrorOnFailure(DataModel::Decode(fields, deallocation));
            VerifyOrReturnError(allocated && deallocation.videoStreamID == kVideoStream && ends == offers,
                                CHIP_ERROR_INVALID_ARGUMENT);
            allocated = false;
            ++deallocations;
            providerMedia.RequestStop();
            return Status(exchange, commandPath);
        }
        VerifyOrReturnError(cluster == Provider::Id && allocated, CHIP_ERROR_INVALID_ARGUMENT);
        if (id == Provider::Commands::ProvideOffer::Id)
        {
            Provider::Commands::ProvideOffer::DecodableType offer;
            ReturnErrorOnFailure(offer.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(offer.originatingEndpointID == kRequestorEndpoint && offer.webRTCSessionID.IsNull() &&
                                    offer.streamUsage == Clusters::Globals::StreamUsageEnum::kLiveView &&
                                    offer.videoStreamID.HasValue() && !offer.videoStreamID.Value().IsNull() &&
                                    offer.videoStreamID.Value().Value() == kVideoStream && !offer.audioStreamID.HasValue() &&
                                    !offer.videoStreams.HasValue() && !offer.audioStreams.HasValue(),
                                CHIP_ERROR_INVALID_ARGUMENT);
            Json::Value message;
            message["type"] = "offer";
            message["sdp"]  = std::string(offer.sdp.data(), offer.sdp.size());
            ReturnErrorOnFailure(providerMedia.Send(std::move(message)));
            ++offers;
            protocolActive = true;
            Provider::Commands::ProvideOfferResponse::Type response;
            response.webRTCSessionID = kProtocolSession;
            return Respond(exchange, commandPath, &response);
        }
        if (id == Provider::Commands::ProvideICECandidates::Id)
        {
            Provider::Commands::ProvideICECandidates::DecodableType candidates;
            ReturnErrorOnFailure(candidates.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(candidates.webRTCSessionID == kProtocolSession, CHIP_ERROR_INVALID_ARGUMENT);
            auto iterator = candidates.ICECandidates.begin();
            while (iterator.Next())
            {
                const auto & candidate = iterator.GetValue();
                Json::Value message;
                message["type"]                   = "candidate";
                message["candidate"]["candidate"] = std::string(candidate.candidate.data(), candidate.candidate.size());
                message["candidate"]["sdpMid"]    = !candidate.SDPMid.IsNull()
                       ? Json::Value(std::string(candidate.SDPMid.Value().data(), candidate.SDPMid.Value().size()))
                       : Json::Value();
                message["candidate"]["sdpMLineIndex"] =
                    !candidate.SDPMLineIndex.IsNull() ? Json::Value(candidate.SDPMLineIndex.Value()) : Json::Value();
                ReturnErrorOnFailure(providerMedia.Send(std::move(message)));
                ++sentCandidates;
            }
            ReturnErrorOnFailure(iterator.GetStatus());
            return Status(exchange, commandPath);
        }
        if (id == Provider::Commands::EndSession::Id)
        {
            Provider::Commands::EndSession::DecodableType end;
            ReturnErrorOnFailure(end.Decode(fields, exchange->GetSessionHandle()->GetFabricIndex()));
            VerifyOrReturnError(end.webRTCSessionID == kProtocolSession &&
                                    end.reason == Clusters::Globals::WebRTCEndReasonEnum::kUserHangup,
                                CHIP_ERROR_INVALID_ARGUMENT);
            ++ends;
            if (rejectEnd)
            {
                return Status(exchange, commandPath, StatusIB(Protocols::InteractionModel::Status::Failure));
            }
            protocolActive = false;
            providerMedia.RequestStop();
            return Status(exchange, commandPath);
        }
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    class Incoming : public CommandSender::ExtendableCallback
    {
    public:
        void OnResponse(CommandSender *, const CommandSender::ResponseData & response) override
        {
            error = response.statusIB.ToChipError();
        }
        void OnError(const CommandSender *, const CommandSender::ErrorData & failure) override { error = failure.error; }
        void OnNoResponse(CommandSender *, const CommandSender::NoResponseData &) override { error = CHIP_ERROR_TIMEOUT; }
        void OnDone(CommandSender *) override { done = true; }
        CHIP_ERROR error = CHIP_NO_ERROR;
        bool done        = false;
    };
    template <class Request>
    CHIP_ERROR SendIncoming(const Request & request)
    {
        VerifyOrReturnError(!requestorSender, CHIP_ERROR_BUSY);
        incoming        = Incoming();
        requestorSender = std::make_unique<CommandSender>(&incoming, &exchanges, false, false, true);
        const CommandPathParams path(kRequestorEndpoint, Requestor::Id, Request::GetCommandId(),
                                     CommandPathFlags::kEndpointIdValid);
        ReturnErrorOnFailure(requestorSender->AddRequestData(path, request));
        return requestorSender->SendCommandRequest(cameraSession.Get().Value(),
                                                   MakeOptional<System::Clock::Timeout>(System::Clock::Seconds16(5)));
    }
    CHIP_ERROR MediaSignal(const Json::Value & message)
    {
        if (message["type"] == "provider-ready")
        {
            providerReady = true;
            return CHIP_NO_ERROR;
        }
        if (message["type"] == "answer")
        {
            const auto sdp = corruptAnswer ? std::string("invalid SDP") : message["sdp"].asString();
            Requestor::Commands::Answer::Type answer;
            answer.webRTCSessionID = kProtocolSession;
            answer.sdp             = CharSpan(sdp.data(), sdp.size());
            return SendIncoming(answer);
        }
        if (message["type"] == "candidate")
        {
            if (corruptAnswer)
            {
                return CHIP_NO_ERROR;
            }
            const auto & value   = message["candidate"];
            const auto candidate = value["candidate"].asString();
            const auto mid       = value["sdpMid"].isNull() ? std::string() : value["sdpMid"].asString();
            Clusters::Globals::Structs::ICECandidateStruct::Type entry;
            entry.candidate = CharSpan(candidate.data(), candidate.size());
            if (!value["sdpMid"].isNull())
            {
                entry.SDPMid.SetNonNull(CharSpan(mid.data(), mid.size()));
            }
            if (!value["sdpMLineIndex"].isNull())
            {
                entry.SDPMLineIndex.SetNonNull(static_cast<uint16_t>(value["sdpMLineIndex"].asUInt()));
            }
            Requestor::Commands::ICECandidates::Type candidates;
            candidates.webRTCSessionID = kProtocolSession;
            candidates.ICECandidates   = DataModel::List<const Clusters::Globals::Structs::ICECandidateStruct::Type>(&entry, 1);
            ++receivedCandidates;
            return SendIncoming(candidates);
        }
        return CHIP_ERROR_INCORRECT_STATE;
    }
    static void Wake(System::Layer *, void *) {}
    bool Pump(bool waitForProvider = false)
    {
        const auto deadline = System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(35);
        while (System::SystemClock().GetMonotonicTimestamp() < deadline && wireError == CHIP_NO_ERROR)
        {
            if (command && LiveViewFlowTestAccess::Played(*command))
            {
                if (changeSoftPrivacy)
                {
                    privacy = true;
                }
                if (changeHardPrivacy)
                {
                    hardPrivacy = true;
                }
            }
            if (requestorSender && incoming.done)
            {
                wireError = incoming.error;
                requestorSender.reset();
            }
            if (!requestorSender)
            {
                if (auto signal = providerMedia.TakeEvent())
                {
                    wireError = MediaSignal(*signal);
                }
            }
            if (waitForProvider ? providerReady : LiveViewFlowTestAccess::Complete(*command))
            {
                return wireError == CHIP_NO_ERROR;
            }
            if (system.StartTimer(System::Clock::Milliseconds32(20), Wake, this) != CHIP_NO_ERROR)
            {
                return false;
            }
            system.PrepareEvents();
            system.WaitForEvents();
            system.HandleEvents();
        }
        return false;
    }

    static inline std::filesystem::path testStorage;
    TestPersistentStorageDelegate storage;
    Testing::FabricTestFixture fabrics{ &storage };
    FabricIndex dummyFabric = 1, controllerFabric = 1, cameraFabric = 1;
    System::LayerImpl system;
    Inet::TCPEndPointManagerImpl endpoints;
    Transport::TCP<2, 4> tcp;
    TransportMgrBase transports;
    Crypto::DefaultSessionKeystore keystore;
    SessionManager sessions;
    secure_channel::MessageCounterManager counters;
    Messaging::ExchangeManager exchanges;
    Credentials::GroupDataProviderImpl groups;
    SessionHolder controllerSession, cameraSession;
    DataModel::InteractionModelContext context{ *this, *this };
    std::unique_ptr<LiveViewCommand> command;
    WebView2Receiver providerMedia;
    std::unique_ptr<CommandSender> requestorSender;
    Incoming incoming;
    Av::Structs::VideoStreamStruct::Type stream;
    CHIP_ERROR wireError = CHIP_NO_ERROR;
    size_t reads = 0, allocations = 0, offers = 0, ends = 0, deallocations = 0;
    size_t sentCandidates = 0, receivedCandidates = 0;
    bool providerReady = false, allocated = false, privacy = false;
    bool invalidMetadata = false, corruptAnswer = false, rejectEnd = false, protocolActive = false;
    bool bridge = false, multipleCameras = false, hardPrivacy = false;
    bool changeSoftPrivacy = false, changeHardPrivacy = false;
    EndpointId cameraEndpoint = kCameraEndpoint;
};

TEST_F(LiveViewFlowTests, CaseDiscoveryAllocationActualVideoAndOwnedCleanup)
{
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_GE(reads, 14u);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(offers, 1u);
    EXPECT_GT(sentCandidates, 0u);
    EXPECT_GT(receivedCandidates, 0u);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_FALSE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, PrivacyRejectsBeforeAllocatingOrOffering)
{
    privacy = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_ERROR_ACCESS_DENIED);
    EXPECT_FALSE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(offers, 0u);
    EXPECT_EQ(deallocations, 0u);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, InvalidAllocatedMetadataDeallocatesWithoutOffering)
{
    invalidMetadata = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    EXPECT_FALSE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(offers, 0u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, RejectedMediaAnswerStillEndsAndDeallocates)
{
    corruptAnswer = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_ERROR_NOT_CONNECTED);
    EXPECT_FALSE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(offers, 1u);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_FALSE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, RemoteEndFailureRemainsAnErrorAfterOwnedDeallocation)
{
    rejectEnd = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == StatusIB(Protocols::InteractionModel::Status::Failure).ToChipError());
    EXPECT_TRUE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(offers, 1u);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_TRUE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, BridgeAtEndpointOneCameraAtEndpointTwoPlaysAndCleansUp)
{
    bridge         = true;
    cameraEndpoint = 2;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(offers, 1u);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_FALSE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, MultipleCamerasRequireExplicitEndpointBeforeAllocation)
{
    multipleCameras = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(offers, 0u);
    EXPECT_FALSE(allocated);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, SoftwarePrivacyDuringPlaybackEndsAndDeallocates)
{
    changeSoftPrivacy = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context, 5) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_ERROR_ACCESS_DENIED);
    EXPECT_TRUE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_TRUE(privacy);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_FALSE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}

TEST_F(LiveViewFlowTests, HardwarePrivacyDuringPlaybackEndsAndDeallocates)
{
    changeHardPrivacy = true;
    ASSERT_TRUE(Connect() == CHIP_NO_ERROR);
    ASSERT_TRUE(providerMedia.Start(WebView2Receiver::Mode::TestProvider) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump(true));
    command = std::make_unique<LiveViewCommand>(nullptr);
    ASSERT_TRUE(LiveViewFlowTestAccess::Start(*command, storage, fabrics.GetFabricTable(), system, exchanges,
                                              controllerSession.Get().Value(), context, 5) == CHIP_NO_ERROR);
    ASSERT_TRUE(Pump());
    EXPECT_TRUE(LiveViewFlowTestAccess::Result(*command) == CHIP_ERROR_ACCESS_DENIED);
    EXPECT_TRUE(LiveViewFlowTestAccess::Played(*command));
    EXPECT_TRUE(hardPrivacy);
    EXPECT_EQ(ends, 1u);
    EXPECT_EQ(deallocations, 1u);
    EXPECT_FALSE(allocated);
    EXPECT_FALSE(protocolActive);
    EXPECT_EQ(exchanges.GetNumActiveExchanges(), 0u);
}
} // namespace
} // namespace CameraController
