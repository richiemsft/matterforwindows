/*
 *
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

/**
 *    @file
 *          Provides the shared System Layer singleton for the native Windows
 *          Device Layer foundation.
 *
 *          This is a focused analogue of src/platform/Globals.cpp. The shared
 *          Globals.cpp reaches the full Device Layer manager closure through
 *          <platform/internal/CHIPDeviceLayerInternal.h>, which is not part of
 *          this milestone. This translation unit provides only the System Layer
 *          accessors the PlatformManager foundation needs, backed by the native
 *          System::LayerImplWindows, without that closure. It is superseded by
 *          the shared Globals.cpp once the full Windows platform library is
 *          built.
 */

#include <system/windows/SystemLayerImplWindows.h>

namespace chip {
namespace DeviceLayer {

namespace {
chip::System::LayerImpl * gMockedSystemLayer = nullptr;
} // namespace

void SetSystemLayerForTesting(System::Layer * layer)
{
    gMockedSystemLayer = static_cast<System::LayerImpl *>(layer);
}

chip::System::LayerImpl & SystemLayerImpl()
{
    if (gMockedSystemLayer != nullptr)
    {
        return *gMockedSystemLayer;
    }

    static chip::System::LayerImpl gSystemLayerImpl;
    return gSystemLayerImpl;
}

chip::System::Layer & SystemLayer()
{
    return SystemLayerImpl();
}

chip::System::LayerSockets & SystemLayerSockets()
{
    return SystemLayerImpl();
}

} // namespace DeviceLayer
} // namespace chip
