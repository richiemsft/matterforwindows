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

#include <app/BufferedReadCallback.h>
#include <app/clusters/webrtc-transport-requestor-server/WebRTCTransportRequestorCluster.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/server-cluster/testing/FabricTestFixture.h>
#include <clusters/Descriptor/Structs.h>
#include <clusters/WebRTCTransportProvider/Commands.h>
#include <controller/CHIPControllerBuildConfig.h>
#include <controller/InvokeInteraction.h>
#include <controller/webrtc/CameraProviderClient.h>
#include <controller/webrtc/CameraRequestorEndpoint.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestPersistentStorageDelegate.h>
#include <system/SystemLayerImpl.h>
#include <transport/SecureSessionTable.h>

#include <array>
#include <string>

namespace {

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using RequestorCluster = WebRTCTransportRequestor::WebRTCTransportRequestorCluster;
using Session          = Globals::Structs::WebRTCSessionStruct::Type;

static_assert(CHIP_CONTROLLER_BUILD_WEBRTC_SIGNALING == 1);

class SignalingDelegate : public WebRTCTransportRequestor::Delegate
{
public:
    CHIP_ERROR HandleOffer(const Session &, const OfferArgs &) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR HandleAnswer(const Session &, const std::string &) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR HandleICECandidates(const Session &, const std::vector<Globals::Structs::ICECandidateStruct::Type> &) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    CHIP_ERROR HandleEnd(const Session &, Globals::WebRTCEndReasonEnum) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
};

class WebRTCSignalingTests : public ::testing::Test
{
public:
    static void SetUpTestSuite() { ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }

    SignalingDelegate delegate;
    RequestorCluster cluster{ 7, delegate };
};

TEST_F(WebRTCSignalingTests, RequestorCommandMetadata)
{
    ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> builder;
    ASSERT_TRUE(cluster.AcceptedCommands({ 7, WebRTCTransportRequestor::Id }, builder) == CHIP_NO_ERROR);
    const auto commands = builder.TakeBuffer();
    const std::array<CommandId, 4> expected{
        WebRTCTransportRequestor::Commands::Offer::Id,
        WebRTCTransportRequestor::Commands::Answer::Id,
        WebRTCTransportRequestor::Commands::ICECandidates::Id,
        WebRTCTransportRequestor::Commands::End::Id,
    };
    ASSERT_EQ(commands.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(commands[i].commandId, expected[i]);
    }
}

TEST_F(WebRTCSignalingTests, RequestorAttributeMetadata)
{
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> builder;
    ASSERT_TRUE(cluster.Attributes({ 7, WebRTCTransportRequestor::Id }, builder) == CHIP_NO_ERROR);
    const auto attributes = builder.TakeBuffer();
    bool hasSessions      = false;
    bool hasRevision      = false;
    for (const auto & attribute : attributes)
    {
        hasSessions |= attribute.attributeId == WebRTCTransportRequestor::Attributes::CurrentSessions::Id;
        hasRevision |= attribute.attributeId == Globals::Attributes::ClusterRevision::Id;
    }
    EXPECT_TRUE(hasSessions);
    EXPECT_TRUE(hasRevision);
}

TEST_F(WebRTCSignalingTests, OwnedRequestorCommandMetadataMatchesSharedCluster)
{
    FabricTable fabrics;
    System::LayerImpl system;
    Controller::CameraSignalingCluster owned(7, fabrics, system);
    ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> sharedBuilder;
    ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> ownedBuilder;
    const ConcreteClusterPath path(7, WebRTCTransportRequestor::Id);
    ASSERT_TRUE(cluster.AcceptedCommands(path, sharedBuilder) == CHIP_NO_ERROR);
    ASSERT_TRUE(owned.AcceptedCommands(path, ownedBuilder) == CHIP_NO_ERROR);
    const auto shared = sharedBuilder.TakeBuffer();
    const auto actual = ownedBuilder.TakeBuffer();
    ASSERT_EQ(actual.size(), shared.size());
    for (size_t i = 0; i < shared.size(); ++i)
    {
        EXPECT_TRUE(actual[i] == shared[i]);
    }
}

TEST_F(WebRTCSignalingTests, OwnedRequestorAttributeMetadataMatchesSharedCluster)
{
    FabricTable fabrics;
    System::LayerImpl system;
    Controller::CameraSignalingCluster owned(7, fabrics, system);
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> sharedBuilder;
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> ownedBuilder;
    const ConcreteClusterPath path(7, WebRTCTransportRequestor::Id);
    ASSERT_TRUE(cluster.Attributes(path, sharedBuilder) == CHIP_NO_ERROR);
    ASSERT_TRUE(owned.Attributes(path, ownedBuilder) == CHIP_NO_ERROR);
    const auto shared = sharedBuilder.TakeBuffer();
    const auto actual = ownedBuilder.TakeBuffer();
    ASSERT_EQ(actual.size(), shared.size());
    for (size_t i = 0; i < shared.size(); ++i)
    {
        EXPECT_TRUE(actual[i] == shared[i]);
    }
}

TEST_F(WebRTCSignalingTests, SessionIdentityIncludesPeerAndFabric)
{
    Session session;
    session.id          = 1;
    session.peerNodeID  = 0x1234;
    session.fabricIndex = 1;
    EXPECT_EQ(cluster.UpsertSession(session), RequestorCluster::UpsertResultEnum::kInserted);
    session.peerNodeID = 0x5678;
    EXPECT_EQ(cluster.UpsertSession(session), RequestorCluster::UpsertResultEnum::kInserted);
    session.fabricIndex = 2;
    EXPECT_EQ(cluster.UpsertSession(session), RequestorCluster::UpsertResultEnum::kInserted);
    session.streamUsage = Globals::StreamUsageEnum::kLiveView;
    EXPECT_EQ(cluster.UpsertSession(session), RequestorCluster::UpsertResultEnum::kUpdated);
    ASSERT_EQ(cluster.GetCurrentSessions().size(), 3u);

    cluster.RemoveSession(1, 0x5678, 1);
    auto sessions = cluster.GetCurrentSessions();
    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[0].peerNodeID, 0x1234u);
    EXPECT_EQ(sessions[1].fabricIndex, 2u);
    EXPECT_EQ(sessions[1].streamUsage, Globals::StreamUsageEnum::kLiveView);
    cluster.RemoveSession(1, 0x5678, 1);
    EXPECT_EQ(cluster.GetCurrentSessions().size(), 2u);
}

TEST_F(WebRTCSignalingTests, RepeatedSessionRemoval)
{
    Session session;
    session.peerNodeID  = 0x1234;
    session.fabricIndex = 1;
    for (uint16_t i = 0; i < 100; ++i)
    {
        session.id = i;
        EXPECT_EQ(cluster.UpsertSession(session), RequestorCluster::UpsertResultEnum::kInserted);
        cluster.RemoveSession(i, session.peerNodeID, session.fabricIndex);
        ASSERT_TRUE(cluster.GetCurrentSessions().empty());
    }
}

TEST_F(WebRTCSignalingTests, ProviderOfferEncodingPreservesLargeSdpAndNullStream)
{
    const std::string sdp(8192, 's');
    std::array<uint8_t, 16384> buffer{};
    WebRTCTransportProvider::Commands::ProvideOffer::Type command;
    command.webRTCSessionID.SetNull();
    command.sdp                   = CharSpan(sdp.data(), sdp.size());
    command.streamUsage           = Globals::StreamUsageEnum::kLiveView;
    command.originatingEndpointID = 7;
    command.videoStreamID.Emplace().SetNull();

    TLV::TLVWriter writer;
    writer.Init(buffer.data(), buffer.size());
    ASSERT_TRUE(command.Encode(writer, TLV::AnonymousTag()) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    EXPECT_GT(writer.GetLengthWritten(), sdp.size());

    TLV::TLVReader reader;
    reader.Init(buffer.data(), writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    WebRTCTransportProvider::Commands::ProvideOffer::DecodableType decoded;
    ASSERT_TRUE(decoded.Decode(reader, 1) == CHIP_NO_ERROR);
    EXPECT_TRUE(decoded.webRTCSessionID.IsNull());
    EXPECT_EQ(std::string(decoded.sdp.data(), decoded.sdp.size()), sdp);
    EXPECT_EQ(decoded.originatingEndpointID, 7);
    ASSERT_TRUE(decoded.videoStreamID.HasValue());
    EXPECT_TRUE(decoded.videoStreamID.Value().IsNull());
    EXPECT_FALSE(decoded.audioStreamID.HasValue());
}

TEST_F(WebRTCSignalingTests, ProviderEncodingRejectsInsufficientBuffer)
{
    const std::string sdp(8192, 's');
    std::array<uint8_t, 128> buffer{};
    WebRTCTransportProvider::Commands::ProvideAnswer::Type command;
    command.webRTCSessionID = 1;
    command.sdp             = CharSpan(sdp.data(), sdp.size());
    TLV::TLVWriter writer;
    writer.Init(buffer.data(), buffer.size());
    EXPECT_TRUE(command.Encode(writer, TLV::AnonymousTag()) == CHIP_ERROR_BUFFER_TOO_SMALL);
}

TEST_F(WebRTCSignalingTests, ProductionOfferPreservesEveryMediaSelection)
{
    using Selection              = Controller::CameraSignalingCluster::StreamSelection;
    using Client                 = Controller::CameraProviderClient;
    const Selection selections[] = { Selection::Absent(), Selection::AutoSelect(), Selection::ExplicitId(0) };
    for (const auto video : selections)
    {
        for (const auto audio : selections)
        {
            Controller::CameraSignalingCluster::SessionInfo info;
            info.videoSelection       = video;
            info.audioSelection       = audio;
            info.metadata.streamUsage = Globals::StreamUsageEnum::kLiveView;
            WebRTCTransportProvider::Commands::ProvideOffer::Type request;
            const uint16_t staleStreams[] = { 42 };
            request.videoStreams.SetValue(DataModel::List<const uint16_t>(staleStreams));
            request.audioStreams.SetValue(DataModel::List<const uint16_t>(staleStreams));
            const auto error = Client::BuildOffer(info, 7, "v=0\r\n"_span, {}, request);
            if (video.GetKind() == Selection::Kind::Absent && audio.GetKind() == Selection::Kind::Absent)
            {
                EXPECT_TRUE(error == CHIP_ERROR_INVALID_ARGUMENT);
                continue;
            }
            ASSERT_TRUE(error == CHIP_NO_ERROR);
            std::array<uint8_t, 512> bytes;
            TLV::TLVWriter writer;
            writer.Init(bytes.data(), bytes.size());
            ASSERT_TRUE(request.Encode(writer, TLV::AnonymousTag()) == CHIP_NO_ERROR);
            ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
            TLV::TLVReader reader;
            reader.Init(bytes.data(), writer.GetLengthWritten());
            ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
            WebRTCTransportProvider::Commands::ProvideOffer::DecodableType decoded;
            ASSERT_TRUE(decoded.Decode(reader, 1) == CHIP_NO_ERROR);
            EXPECT_FALSE(decoded.videoStreams.HasValue());
            EXPECT_FALSE(decoded.audioStreams.HasValue());
            EXPECT_EQ(decoded.videoStreamID.HasValue(), video.GetKind() != Selection::Kind::Absent);
            EXPECT_EQ(decoded.audioStreamID.HasValue(), audio.GetKind() != Selection::Kind::Absent);
            for (const auto & field : { std::pair{ video, decoded.videoStreamID }, std::pair{ audio, decoded.audioStreamID } })
            {
                if (field.second.HasValue())
                {
                    EXPECT_EQ(field.second.Value().IsNull(), field.first.GetKind() == Selection::Kind::AutoSelect);
                    if (!field.second.Value().IsNull())
                    {
                        EXPECT_EQ(field.second.Value().Value(), 0);
                    }
                }
            }
        }
    }
}

TEST_F(WebRTCSignalingTests, MediaSelectionRejectsReservedAndUnexpectedResponseIds)
{
    using Selection = Controller::CameraSignalingCluster::StreamSelection;
    Optional<DataModel::Nullable<uint16_t>> field;
    EXPECT_TRUE(Selection::Absent().MatchesResponse(field));
    EXPECT_TRUE(Selection::AutoSelect().MatchesResponse(field));
    EXPECT_TRUE(Selection::ExplicitId(3).MatchesResponse(field));
    field.Emplace().SetNull();
    EXPECT_TRUE(Selection::Absent().MatchesResponse(field));
    EXPECT_TRUE(Selection::AutoSelect().MatchesResponse(field));
    EXPECT_FALSE(Selection::ExplicitId(3).MatchesResponse(field));
    field.Emplace().SetNonNull(3);
    EXPECT_FALSE(Selection::Absent().MatchesResponse(field));
    EXPECT_TRUE(Selection::AutoSelect().MatchesResponse(field));
    EXPECT_TRUE(Selection::ExplicitId(3).MatchesResponse(field));
    EXPECT_FALSE(Selection::ExplicitId(4).MatchesResponse(field));
    field.Emplace().SetNonNull(UINT16_MAX);
    EXPECT_FALSE(Selection::AutoSelect().MatchesResponse(field));
    EXPECT_FALSE(Selection::ExplicitId(UINT16_MAX).IsValid());
    Controller::CameraSignalingCluster::SessionInfo info;
    info.videoSelection = Selection::ExplicitId(UINT16_MAX);
    WebRTCTransportProvider::Commands::ProvideOffer::Type request;
    EXPECT_TRUE(Controller::CameraProviderClient::BuildOffer(info, 7, "v=0\r\n"_span, {}, request) == CHIP_ERROR_INVALID_ARGUMENT);
}

TEST_F(WebRTCSignalingTests, IceServerValidationBoundsAndOptionalPresence)
{
    using Client          = Controller::CameraProviderClient;
    const CharSpan urls[] = { "stun:lan.invalid:3478"_span, "turns:relay.invalid:5349"_span };
    Client::IceServer server;
    server.URLs = DataModel::List<const CharSpan>(urls);
    server.username.SetValue(""_span);
    server.credential.SetValue("test-only-credential"_span);
    server.caid.SetValue(0);
    ASSERT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(&server, 1)) == CHIP_NO_ERROR);
    WebRTCTransportProvider::Commands::ProvideOffer::Type request;
    request.webRTCSessionID.SetNull();
    const std::string sdp(8192, 's');
    request.sdp = CharSpan(sdp.data(), sdp.size());
    request.ICEServers.SetValue(DataModel::List<const Client::IceServer>(&server, 1));
    std::array<uint8_t, 16384> bytes;
    TLV::TLVWriter writer;
    writer.Init(bytes.data(), bytes.size());
    ASSERT_TRUE(request.Encode(writer, TLV::AnonymousTag()) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    TLV::TLVReader reader;
    reader.Init(bytes.data(), writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    WebRTCTransportProvider::Commands::ProvideOffer::DecodableType decoded;
    ASSERT_TRUE(decoded.Decode(reader, 1) == CHIP_NO_ERROR);
    ASSERT_TRUE(decoded.ICEServers.HasValue());
    auto iterator = decoded.ICEServers.Value().begin();
    ASSERT_TRUE(iterator.Next());
    const auto & result = iterator.GetValue();
    ASSERT_TRUE(result.username.HasValue());
    EXPECT_TRUE(result.username.Value().empty());
    ASSERT_TRUE(result.credential.HasValue());
    EXPECT_TRUE(result.credential.Value().data_equal("test-only-credential"_span));
    ASSERT_TRUE(result.caid.HasValue());
    EXPECT_EQ(result.caid.Value(), 0);
    auto urlIterator = result.URLs.begin();
    ASSERT_TRUE(urlIterator.Next());
    EXPECT_TRUE(urlIterator.GetValue().data_equal(urls[0]));
    ASSERT_TRUE(urlIterator.Next());
    EXPECT_TRUE(urlIterator.GetValue().data_equal(urls[1]));
    EXPECT_FALSE(urlIterator.Next());
    EXPECT_TRUE(urlIterator.GetStatus() == CHIP_NO_ERROR);
    EXPECT_FALSE(iterator.Next());
    EXPECT_TRUE(iterator.GetStatus() == CHIP_NO_ERROR);
    const std::string oversized(Client::kMaxIceCredentialBytes + 1, 'x');
    server.credential.SetValue(CharSpan(oversized.data(), oversized.size()));
    EXPECT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(&server, 1)) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    server.credential.ClearValue();
    const CharSpan malformed[] = { ""_span, "https:relay.invalid"_span, "turn:host with space"_span,
                                   CharSpan("stun:bad\0host", 13) };
    for (auto const & url : malformed)
    {
        server.URLs = DataModel::List<const CharSpan>(&url, 1);
        EXPECT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(&server, 1)) ==
                    CHIP_IM_GLOBAL_STATUS(ConstraintError));
    }
    server.URLs = DataModel::List<const CharSpan>();
    EXPECT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(&server, 1)) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    std::array<CharSpan, Client::kMaxIceUrls + 1> tooManyUrls;
    tooManyUrls.fill(urls[0]);
    server.URLs = DataModel::List<const CharSpan>(tooManyUrls);
    EXPECT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(&server, 1)) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    EXPECT_TRUE(Client::ValidateIceServers({}) == CHIP_NO_ERROR);
    std::array<Client::IceServer, Client::kMaxIceServers + 1> tooManyServers;
    EXPECT_TRUE(Client::ValidateIceServers(Span<const Client::IceServer>(tooManyServers)) ==
                CHIP_IM_GLOBAL_STATUS(ConstraintError));
}

class NoEvents : public DataModel::EventsGenerator
{
public:
    void ScheduleUrgentEventDeliverySync(std::optional<FabricIndex>) override {}
    CHIP_ERROR GenerateEvent(EventLoggingDelegate *, const EventOptions &, EventNumber &) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
    System::Clock::Milliseconds64 GetMonotonicStartupTime() const override { return System::Clock::Milliseconds64(0); }
};

template <class Value>
class DescriptorListProbe : public ReadClient::Callback
{
public:
    void OnAttributeData(const ConcreteDataAttributePath &, TLV::TLVReader * data, const StatusIB & status) override
    {
        error = status.ToChipError();
        ASSERT_TRUE(data != nullptr);
        DataModel::DecodableList<Value> list;
        ASSERT_TRUE(DataModel::Decode(*data, list) == CHIP_NO_ERROR);
        auto iterator = list.begin();
        while (iterator.Next())
        {
            values.push_back(iterator.GetValue());
        }
        error = iterator.GetStatus();
        ++reports;
    }
    void OnError(CHIP_ERROR failure) override { error = failure; }
    void OnDone(ReadClient *) override {}
    std::vector<Value> values;
    CHIP_ERROR error = CHIP_NO_ERROR;
    size_t reports   = 0;
};

template <class Value>
void FeedDescriptorChunk(ReadClient::Callback & callback, const ConcreteDataAttributePath & path, const Value & value)
{
    std::array<uint8_t, 512> bytes;
    TLV::TLVWriter writer;
    writer.Init(bytes.data(), bytes.size());
    ASSERT_TRUE(DataModel::Encode(writer, TLV::AnonymousTag(), value) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    TLV::TLVReader reader;
    reader.Init(bytes.data(), writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    callback.OnAttributeData(path, &reader, StatusIB(Protocols::InteractionModel::Status::Success));
}

TEST_F(WebRTCSignalingTests, ChunkedDescriptorPartsListPreservesBridgeAndCameraEndpoints)
{
    DescriptorListProbe<uint16_t> probe;
    BufferedReadCallback buffered(probe, true);
    ReadClient::Callback & callback = buffered;
    ConcreteDataAttributePath path(0, Descriptor::Id, Descriptor::Attributes::PartsList::Id);
    path.mListOp = ConcreteDataAttributePath::ListOperation::ReplaceAll;
    callback.OnReportBegin();
    const uint16_t bridge[] = { 1 };
    FeedDescriptorChunk(callback, path, DataModel::List<const uint16_t>(bridge));
    path.mListOp = ConcreteDataAttributePath::ListOperation::AppendItem;
    FeedDescriptorChunk(callback, path, uint16_t(2));
    EXPECT_EQ(probe.reports, 0u);
    callback.OnReportEnd();
    EXPECT_TRUE(probe.error == CHIP_NO_ERROR);
    ASSERT_EQ(probe.values.size(), 2u);
    EXPECT_EQ(probe.values[0], 1);
    EXPECT_EQ(probe.values[1], 2);
    EXPECT_EQ(probe.reports, 1u);
}

TEST_F(WebRTCSignalingTests, ChunkedDescriptorDeviceTypeListPreservesStructuredFields)
{
    using DeviceType = Descriptor::Structs::DeviceTypeStruct::Type;
    DescriptorListProbe<DeviceType> probe;
    BufferedReadCallback buffered(probe, true);
    ReadClient::Callback & callback = buffered;
    ConcreteDataAttributePath path(2, Descriptor::Id, Descriptor::Attributes::DeviceTypeList::Id);
    path.mListOp = ConcreteDataAttributePath::ListOperation::ReplaceAll;
    callback.OnReportBegin();
    const DeviceType first[] = { { 0x1234, 1 } };
    FeedDescriptorChunk(callback, path, DataModel::List<const DeviceType>(first));
    path.mListOp = ConcreteDataAttributePath::ListOperation::AppendItem;
    FeedDescriptorChunk(callback, path, DeviceType{ 0x5678, 2 });
    callback.OnReportEnd();
    EXPECT_TRUE(probe.error == CHIP_NO_ERROR);
    ASSERT_EQ(probe.values.size(), 2u);
    EXPECT_EQ(probe.values[0].deviceType, 0x1234u);
    EXPECT_EQ(probe.values[0].revision, 1);
    EXPECT_EQ(probe.values[1].deviceType, 0x5678u);
    EXPECT_EQ(probe.values[1].revision, 2);
    EXPECT_EQ(probe.reports, 1u);
}

class NoExchange : public DataModel::ActionContext
{
public:
    Messaging::ExchangeContext * CurrentExchange() override { return nullptr; }
};

class NoDeviceTypes : public Access::AccessControl::DeviceTypeResolver
{
public:
    bool IsDeviceTypeOnEndpoint(DeviceTypeId, EndpointId) override { return false; }
};

class DenyAccess : public Access::AccessControl::Delegate
{
};

class CameraRuntimeTests : public ::testing::Test
{
public:
    using CameraCluster = Controller::CameraSignalingCluster;
    static void SetUpTestSuite() { ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }

    void SetUp() override
    {
        ASSERT_TRUE(fabrics.SetUpTestFabric(fabricIndex) == CHIP_NO_ERROR);
        ASSERT_TRUE(system.Init() == CHIP_NO_ERROR);
        ASSERT_TRUE(attributeStorage.Init(&storage) == CHIP_NO_ERROR);
        ASSERT_TRUE(access.Init(&deny, resolver) == CHIP_NO_ERROR);
        ASSERT_TRUE(provider.Startup(context) == CHIP_NO_ERROR);
        requestor.emplace(7, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(), system);
        ASSERT_TRUE(requestor->Register(provider, access) == CHIP_NO_ERROR);
        subject.fabricIndex = fabricIndex;
        subject.authMode    = Access::AuthMode::kCase;
        subject.subject     = 0x1234;
    }
    void TearDown() override
    {
        if (requestor.has_value())
        {
            EXPECT_TRUE(requestor->Unregister() == CHIP_NO_ERROR);
            requestor.reset();
        }
        EXPECT_TRUE(provider.Shutdown() == CHIP_NO_ERROR);
        access.Finish();
        fabrics.GetFabricTable().Shutdown();
        system.Shutdown();
    }
    uint64_t Create(EndpointId cameraEndpoint = 2, NodeId node = 0x1234)
    {
        uint64_t handle = 0;
        EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(node, fabricIndex), cameraEndpoint,
                                                         DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(),
                                                         handle) == CHIP_NO_ERROR);
        return handle;
    }
    template <class Command>
    DataModel::ActionReturnStatus Invoke(const Command & command, Access::SubjectDescriptor caller)
    {
        std::array<uint8_t, 65536> bytes;
        TLV::TLVWriter writer;
        writer.Init(bytes.data(), bytes.size());
        EXPECT_TRUE(command.Encode(writer, TLV::AnonymousTag()) == CHIP_NO_ERROR);
        EXPECT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
        TLV::TLVReader reader;
        reader.Init(bytes.data(), writer.GetLengthWritten());
        EXPECT_TRUE(reader.Next() == CHIP_NO_ERROR);
        DataModel::InvokeRequest request({ 7, WebRTCTransportRequestor::Id, Command::GetCommandId() }, caller);
        auto result = provider.InvokeCommand(request, reader, nullptr);
        EXPECT_TRUE(result.has_value());
        return result.value_or(DataModel::ActionReturnStatus(CHIP_ERROR_INTERNAL));
    }
    CHIP_ERROR ApplyResponse(uint64_t handle, const WebRTCTransportProvider::Commands::ProvideOfferResponse::Type & response)
    {
        std::array<uint8_t, 512> bytes;
        TLV::TLVWriter writer;
        writer.Init(bytes.data(), bytes.size());
        DataModel::FabricAwareTLVWriter fabricWriter(writer, fabricIndex);
        EXPECT_TRUE(response.Encode(fabricWriter, TLV::AnonymousTag()) == CHIP_NO_ERROR);
        EXPECT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
        TLV::TLVReader reader;
        reader.Init(bytes.data(), writer.GetLengthWritten());
        EXPECT_TRUE(reader.Next() == CHIP_NO_ERROR);
        return Controller::CameraProviderClient::ApplyOfferResponse(requestor->Signaling(), handle, reader);
    }

    TestPersistentStorageDelegate storage;
    DefaultAttributePersistenceProvider attributeStorage;
    Testing::FabricTestFixture fabrics{ &storage };
    FabricIndex fabricIndex = 1;
    System::LayerImpl system;
    NoEvents events;
    NoExchange exchange;
    DataModel::InteractionModelContext context{ events, exchange };
    CodeDrivenDataModelProvider provider{ storage, attributeStorage };
    NoDeviceTypes resolver;
    DenyAccess deny;
    Access::AccessControl access;
    // Test-only device type. Production callers supply their selected revision's metadata.
    const DataModel::DeviceTypeEntry types[1]{ { 0x1234, 1 } };
    std::optional<Controller::CameraRequestorEndpoint> requestor;
    Access::SubjectDescriptor subject;
};

TEST_F(CameraRuntimeTests, EndpointMetadataIsRoutableAndCollisionSafe)
{
    ReadOnlyBufferBuilder<DataModel::EndpointEntry> endpoints;
    ASSERT_TRUE(provider.Endpoints(endpoints) == CHIP_NO_ERROR);
    auto endpointList = endpoints.TakeBuffer();
    ASSERT_EQ(endpointList.size(), 1u);
    EXPECT_EQ(endpointList[0].id, 7);
    ReadOnlyBufferBuilder<DataModel::ServerClusterEntry> clusters;
    ASSERT_TRUE(provider.ServerClusters(7, clusters) == CHIP_NO_ERROR);
    EXPECT_EQ(clusters.TakeBuffer().size(), 2u);
    ReadOnlyBufferBuilder<ClusterId> clients;
    ASSERT_TRUE(provider.ClientClusters(7, clients) == CHIP_NO_ERROR);
    const auto clientList = clients.TakeBuffer();
    ASSERT_EQ(clientList.size(), 1u);
    EXPECT_EQ(clientList[0], WebRTCTransportProvider::Id);
    ReadOnlyBufferBuilder<DataModel::AcceptedCommandEntry> commands;
    ASSERT_TRUE(provider.AcceptedCommands({ 7, WebRTCTransportRequestor::Id }, commands) == CHIP_NO_ERROR);
    EXPECT_EQ(commands.TakeBuffer().size(), 4u);
    EndpointId available = kInvalidEndpointId;
    ASSERT_TRUE(Controller::CameraRequestorEndpoint::FindAvailableEndpoint(provider, available) == CHIP_NO_ERROR);
    EXPECT_EQ(available, 1);
    Controller::CameraRequestorEndpoint collision(7, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(),
                                                  system);
    EXPECT_TRUE(collision.Register(provider, access) == CHIP_ERROR_DUPLICATE_KEY_ID);
}

TEST_F(CameraRuntimeTests, HostedControllerUsesSharedBindingAndPeerScopedMetadataAccess)
{
    CASESessionManager sessions;
    Binding::Manager bindings;
    Binding::ManagerInitParams params;
    params.mFabricTable               = &fabrics.GetFabricTable();
    params.mCASESessionManager        = &sessions;
    params.mStorage                   = &storage;
    params.mEstablishConnectionOnInit = false;
    ASSERT_TRUE(bindings.Init(params) == CHIP_NO_ERROR);
    auto cleanup = ScopeExit([&] { bindings.Shutdown(); });
    Controller::CameraRequestorEndpoint hosted(9, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(), system,
                                               bindings, DeviceLayer::PlatformMgr());
    ASSERT_TRUE(hosted.Register(provider, access) == CHIP_NO_ERROR);
    auto unregister = ScopeExit([&] { EXPECT_TRUE(hosted.Unregister() == CHIP_NO_ERROR); });
    ReadOnlyBufferBuilder<DataModel::ServerClusterEntry> builder;
    ASSERT_TRUE(provider.ServerClusters(9, builder) == CHIP_NO_ERROR);
    const auto clusters = builder.TakeBuffer();
    ASSERT_EQ(clusters.size(), 3u);
    bool bindingFound = false;
    for (const auto & entry : clusters)
    {
        bindingFound |= entry.clusterId == Binding::Id;
    }
    EXPECT_TRUE(bindingFound);
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> attributes;
    ASSERT_TRUE(provider.Attributes({ 9, Binding::Id }, attributes) == CHIP_NO_ERROR);
    EXPECT_FALSE(attributes.TakeBuffer().empty());
    uint64_t handle = 0;
    ASSERT_TRUE(hosted.Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2, DataModel::Nullable<uint16_t>(1),
                                                 DataModel::Nullable<uint16_t>(), handle) == CHIP_NO_ERROR);
    Access::RequestPath path;
    path.endpoint    = 9;
    path.requestType = Access::RequestType::kAttributeReadRequest;
    for (const auto clusterId : { Descriptor::Id, Binding::Id })
    {
        path.cluster = clusterId;
        EXPECT_TRUE(hosted.CheckAccess(subject, path, Access::Privilege::kView) == CHIP_NO_ERROR);
        EXPECT_TRUE(hosted.CheckAccess(subject, path, Access::Privilege::kAdminister) == CHIP_ERROR_ACCESS_DENIED);
        auto other = subject;
        ++other.subject;
        EXPECT_TRUE(hosted.CheckAccess(other, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
        path.endpoint = 7;
        EXPECT_TRUE(hosted.CheckAccess(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
        path.endpoint = 9;
    }
    path.cluster     = Binding::Id;
    path.requestType = Access::RequestType::kAttributeWriteRequest;
    EXPECT_TRUE(hosted.CheckAccess(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    ASSERT_TRUE(hosted.Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    path.requestType = Access::RequestType::kAttributeReadRequest;
    EXPECT_TRUE(hosted.CheckAccess(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
}

TEST_F(CameraRuntimeTests, BindingRegistrationCollisionRollsBackOnlyOwnedClusters)
{
    CASESessionManager sessions;
    Binding::Manager bindings;
    Binding::ManagerInitParams params;
    params.mFabricTable               = &fabrics.GetFabricTable();
    params.mCASESessionManager        = &sessions;
    params.mStorage                   = &storage;
    params.mEstablishConnectionOnInit = false;
    ASSERT_TRUE(bindings.Init(params) == CHIP_NO_ERROR);
    auto cleanup = ScopeExit([&] { bindings.Shutdown(); });
    RegisteredServerCluster<BindingCluster> existing(
        BindingCluster::Context{ bindings.GetBindingTable(), bindings, DeviceLayer::PlatformMgr() }, 9);
    ASSERT_TRUE(provider.AddCluster(existing.Registration()) == CHIP_NO_ERROR);
    auto remove = ScopeExit([&] { EXPECT_TRUE(provider.RemoveCluster(&existing.Cluster()) == CHIP_NO_ERROR); });
    Controller::CameraRequestorEndpoint hosted(9, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(), system,
                                               bindings, DeviceLayer::PlatformMgr());
    EXPECT_TRUE(hosted.Register(provider, access) == CHIP_ERROR_DUPLICATE_KEY_ID);
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> attributes;
    EXPECT_TRUE(provider.Attributes({ 9, Binding::Id }, attributes) == CHIP_NO_ERROR);
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> descriptor;
    EXPECT_TRUE(provider.Attributes({ 9, Descriptor::Id }, descriptor) == CHIP_ERROR_KEY_NOT_FOUND);
    ReadOnlyBufferBuilder<DataModel::AttributeEntry> signaling;
    EXPECT_TRUE(provider.Attributes({ 9, WebRTCTransportRequestor::Id }, signaling) == CHIP_ERROR_KEY_NOT_FOUND);
}

TEST_F(CameraRuntimeTests, EarlyAnswerIsOwnedAndCorrelated)
{
    const uint64_t handle = Create();
    CameraCluster::SessionState state;
    ASSERT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, CameraCluster::SessionState::Negotiating);
    std::string sdp = "v=0\r\nowned answer";
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = CharSpan(sdp.data(), sdp.size());
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    sdp.assign(sdp.size(), 'x');
    EXPECT_FALSE(requestor->Signaling().IsNegotiated(handle));
    ASSERT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, CameraCluster::SessionState::Negotiating);
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().IsNegotiated(handle));
    ASSERT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, CameraCluster::SessionState::Established);
    const auto signal = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(signal.has_value());
    EXPECT_EQ(signal->sdp, "v=0\r\nowned answer");
    EXPECT_EQ(signal->handle, handle);
    EXPECT_EQ(signal->cameraEndpoint, 2);
    EXPECT_EQ(signal->peer.GetNodeId(), subject.subject);
    EXPECT_EQ(signal->peer.GetFabricIndex(), subject.fabricIndex);
    EXPECT_TRUE(Invoke(answer, subject) == Protocols::InteractionModel::Status::InvalidInState);
}

TEST_F(CameraRuntimeTests, SessionStateDoesNotConsumeSignalsOrAuthorizeFailedSession)
{
    const auto handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().FailSession(handle, CHIP_ERROR_TIMEOUT) == CHIP_NO_ERROR);
    CameraCluster::SessionState state;
    for (size_t i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_NO_ERROR);
        EXPECT_EQ(state, CameraCluster::SessionState::Failed);
    }
    CameraCluster::SessionInfo info;
    EXPECT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_ERROR_NOT_FOUND);
    EXPECT_TRUE(requestor->Signaling().TakeSignal().has_value());
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_ERROR_NOT_FOUND);
}

TEST_F(CameraRuntimeTests, RemoteEndReportsClosedBeforeLocalRelease)
{
    const auto handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    WebRTCTransportRequestor::Commands::End::Type end;
    end.webRTCSessionID = 42;
    end.reason          = Globals::WebRTCEndReasonEnum::kUserHangup;
    ASSERT_TRUE(Invoke(end, subject).IsSuccess());
    CameraCluster::SessionState state;
    ASSERT_TRUE(requestor->Signaling().GetSessionState(handle, state) == CHIP_NO_ERROR);
    EXPECT_EQ(state, CameraCluster::SessionState::Closed);
    const auto signal = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(signal.has_value());
    EXPECT_EQ(signal->type, CameraCluster::SignalType::End);
}

TEST_F(CameraRuntimeTests, UnexpectedPeerFabricAndAuthAreRejected)
{
    const uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    auto caller            = subject;
    caller.subject         = 0x5678;
    EXPECT_TRUE(Invoke(answer, caller) == Protocols::InteractionModel::Status::UnsupportedAccess);
    caller             = subject;
    caller.fabricIndex = 2;
    EXPECT_TRUE(Invoke(answer, caller) == Protocols::InteractionModel::Status::UnsupportedAccess);
    caller          = subject;
    caller.authMode = Access::AuthMode::kPase;
    EXPECT_TRUE(Invoke(answer, caller) == Protocols::InteractionModel::Status::UnsupportedAccess);
    caller                 = subject;
    caller.isCommissioning = true;
    EXPECT_TRUE(Invoke(answer, caller) == Protocols::InteractionModel::Status::UnsupportedAccess);
    EXPECT_FALSE(requestor->Signaling().TakeSignal().has_value());
}

TEST_F(CameraRuntimeTests, ClosedSessionsCannotReceiveOrReuseOldIdentity)
{
    uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    handle = Create();
    EXPECT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_ERROR_DUPLICATE_KEY_ID);
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    EXPECT_TRUE(Invoke(answer, subject) == Protocols::InteractionModel::Status::UnsupportedAccess);
    EXPECT_FALSE(requestor->Signaling().TakeSignal().has_value());
}

TEST_F(CameraRuntimeTests, PendingCancellationCannotAuthorizeLateAnswer)
{
    uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2,
                                                     DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(),
                                                     handle) == CHIP_ERROR_DUPLICATE_KEY_ID);
}

TEST_F(CameraRuntimeTests, TimeoutAndShutdownAreObservable)
{
    const uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    requestor->Signaling().ExpireSessions(System::SystemClock().GetMonotonicTimestamp() + System::Clock::Seconds32(31));
    auto failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->handle, handle);
    ASSERT_TRUE(failure->protocolId.has_value());
    EXPECT_EQ(*failure->protocolId, 42);
    EXPECT_TRUE(failure->error == CHIP_ERROR_TIMEOUT);
    EXPECT_FALSE(requestor->Signaling().IsNegotiated(handle));
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    const uint64_t active = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(active, 43) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Unregister() == CHIP_NO_ERROR);
    failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->handle, active);
    EXPECT_TRUE(failure->error == CHIP_ERROR_INCORRECT_STATE);
}

TEST_F(CameraRuntimeTests, FailurePreservesAbsentAndZeroProtocolIdentity)
{
    uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().FailSession(handle, CHIP_ERROR_TIMEOUT) == CHIP_NO_ERROR);
    auto failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    EXPECT_FALSE(failure->protocolId.has_value());
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    // An unknown failed identity retires the peer on this endpoint.
    ASSERT_TRUE(requestor->Unregister() == CHIP_NO_ERROR);
    requestor.reset();
    requestor.emplace(7, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(), system);
    ASSERT_TRUE(requestor->Register(provider, access) == CHIP_NO_ERROR);
    handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 0) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().FailSession(handle, CHIP_ERROR_TIMEOUT) == CHIP_NO_ERROR);
    failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    ASSERT_TRUE(failure->protocolId.has_value());
    EXPECT_EQ(*failure->protocolId, 0);
}

TEST_F(CameraRuntimeTests, CandidateQueueOverflowFailsRatherThanDropping)
{
    const uint64_t handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 42) == CHIP_NO_ERROR);
    Globals::Structs::ICECandidateStruct::Type candidate;
    candidate.candidate = "candidate:owned"_span;
    candidate.SDPMid.SetNull();
    candidate.SDPMLineIndex.SetNull();
    WebRTCTransportRequestor::Commands::ICECandidates::Type command;
    command.webRTCSessionID = 42;
    command.ICECandidates   = DataModel::List<const Globals::Structs::ICECandidateStruct::Type>(&candidate, 1);
    for (size_t i = 0; i < CameraCluster::kMaxSignals; ++i)
    {
        ASSERT_TRUE(Invoke(command, subject).IsSuccess());
    }
    EXPECT_TRUE(Invoke(command, subject) == CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    const auto failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->type, CameraCluster::SignalType::Failure);
    EXPECT_TRUE(failure->error == CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    EXPECT_FALSE(requestor->Signaling().TakeSignal().has_value());
}

TEST_F(CameraRuntimeTests, MalformedAndOversizedSdpNeverReachConsumer)
{
    Create();
    std::string sdp(CameraCluster::kMaxSdpBytes + 1, 's');
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = CharSpan(sdp.data(), sdp.size());
    EXPECT_TRUE(Invoke(answer, subject) == Protocols::InteractionModel::Status::ConstraintError);
    sdp        = std::string("bad\0sdp", 7);
    answer.sdp = CharSpan(sdp.data(), sdp.size());
    EXPECT_TRUE(Invoke(answer, subject) == Protocols::InteractionModel::Status::ConstraintError);
    EXPECT_FALSE(requestor->Signaling().TakeSignal().has_value());
}

TEST_F(CameraRuntimeTests, RepeatStartStopPreservesFabricAndEndpoint)
{
    for (uint16_t i = 1; i <= 100; ++i)
    {
        const auto handle = Create();
        ASSERT_TRUE(requestor->Signaling().BindSession(handle, i) == CHIP_NO_ERROR);
        ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    }
    EXPECT_NE(fabrics.GetFabricTable().FindFabricWithIndex(fabricIndex), nullptr);
    ReadOnlyBufferBuilder<DataModel::EndpointEntry> endpoints;
    ASSERT_TRUE(provider.Endpoints(endpoints) == CHIP_NO_ERROR);
    EXPECT_EQ(endpoints.TakeBuffer().size(), 1u);
}

TEST_F(CameraRuntimeTests, AccessPolicyIsRestrictedToOwnedPeerPathAndPrivileges)
{
    Access::RequestPath path;
    path.endpoint    = 7;
    path.cluster     = WebRTCTransportRequestor::Id;
    path.requestType = Access::RequestType::kCommandInvokeRequest;
    path.entityId    = WebRTCTransportRequestor::Commands::Answer::Id;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    const auto handle = Create();
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_NO_ERROR);
    path.requestType = Access::RequestType::kAttributeWriteRequest;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    path.requestType = Access::RequestType::kCommandInvokeRequest;
    path.entityId    = WebRTCTransportRequestor::Commands::Offer::Id;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    path.entityId = WebRTCTransportRequestor::Commands::Answer::Id;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kAdminister) == CHIP_ERROR_ACCESS_DENIED);
    auto caller    = subject;
    caller.subject = 0x5678;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(caller, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    caller             = subject;
    caller.fabricIndex = 2;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(caller, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    caller          = subject;
    caller.authMode = Access::AuthMode::kPase;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(caller, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    path.endpoint = 8;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    path.endpoint = 7;
    path.cluster  = Descriptor::Id;
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    path.cluster = WebRTCTransportRequestor::Id;
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().CheckAccess(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
}

TEST_F(CameraRuntimeTests, ClosingSessionImmediatelyReclaimsQueueCapacity)
{
    const auto first = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(first, 42) == CHIP_NO_ERROR);
    const auto second = Create(3, 0x5678);
    ASSERT_TRUE(requestor->Signaling().BindSession(second, 43) == CHIP_NO_ERROR);
    Globals::Structs::ICECandidateStruct::Type candidate;
    candidate.candidate = "candidate:owned"_span;
    WebRTCTransportRequestor::Commands::ICECandidates::Type command;
    command.ICECandidates = DataModel::List<const Globals::Structs::ICECandidateStruct::Type>(&candidate, 1);
    for (size_t i = 0; i < CameraCluster::kMaxSignals; ++i)
    {
        auto caller             = subject;
        caller.subject          = i % 2 == 0 ? 0x1234 : 0x5678;
        command.webRTCSessionID = i % 2 == 0 ? 42 : 43;
        ASSERT_TRUE(Invoke(command, caller).IsSuccess());
    }
    ASSERT_TRUE(requestor->Signaling().CloseSession(first) == CHIP_NO_ERROR);
    auto caller             = subject;
    caller.subject          = 0x5678;
    command.webRTCSessionID = 43;
    for (size_t i = 0; i < CameraCluster::kMaxSignals / 2; ++i)
    {
        ASSERT_TRUE(Invoke(command, caller).IsSuccess());
    }
    for (size_t i = 0; i < CameraCluster::kMaxSignals; ++i)
    {
        const auto signal = requestor->Signaling().TakeSignal();
        ASSERT_TRUE(signal.has_value());
        EXPECT_EQ(signal->handle, second);
        EXPECT_EQ(signal->type, CameraCluster::SignalType::Candidates);
    }
    EXPECT_FALSE(requestor->Signaling().TakeSignal().has_value());
}

TEST_F(CameraRuntimeTests, OwnedCandidatesAndFabricRemovalAreObservable)
{
    const auto handle = Create();
    std::string text  = "candidate:owned";
    std::string mid   = "video";
    Globals::Structs::ICECandidateStruct::Type candidate;
    candidate.candidate = CharSpan(text.data(), text.size());
    candidate.SDPMid.SetNonNull(CharSpan(mid.data(), mid.size()));
    candidate.SDPMLineIndex.SetNonNull(0);
    WebRTCTransportRequestor::Commands::ICECandidates::Type command;
    command.webRTCSessionID = 42;
    command.ICECandidates   = DataModel::List<const Globals::Structs::ICECandidateStruct::Type>(&candidate, 1);
    ASSERT_TRUE(Invoke(command, subject).IsSuccess());
    text.assign(text.size(), 'x');
    mid.assign(mid.size(), 'x');
    auto signal = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(signal.has_value());
    ASSERT_EQ(signal->candidates.size(), 1u);
    EXPECT_EQ(signal->candidates[0].candidate, "candidate:owned");
    EXPECT_EQ(signal->candidates[0].mid, std::optional<std::string>("video"));
    EXPECT_EQ(signal->candidates[0].lineIndex, std::optional<uint16_t>(0));
    ASSERT_TRUE(fabrics.GetFabricTable().Delete(fabricIndex) == CHIP_NO_ERROR);
    signal = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(signal.has_value());
    EXPECT_EQ(signal->handle, handle);
    EXPECT_TRUE(signal->error == CHIP_ERROR_INVALID_FABRIC_INDEX);
}

TEST_F(CameraRuntimeTests, TypedOfferResponsePreservesEarlyAnswerAndChecksStreamIdentity)
{
    const auto handle = Create();
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    WebRTCTransportProvider::Commands::ProvideOfferResponse::Type response;
    response.webRTCSessionID = 42;
    response.videoStreamID.SetValue(DataModel::Nullable<uint16_t>(2));
    EXPECT_TRUE(ApplyResponse(handle, response) == CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(requestor->Signaling().IsNegotiated(handle));
    response.videoStreamID.SetValue(DataModel::Nullable<uint16_t>(1));
    ASSERT_TRUE(ApplyResponse(handle, response) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().IsNegotiated(handle));
    EXPECT_TRUE(ApplyResponse(handle, response) == CHIP_ERROR_INCORRECT_STATE);
}

TEST_F(CameraRuntimeTests, VideoOnlyOmissionRejectsUnexpectedAudioAndPreservesProtocolZero)
{
    using Selection = CameraCluster::StreamSelection;
    uint64_t handle = 0;
    ASSERT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2, Selection::ExplicitId(0),
                                                     Selection::Absent(), handle) == CHIP_NO_ERROR);
    EXPECT_NE(handle, 0u);
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 0;
    answer.sdp             = "v=0\r\n"_span;
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    WebRTCTransportProvider::Commands::ProvideOfferResponse::Type response;
    response.webRTCSessionID = 0;
    response.videoStreamID.SetValue(DataModel::Nullable<uint16_t>(0));
    response.audioStreamID.SetValue(DataModel::Nullable<uint16_t>(2));
    EXPECT_TRUE(ApplyResponse(handle, response) == CHIP_ERROR_INVALID_ARGUMENT);
    CameraCluster::SessionInfo info;
    ASSERT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_NO_ERROR);
    EXPECT_FALSE(info.responseReceived);
    EXPECT_TRUE(info.metadata.audioStreamID.IsNull());
    response.audioStreamID.ClearValue();
    ASSERT_TRUE(ApplyResponse(handle, response) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_NO_ERROR);
    EXPECT_EQ(info.protocolId, 0);
    EXPECT_EQ(info.metadata.videoStreamID.Value(), 0);
    EXPECT_EQ(info.videoSelection.GetKind(), Selection::Kind::ExplicitId);
    EXPECT_EQ(info.audioSelection.GetKind(), Selection::Kind::Absent);
    EXPECT_TRUE(requestor->Signaling().IsNegotiated(handle));
}

TEST_F(CameraRuntimeTests, AutomaticAudioSelectionRetainsRequestIntentAndReturnedId)
{
    using Selection = CameraCluster::StreamSelection;
    uint64_t handle = 0;
    ASSERT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2, Selection::Absent(),
                                                     Selection::AutoSelect(), handle) == CHIP_NO_ERROR);
    WebRTCTransportProvider::Commands::ProvideOfferResponse::Type response;
    response.webRTCSessionID = 0;
    response.audioStreamID.SetValue(DataModel::Nullable<uint16_t>(3));
    ASSERT_TRUE(ApplyResponse(handle, response) == CHIP_NO_ERROR);
    CameraCluster::SessionInfo info;
    ASSERT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_NO_ERROR);
    EXPECT_TRUE(info.metadata.videoStreamID.IsNull());
    EXPECT_EQ(info.metadata.audioStreamID.Value(), 3);
    EXPECT_EQ(info.videoSelection.GetKind(), Selection::Kind::Absent);
    EXPECT_EQ(info.audioSelection.GetKind(), Selection::Kind::AutoSelect);
}

TEST_F(CameraRuntimeTests, LegacyNullableAudioStillRequestsAutomaticSelection)
{
    const auto handle = Create();
    CameraCluster::SessionInfo info;
    ASSERT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_NO_ERROR);
    EXPECT_EQ(info.videoSelection.GetKind(), CameraCluster::StreamSelection::Kind::ExplicitId);
    EXPECT_EQ(info.audioSelection.GetKind(), CameraCluster::StreamSelection::Kind::AutoSelect);
    WebRTCTransportProvider::Commands::ProvideOffer::Type request;
    ASSERT_TRUE(Controller::CameraProviderClient::BuildOffer(info, 7, "v=0\r\n"_span, {}, request) == CHIP_NO_ERROR);
    ASSERT_TRUE(request.audioStreamID.HasValue());
    EXPECT_TRUE(request.audioStreamID.Value().IsNull());
}

TEST_F(CameraRuntimeTests, ContradictoryEarlyIdentityCannotLeakIntoLaterOperation)
{
    const auto handle = Create();
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    WebRTCTransportProvider::Commands::ProvideOfferResponse::Type response;
    response.webRTCSessionID = 43;
    EXPECT_TRUE(ApplyResponse(handle, response) == CHIP_ERROR_INVALID_ARGUMENT);
    auto failure = requestor->Signaling().TakeSignal();
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure->type, CameraCluster::SignalType::Failure);
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    uint64_t next = 0;
    EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2,
                                                     DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(),
                                                     next) == CHIP_ERROR_DUPLICATE_KEY_ID);
}

TEST_F(CameraRuntimeTests, BoundProtocolZeroDoesNotPreventAnotherDistinctSession)
{
    const auto first = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(first, 0) == CHIP_NO_ERROR);
    ASSERT_TRUE(requestor->Signaling().CloseSession(first) == CHIP_NO_ERROR);
    const auto second = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(second, 1) == CHIP_NO_ERROR);
}

TEST_F(CameraRuntimeTests, FailedIdentityIsAvailableOnlyForEndWithoutRestoringAuthorization)
{
    const auto handle = Create();
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    ASSERT_TRUE(requestor->Signaling().FailSession(handle, CHIP_ERROR_CANCELLED) == CHIP_NO_ERROR);
    CameraCluster::SessionInfo info;
    EXPECT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_ERROR_NOT_FOUND);
    ASSERT_TRUE(requestor->Signaling().GetSessionInfoForEnd(handle, info) == CHIP_NO_ERROR);
    EXPECT_EQ(info.protocolId, 42);
    EXPECT_FALSE(info.responseReceived);
    EXPECT_EQ(info.metadata.peerNodeID, subject.subject);
    EXPECT_EQ(info.metadata.fabricIndex, subject.fabricIndex);
    EXPECT_TRUE(Invoke(answer, subject) == Protocols::InteractionModel::Status::UnsupportedAccess);
    ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().GetSessionInfoForEnd(handle, info) == CHIP_ERROR_NOT_FOUND);
}

TEST_F(CameraRuntimeTests, ContradictoryIdentityAndUnknownProtocolCannotBeUsedForRemoteCleanup)
{
    const auto handle = Create();
    WebRTCTransportRequestor::Commands::Answer::Type answer;
    answer.webRTCSessionID = 42;
    answer.sdp             = "v=0\r\n"_span;
    ASSERT_TRUE(Invoke(answer, subject).IsSuccess());
    EXPECT_TRUE(requestor->Signaling().BindSession(handle, 43) == CHIP_ERROR_INVALID_ARGUMENT);
    CameraCluster::SessionInfo info;
    ASSERT_TRUE(requestor->Signaling().GetSessionInfoForEnd(handle, info) == CHIP_NO_ERROR);
    EXPECT_FALSE(info.protocolId.has_value());
}

TEST_F(CameraRuntimeTests, ProviderRequiresCaseToExpectedPeerOverLargePayloadTransport)
{
    const auto handle = Create();
    CameraCluster::SessionInfo info;
    ASSERT_TRUE(requestor->Signaling().GetSessionInfo(handle, info) == CHIP_NO_ERROR);
    Transport::SecureSessionTable sessions;
    auto session = sessions.CreateNewSecureSessionForTest(Transport::SecureSession::Type::kCASE, 1, 0x4321, subject.subject,
                                                          kUndefinedCATs, 2, fabricIndex, GetDefaultMRPConfig());
    ASSERT_TRUE(session.HasValue());
    EXPECT_TRUE(Controller::CameraProviderClient::ValidateTransport(info, session.Value()) == CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
    session.Value()->AsSecureSession()->SetPeerAddress(
        Transport::PeerAddress::TCP(Inet::IPAddress::Loopback(Inet::IPAddressType::kIPv6)));
    EXPECT_TRUE(Controller::CameraProviderClient::ValidateTransport(info, session.Value()) == CHIP_NO_ERROR);
    info.metadata.peerNodeID = 0x5678;
    EXPECT_TRUE(Controller::CameraProviderClient::ValidateTransport(info, session.Value()) == CHIP_ERROR_ACCESS_DENIED);
    info.metadata.peerNodeID  = subject.subject;
    info.metadata.fabricIndex = 2;
    EXPECT_TRUE(Controller::CameraProviderClient::ValidateTransport(info, session.Value()) == CHIP_ERROR_ACCESS_DENIED);
    info.metadata.fabricIndex = fabricIndex;
    auto pase = sessions.CreateNewSecureSessionForTest(Transport::SecureSession::Type::kPASE, 3, 0x4321, subject.subject,
                                                       kUndefinedCATs, 4, fabricIndex, GetDefaultMRPConfig());
    ASSERT_TRUE(pase.HasValue());
    pase.Value()->AsSecureSession()->SetPeerAddress(
        Transport::PeerAddress::TCP(Inet::IPAddress::Loopback(Inet::IPAddressType::kIPv6)));
    EXPECT_TRUE(Controller::CameraProviderClient::ValidateTransport(info, pase.Value()) == CHIP_ERROR_ACCESS_DENIED);
}

TEST_F(CameraRuntimeTests, SessionCapacityAndNullableSentinelsAreRejected)
{
    uint64_t handle = 0;
    EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2,
                                                     DataModel::Nullable<uint16_t>(UINT16_MAX), DataModel::Nullable<uint16_t>(),
                                                     handle) == CHIP_ERROR_INVALID_ARGUMENT);
    for (uint16_t i = 0; i < CameraCluster::kMaxSessions; ++i)
    {
        handle = Create();
        ASSERT_TRUE(requestor->Signaling().BindSession(handle, i) == CHIP_NO_ERROR);
    }
    EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2,
                                                     DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(),
                                                     handle) == CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
}

TEST_F(CameraRuntimeTests, RetiredIdentityCapacityFailsClosedWithoutReusingProtocolIds)
{
    for (uint16_t i = 0; i < 128; ++i)
    {
        const auto handle = Create();
        ASSERT_TRUE(requestor->Signaling().BindSession(handle, i) == CHIP_NO_ERROR);
        ASSERT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_NO_ERROR);
    }
    const auto handle = Create();
    ASSERT_TRUE(requestor->Signaling().BindSession(handle, 128) == CHIP_NO_ERROR);
    EXPECT_TRUE(requestor->Signaling().CloseSession(handle) == CHIP_IM_GLOBAL_STATUS(ResourceExhausted));
    uint64_t next = 0;
    EXPECT_TRUE(requestor->Signaling().CreateSession(ScopedNodeId(subject.subject, fabricIndex), 2,
                                                     DataModel::Nullable<uint16_t>(1), DataModel::Nullable<uint16_t>(),
                                                     next) == CHIP_ERROR_INCORRECT_STATE);
}

TEST_F(CameraRuntimeTests, RegistrationWithoutInitializedAccessControlDoesNotPublishEndpoint)
{
    Access::AccessControl uninitialized;
    Controller::CameraRequestorEndpoint endpoint(8, Span<const DataModel::DeviceTypeEntry>(types), fabrics.GetFabricTable(),
                                                 system);
    EXPECT_TRUE(endpoint.Register(provider, uninitialized) == CHIP_ERROR_INCORRECT_STATE);
    ReadOnlyBufferBuilder<DataModel::EndpointEntry> endpoints;
    ASSERT_TRUE(provider.Endpoints(endpoints) == CHIP_NO_ERROR);
    const auto list = endpoints.TakeBuffer();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, 7);
}

} // namespace
