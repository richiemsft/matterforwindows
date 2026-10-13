/*
 * Copyright (c) 2026 Project CHIP Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "LiveViewCommand.h"

#include <gtest/gtest.h>
#include <lib/support/CHIPMem.h>

namespace CameraController {

class LiveViewCommandTestAccess
{
public:
    static CHIP_ERROR Event(LiveViewCommand & command, const Json::Value & event) { return command.MediaEvent(event); }
    static bool Playing(const LiveViewCommand & command) { return command.mPlaying; }
    static bool OwnsStream(const LiveViewCommand & command) { return command.mStream.has_value(); }
    static void AnswerApplied(LiveViewCommand & command)
    {
        command.mAnswerApplied = true;
        command.mAllocated.emplace();
        command.mAllocated->minResolution = command.mSelection.minResolution;
        command.mAllocated->maxResolution = command.mSelection.maxResolution;
    }
    static void NarrowAllocation(LiveViewCommand & command)
    {
        command.mSelection.maxResolution.width  = 1920;
        command.mAllocated->maxResolution.width = 1280;
    }
    static CHIP_ERROR Discover(LiveViewCommand & command, chip::TLV::TLVReader & reader)
    {
        command.mReadStep = LiveViewCommand::ReadStep::Parts;
        return command.DecodeRead(reader);
    }
    static CHIP_ERROR Capability(LiveViewCommand & command, chip::TLV::TLVReader & reader, bool privacy)
    {
        command.mReadStep = privacy ? LiveViewCommand::ReadStep::HardPrivacy : LiveViewCommand::ReadStep::Features;
        return command.DecodeRead(reader);
    }
    static void PendingAllocate(LiveViewCommand & command)
    {
        command.mComplete = false;
        command.mCamera   = 1;
        command.mInvoke   = LiveViewCommand::Invoke::Allocate;
    }
    static void Stop(LiveViewCommand & command) { command.Stop(CHIP_ERROR_CANCELLED); }
    static void AllocationResponse(LiveViewCommand & command, chip::TLV::TLVReader & data)
    {
        chip::app::CommandSender::ResponseData response{
            chip::app::ConcreteCommandPath(1, Av::Id, Av::Commands::VideoStreamAllocateResponse::Id), chip::app::StatusIB(), &data
        };
        command.OnResponse(nullptr, response);
    }
};
namespace {
using namespace chip;

class CameraLiveViewTests : public testing::Test
{
public:
    static void SetUpTestSuite() { ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }
    Av::Commands::VideoStreamAllocate::Type request;
    Av::Structs::VideoStreamStruct::Type stream;
    void SetUp() override
    {
        request.streamUsage         = app::Clusters::Globals::StreamUsageEnum::kLiveView;
        request.videoCodec          = Av::VideoCodecEnum::kH264;
        request.minResolution.width = request.maxResolution.width = 640;
        request.minResolution.height = request.maxResolution.height = 480;
        request.minFrameRate = request.maxFrameRate = 30;
        request.minBitRate                          = 500000;
        request.maxBitRate                          = 2000000;
        request.keyFrameInterval                    = 1000;
        stream.streamUsage                          = request.streamUsage;
        stream.videoCodec                           = request.videoCodec;
        stream.minResolution                        = request.minResolution;
        stream.maxResolution                        = request.maxResolution;
        stream.minFrameRate                         = request.minFrameRate;
        stream.maxFrameRate                         = request.maxFrameRate;
        stream.minBitRate                           = request.minBitRate;
        stream.maxBitRate                           = request.maxBitRate;
        stream.keyFrameInterval                     = request.keyFrameInterval;
        stream.referenceCount                       = 0;
    }
    Json::Value Frame()
    {
        Json::Value value;
        value["type"]   = "decoded";
        value["codec"]  = "video/H264";
        value["frames"] = Json::UInt64(1);
        value["width"]  = 640;
        value["height"] = 480;
        return value;
    }
};

TEST_F(CameraLiveViewTests, ExactSelectionAndAllocationAreAccepted)
{
    EXPECT_TRUE(ValidateSelection(request) == CHIP_NO_ERROR);
    EXPECT_TRUE(ValidateAllocatedStream(request, stream) == CHIP_NO_ERROR);
}

TEST_F(CameraLiveViewTests, InvalidRangesAndZeroValuesAreRejected)
{
    request.maxFrameRate = 29;
    EXPECT_TRUE(ValidateSelection(request) == CHIP_ERROR_INVALID_ARGUMENT);
    request.maxFrameRate = 30;
    request.minBitRate   = 0;
    EXPECT_TRUE(ValidateSelection(request) == CHIP_ERROR_INVALID_ARGUMENT);
}

TEST_F(CameraLiveViewTests, UnsupportedCodecAndUsageAreRejected)
{
    request.videoCodec = Av::VideoCodecEnum::kHevc;
    EXPECT_TRUE(ValidateSelection(request) == CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
    request.videoCodec  = Av::VideoCodecEnum::kH264;
    request.streamUsage = app::Clusters::Globals::StreamUsageEnum::kUnknownEnumValue;
    EXPECT_TRUE(ValidateSelection(request) == CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
}

TEST_F(CameraLiveViewTests, SensorLimitsAreNotSilentlyClamped)
{
    Av::Structs::VideoSensorParamsStruct::Type sensor;
    sensor.sensorWidth  = 1920;
    sensor.sensorHeight = 1080;
    sensor.maxFPS       = 30;
    Av::Structs::VideoResolutionStruct::Type viewport;
    viewport.width  = 320;
    viewport.height = 240;
    EXPECT_TRUE(ValidateSensor(request, sensor, viewport) == CHIP_NO_ERROR);
    sensor.maxFPS = 29;
    EXPECT_TRUE(ValidateSensor(request, sensor, viewport) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    EXPECT_EQ(request.maxFrameRate, 30);
}

TEST_F(CameraLiveViewTests, AllocationCannotChangeCodecOrDimensions)
{
    stream.videoCodec = Av::VideoCodecEnum::kAv1;
    EXPECT_TRUE(ValidateAllocatedStream(request, stream) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    stream.videoCodec          = request.videoCodec;
    stream.maxResolution.width = 1920;
    EXPECT_TRUE(ValidateAllocatedStream(request, stream) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
}

TEST_F(CameraLiveViewTests, TransportReferenceCountIsNotAllocationOwnership)
{
    for (const uint8_t count : { uint8_t(0), uint8_t(1), uint8_t(UINT8_MAX) })
    {
        stream.referenceCount = count;
        EXPECT_TRUE(ValidateAllocatedStream(request, stream) == CHIP_NO_ERROR);
        EXPECT_EQ(stream.referenceCount, count);
    }
}

TEST_F(CameraLiveViewTests, SignalingOrConnectedEventIsNotPlayback)
{
    LiveViewCommand command(nullptr);
    Json::Value event;
    event["type"] = "connected";
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_NO_ERROR);
    EXPECT_FALSE(LiveViewCommandTestAccess::Playing(command));
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, Frame()) == CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(LiveViewCommandTestAccess::Playing(command));
}

TEST_F(CameraLiveViewTests, PlaybackRequiresDecodedH264AndValidDimensions)
{
    LiveViewCommand command(nullptr);
    LiveViewCommandTestAccess::AnswerApplied(command);
    auto frame     = Frame();
    frame["codec"] = "video/VP8";
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, frame) == CHIP_ERROR_INVALID_ARGUMENT);
    frame          = Frame();
    frame["width"] = 1920;
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, frame) == CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(LiveViewCommandTestAccess::Playing(command));
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, Frame()) == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewCommandTestAccess::Playing(command));
}

TEST_F(CameraLiveViewTests, FrameCounterCannotMoveBackwards)
{
    LiveViewCommand command(nullptr);
    LiveViewCommandTestAccess::AnswerApplied(command);
    auto frame      = Frame();
    frame["frames"] = Json::UInt64(2);
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, frame) == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, Frame()) == CHIP_ERROR_INVALID_ARGUMENT);
}

TEST_F(CameraLiveViewTests, DecodedDimensionsMustFitActualAllocationNotJustRequestedBounds)
{
    LiveViewCommand command(nullptr);
    LiveViewCommandTestAccess::AnswerApplied(command);
    LiveViewCommandTestAccess::NarrowAllocation(command);
    auto frame     = Frame();
    frame["width"] = 1920;
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, frame) == CHIP_ERROR_INVALID_ARGUMENT);
    EXPECT_FALSE(LiveViewCommandTestAccess::Playing(command));
    frame["width"] = 1280;
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, frame) == CHIP_NO_ERROR);
}

TEST_F(CameraLiveViewTests, DuplicateOffersAndEmbeddedNulAreRejected)
{
    LiveViewCommand command(nullptr);
    Json::Value event;
    event["type"] = "offer";
    event["sdp"]  = std::string("v=0\0hidden", 10);
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_ERROR_INVALID_ARGUMENT);
    event["sdp"] = "v=0\r\n";
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_ERROR_INVALID_ARGUMENT);
}

TEST_F(CameraLiveViewTests, IceQueueIsBoundedAndCannotContainInvalidLineIndex)
{
    LiveViewCommand command(nullptr);
    Json::Value event;
    event["type"]                       = "candidate";
    event["candidate"]["candidate"]     = "candidate:1";
    event["candidate"]["sdpMid"]        = "0";
    event["candidate"]["sdpMLineIndex"] = 65536;
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_ERROR_INVALID_ARGUMENT);
    event["candidate"]["sdpMLineIndex"] = 0;
    for (size_t i = 0; i < 128; ++i)
    {
        EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_NO_ERROR);
    }
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, event) == CHIP_ERROR_INVALID_ARGUMENT);
}

TEST_F(CameraLiveViewTests, LateAllocationResponseRemainsOwnedAfterCancellation)
{
    LiveViewCommand command(nullptr);
    LiveViewCommandTestAccess::PendingAllocate(command);
    LiveViewCommandTestAccess::Stop(command);
    uint8_t buffer[64];
    TLV::TLVWriter writer;
    writer.Init(buffer);
    app::DataModel::FabricAwareTLVWriter fabricWriter(writer, 1);
    Av::Commands::VideoStreamAllocateResponse::Type response;
    response.videoStreamID = 17;
    ASSERT_TRUE(response.Encode(fabricWriter, TLV::AnonymousTag()) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    TLV::TLVReader reader;
    reader.Init(buffer, writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    LiveViewCommandTestAccess::AllocationResponse(command, reader);
    EXPECT_TRUE(LiveViewCommandTestAccess::OwnsStream(command));
}

TEST_F(CameraLiveViewTests, CameraPrivacyAndMissingVideoFeatureFailClosed)
{
    LiveViewCommand command(nullptr);
    uint8_t buffer[32];
    TLV::TLVWriter writer;
    writer.Init(buffer);
    ASSERT_TRUE(writer.Put(TLV::AnonymousTag(), true) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    TLV::TLVReader reader;
    reader.Init(buffer, writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewCommandTestAccess::Capability(command, reader, true) == CHIP_ERROR_ACCESS_DENIED);
    writer.Init(buffer);
    ASSERT_TRUE(writer.Put(TLV::AnonymousTag(), uint32_t(0)) == CHIP_NO_ERROR);
    ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
    reader.Init(buffer, writer.GetLengthWritten());
    ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
    EXPECT_TRUE(LiveViewCommandTestAccess::Capability(command, reader, false) == CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
}

TEST_F(CameraLiveViewTests, EndpointDiscoveryRejectsRootAndUnboundedLists)
{
    for (size_t count : { size_t(1), size_t(129) })
    {
        LiveViewCommand command(nullptr);
        uint8_t buffer[1024];
        TLV::TLVWriter writer;
        writer.Init(buffer);
        TLV::TLVType container;
        ASSERT_TRUE(writer.StartContainer(TLV::AnonymousTag(), TLV::kTLVType_Array, container) == CHIP_NO_ERROR);
        for (size_t i = 0; i < count; ++i)
        {
            ASSERT_TRUE(writer.Put(TLV::AnonymousTag(), static_cast<EndpointId>(i)) == CHIP_NO_ERROR);
        }
        ASSERT_TRUE(writer.EndContainer(container) == CHIP_NO_ERROR);
        ASSERT_TRUE(writer.Finalize() == CHIP_NO_ERROR);
        TLV::TLVReader reader;
        reader.Init(buffer, writer.GetLengthWritten());
        ASSERT_TRUE(reader.Next() == CHIP_NO_ERROR);
        EXPECT_TRUE(LiveViewCommandTestAccess::Discover(command, reader) ==
                    (count == 1 ? CHIP_ERROR_INVALID_ARGUMENT : CHIP_ERROR_BUFFER_TOO_SMALL));
    }
}

TEST_F(CameraLiveViewTests, RemoteMediaErrorIsNotSuccessfulPlayback)
{
    LiveViewCommand command(nullptr);
    Json::Value error;
    error["type"] = "error";
    EXPECT_TRUE(LiveViewCommandTestAccess::Event(command, error) == CHIP_ERROR_NOT_CONNECTED);
    EXPECT_FALSE(LiveViewCommandTestAccess::Playing(command));
}

TEST_F(CameraLiveViewTests, MediaShutdownBeforeThreadStartupIsSafe)
{
    WebView2Receiver receiver;
    receiver.RequestStop();
    EXPECT_TRUE(receiver.Join() == CHIP_NO_ERROR);
}
} // namespace
} // namespace CameraController
