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

#include <controller/webrtc/CameraSignalingCluster.h>

#include <access/AccessControl.h>
#include <app/clusters/bindings/BindingCluster.h>
#include <app/clusters/descriptor/DescriptorCluster.h>
#include <data-model-providers/codedriven/CodeDrivenDataModelProvider.h>
#include <data-model-providers/codedriven/endpoint/SpanEndpoint.h>

#include <optional>

namespace chip::Controller {

/// Owns its requestor endpoint, Descriptor, and optionally the shared Binding cluster.
/// Borrowed dependencies, including an initialized binding manager, must outlive it.
class CameraRequestorEndpoint
{
public:
    CameraRequestorEndpoint(EndpointId endpoint, Span<const app::DataModel::DeviceTypeEntry> deviceTypes, FabricTable & fabrics,
                            System::Layer & system);
    CameraRequestorEndpoint(EndpointId endpoint, Span<const app::DataModel::DeviceTypeEntry> deviceTypes, FabricTable & fabrics,
                            System::Layer & system, app::Clusters::Binding::Manager & bindingManager,
                            DeviceLayer::PlatformManager & platformManager);
    ~CameraRequestorEndpoint();
    CameraRequestorEndpoint(const CameraRequestorEndpoint &)             = delete;
    CameraRequestorEndpoint & operator=(const CameraRequestorEndpoint &) = delete;

    static CHIP_ERROR FindAvailableEndpoint(app::DataModel::Provider & provider, EndpointId & endpoint);
    CHIP_ERROR Register(app::CodeDrivenDataModelProvider & provider, Access::AccessControl & access);
    CHIP_ERROR Unregister();
    CHIP_ERROR CheckAccess(const Access::SubjectDescriptor & subject, const Access::RequestPath & path,
                           Access::Privilege privilege);
    CameraSignalingCluster & Signaling() { return mSignaling.Cluster(); }
    EndpointId GetEndpointId() const { return mRegistration.endpointEntry.id; }

private:
    app::RegisteredServerCluster<CameraSignalingCluster> mSignaling;
    app::RegisteredServerCluster<app::Clusters::DescriptorCluster> mDescriptor;
    std::optional<app::RegisteredServerCluster<app::Clusters::BindingCluster>> mBinding;
    app::SpanEndpoint mEndpoint;
    app::EndpointInterfaceRegistration mRegistration;
    app::CodeDrivenDataModelProvider * mProvider = nullptr;
};

} // namespace chip::Controller
