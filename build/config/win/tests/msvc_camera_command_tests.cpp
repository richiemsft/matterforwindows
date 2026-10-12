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
#include <gtest/gtest.h>

#include <clusters/Descriptor/ClusterId.h>
#include <clusters/WebRTCTransportRequestor/ClusterId.h>
#include <commands/camera/CameraSignalingCommand.h>
#include <lib/support/CHIPMem.h>
#include <system/SystemLayerImpl.h>

#include <sstream>

namespace {
using namespace chip;

class CameraCommandTests : public ::testing::Test
{
public:
    static void SetUpTestSuite() { ASSERT_TRUE(Platform::MemoryInit() == CHIP_NO_ERROR); }
    static void TearDownTestSuite() { Platform::MemoryShutdown(); }
};

TEST_F(CameraCommandTests, OfferPreservesLineEndings)
{
    const std::string sdp = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n";
    std::istringstream input(sdp);
    std::string offer;
    ASSERT_TRUE(CameraSignalingCommand::ReadOffer(input, offer) == CHIP_NO_ERROR);
    EXPECT_EQ(offer, sdp);
}

TEST_F(CameraCommandTests, OfferAllowsExactLimit)
{
    const std::string sdp(Controller::CameraSignalingCluster::kMaxSdpBytes, 's');
    std::istringstream input(sdp);
    std::string offer;
    ASSERT_TRUE(CameraSignalingCommand::ReadOffer(input, offer) == CHIP_NO_ERROR);
    EXPECT_EQ(offer, sdp);
}

TEST_F(CameraCommandTests, OfferRejectsOverflowWithoutReturningTruncatedSuccess)
{
    std::istringstream input(std::string(Controller::CameraSignalingCluster::kMaxSdpBytes + 1, 's'));
    std::string offer = "old";
    EXPECT_TRUE(CameraSignalingCommand::ReadOffer(input, offer) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
    EXPECT_TRUE(offer.empty());
}

TEST_F(CameraCommandTests, OfferRejectsEmptyAndEmbeddedNul)
{
    for (const auto & text : { std::string(), std::string("v=0\0hidden", 10) })
    {
        std::istringstream input(text);
        std::string offer = "old";
        EXPECT_TRUE(CameraSignalingCommand::ReadOffer(input, offer) == CHIP_IM_GLOBAL_STATUS(ConstraintError));
        EXPECT_TRUE(offer.empty());
    }
}

TEST_F(CameraCommandTests, OfferSurfacesIoFailure)
{
    std::istringstream input("v=0");
    input.setstate(std::ios::badbit);
    std::string offer = "old";
    EXPECT_TRUE(CameraSignalingCommand::ReadOffer(input, offer) == CHIP_ERROR_READ_FAILED);
    EXPECT_TRUE(offer.empty());
}

class CommandConfiguration : public CameraSignalingCommand
{
public:
    CommandConfiguration() : CameraSignalingCommand(nullptr) {}
    using CameraSignalingCommand::ConfigureFactory;
    using CameraSignalingCommand::GetWaitDuration;
    using CameraSignalingCommand::NeedsOperationalAdvertising;
};

TEST_F(CameraCommandTests, StandaloneCommandEnablesCaseAndTcpWithCleanupBudget)
{
    CommandConfiguration command;
    EXPECT_TRUE(command.NeedsOperationalAdvertising());
    Controller::FactoryInitParams params;
    EXPECT_TRUE(command.ConfigureFactory(params) == CHIP_NO_ERROR);
    EXPECT_TRUE(params.enableTCPServer);
    EXPECT_EQ(params.listenPort, CHIP_PORT);
    params.listenPort = 12345;
    EXPECT_TRUE(command.ConfigureFactory(params) == CHIP_NO_ERROR);
    EXPECT_EQ(params.listenPort, 12345);
    ASSERT_NE(params.dataModelProvider, nullptr);
    EndpointId endpoint = kInvalidEndpointId;
    EXPECT_TRUE(Controller::CameraRequestorEndpoint::FindAvailableEndpoint(*params.dataModelProvider, endpoint) == CHIP_NO_ERROR);
    EXPECT_EQ(endpoint, 1);
    EXPECT_EQ(command.GetWaitDuration(), System::Clock::Seconds32(55));
}

TEST_F(CameraCommandTests, DescriptorAccessIsBoundToPeerFabricEndpointAndView)
{
    FabricTable fabrics;
    System::LayerImpl system;
    Controller::CameraSignalingCluster cluster(0, fabrics, system);
    CameraSignalingAccess access;
    access.SetRequestor(&cluster, ScopedNodeId(0x1234, 1));
    Access::SubjectDescriptor subject;
    subject.subject     = 0x1234;
    subject.fabricIndex = 1;
    subject.authMode    = Access::AuthMode::kCase;
    Access::RequestPath path;
    path.endpoint = 0;
    path.cluster  = app::Clusters::Descriptor::Id;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_NO_ERROR);
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    subject.subject = 0x5678;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    subject.subject     = 0x1234;
    subject.fabricIndex = 2;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    subject.fabricIndex     = 1;
    subject.isCommissioning = true;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    subject.isCommissioning = false;
    subject.authMode        = Access::AuthMode::kPase;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    subject.authMode = Access::AuthMode::kCase;
    path.endpoint    = 1;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    path.endpoint = 0;
    path.cluster  = 0x0006;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
    path.cluster = app::Clusters::WebRTCTransportRequestor::Id;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kOperate) == CHIP_ERROR_ACCESS_DENIED);
    access.SetRequestor(nullptr, ScopedNodeId());
    path.cluster = app::Clusters::Descriptor::Id;
    EXPECT_TRUE(access.Check(subject, path, Access::Privilege::kView) == CHIP_ERROR_ACCESS_DENIED);
}

TEST_F(CameraCommandTests, InteractiveRefusalLeavesExistingAclIntact)
{
    class Resolver : public Access::AccessControl::DeviceTypeResolver
    {
        bool IsDeviceTypeOnEndpoint(DeviceTypeId, EndpointId) override { return false; }
    } resolver;
    Access::AccessControl::Delegate delegate;
    auto & access = Access::GetAccessControl();
    ASSERT_TRUE(access.Init(&delegate, resolver) == CHIP_NO_ERROR);
    CameraSignalingCommand command(nullptr);
    EXPECT_TRUE(command.RunAsInteractive(Optional<char *>(), false) == CHIP_ERROR_INCORRECT_STATE);
    size_t count = 1;
    EXPECT_TRUE(access.GetEntryCount(count) == CHIP_NO_ERROR);
    EXPECT_EQ(count, 0u);
    access.Finish();
}
} // namespace
