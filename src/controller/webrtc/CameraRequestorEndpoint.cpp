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

#include "CameraRequestorEndpoint.h"

#include <clusters/WebRTCTransportProvider/ClusterId.h>
#include <lib/support/CodeUtils.h>

#include <algorithm>

namespace chip::Controller {
namespace {
constexpr ClusterId kClientClusters[] = { app::Clusters::WebRTCTransportProvider::Id };
}

CameraRequestorEndpoint::CameraRequestorEndpoint(EndpointId endpoint, Span<const app::DataModel::DeviceTypeEntry> deviceTypes,
                                                 FabricTable & fabrics, System::Layer & system) :
    mSignaling(endpoint, fabrics, system),
    mDescriptor(endpoint, app::Clusters::DescriptorCluster::OptionalAttributesSet{},
                Span<const app::Clusters::DescriptorCluster::SemanticTag>{}),
    mEndpoint(app::SpanEndpoint::Builder().SetDeviceTypes(deviceTypes).SetClientClusters(Span(kClientClusters)).Build()),
    mRegistration(mEndpoint, { endpoint, kInvalidEndpointId, app::DataModel::EndpointCompositionPattern::kTree })
{
    VerifyOrDie(endpoint != kRootEndpointId && endpoint != kInvalidEndpointId);
    VerifyOrDie(!deviceTypes.empty());
}

CameraRequestorEndpoint::~CameraRequestorEndpoint()
{
    LogErrorOnFailure(Unregister());
}

CameraRequestorEndpoint::CameraRequestorEndpoint(EndpointId endpoint, Span<const app::DataModel::DeviceTypeEntry> deviceTypes,
                                                 FabricTable & fabrics, System::Layer & system,
                                                 app::Clusters::Binding::Manager & bindingManager,
                                                 DeviceLayer::PlatformManager & platformManager) :
    CameraRequestorEndpoint(endpoint, deviceTypes, fabrics, system)
{
    mBinding.emplace(app::Clusters::BindingCluster::Context{ bindingManager.GetBindingTable(), bindingManager, platformManager },
                     endpoint);
}

CHIP_ERROR CameraRequestorEndpoint::CheckAccess(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                                                Access::Privilege privilege)
{
    if (path.cluster == app::Clusters::Descriptor::Id || (mBinding.has_value() && path.cluster == app::Clusters::Binding::Id))
    {
        VerifyOrReturnError(privilege == Access::Privilege::kView && path.requestType == Access::RequestType::kAttributeReadRequest,
                            CHIP_ERROR_ACCESS_DENIED);
        auto authorizationPath    = path;
        authorizationPath.cluster = app::Clusters::WebRTCTransportRequestor::Id;
        return Signaling().CheckAccess(subject, authorizationPath, privilege);
    }
    return Signaling().CheckAccess(subject, path, privilege);
}

CHIP_ERROR CameraRequestorEndpoint::FindAvailableEndpoint(app::DataModel::Provider & provider, EndpointId & endpoint)
{
    ReadOnlyBufferBuilder<app::DataModel::EndpointEntry> builder;
    ReturnErrorOnFailure(provider.Endpoints(builder));
    const auto endpoints = builder.TakeBuffer();
    for (EndpointId candidate = 1; candidate != kInvalidEndpointId; ++candidate)
    {
        const bool occupied =
            std::any_of(endpoints.begin(), endpoints.end(), [candidate](const auto & entry) { return entry.id == candidate; });
        if (!occupied)
        {
            endpoint = candidate;
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_IM_GLOBAL_STATUS(ResourceExhausted);
}

CHIP_ERROR CameraRequestorEndpoint::Register(app::CodeDrivenDataModelProvider & provider, Access::AccessControl & access)
{
    VerifyOrReturnError(mProvider == nullptr, CHIP_ERROR_INCORRECT_STATE);
    size_t count = 0;
    // Fail before publishing metadata if access control has not been initialized.
    ReturnErrorOnFailure(access.GetEntryCount(count));
    ReadOnlyBufferBuilder<app::DataModel::EndpointEntry> endpoints;
    ReturnErrorOnFailure(provider.Endpoints(endpoints));
    const auto entries = endpoints.TakeBuffer();
    for (const auto & entry : entries)
    {
        VerifyOrReturnError(entry.id != GetEndpointId(), CHIP_ERROR_DUPLICATE_KEY_ID);
    }

    ReturnErrorOnFailure(provider.AddCluster(mSignaling.Registration()));
    CHIP_ERROR error = provider.AddCluster(mDescriptor.Registration());
    if (error != CHIP_NO_ERROR)
    {
        LogErrorOnFailure(provider.RemoveCluster(&mSignaling.Cluster()));
        return error;
    }
    if (mBinding.has_value())
    {
        error = provider.AddCluster(mBinding->Registration());
        if (error != CHIP_NO_ERROR)
        {
            LogErrorOnFailure(provider.RemoveCluster(&mDescriptor.Cluster()));
            LogErrorOnFailure(provider.RemoveCluster(&mSignaling.Cluster()));
            return error;
        }
    }
    error = provider.AddEndpoint(mRegistration);
    if (error != CHIP_NO_ERROR)
    {
        // AddEndpoint can publish metadata before a cluster Startup fails.
        CHIP_ERROR removal = provider.RemoveEndpoint(GetEndpointId());
        if (removal != CHIP_NO_ERROR && removal != CHIP_ERROR_NOT_FOUND)
        {
            ChipLogError(Camera, "Requestor endpoint rollback failed: %" CHIP_ERROR_FORMAT, removal.Format());
        }
        if (mBinding.has_value())
        {
            LogErrorOnFailure(provider.RemoveCluster(&mBinding->Cluster()));
        }
        LogErrorOnFailure(provider.RemoveCluster(&mDescriptor.Cluster()));
        LogErrorOnFailure(provider.RemoveCluster(&mSignaling.Cluster()));
        return error;
    }
    mProvider = &provider;
    provider.NotifyEndpointChanged(GetEndpointId(), app::DataModel::EndpointChangeType::kAdded);
    return CHIP_NO_ERROR;
}

CHIP_ERROR CameraRequestorEndpoint::Unregister()
{
    if (mProvider == nullptr)
    {
        return CHIP_NO_ERROR;
    }
    ReturnErrorOnFailure(mProvider->RemoveEndpoint(GetEndpointId()));
    if (mBinding.has_value())
    {
        ReturnErrorOnFailure(mProvider->RemoveCluster(&mBinding->Cluster()));
    }
    ReturnErrorOnFailure(mProvider->RemoveCluster(&mDescriptor.Cluster()));
    ReturnErrorOnFailure(mProvider->RemoveCluster(&mSignaling.Cluster()));
    mProvider->NotifyEndpointChanged(GetEndpointId(), app::DataModel::EndpointChangeType::kRemoved);
    mProvider = nullptr;
    return CHIP_NO_ERROR;
}

} // namespace chip::Controller
