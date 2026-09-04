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
 *          Provides the implementation of the PlatformManager object for native
 *          Windows hosts.
 */

#include <platform/PlatformManager.h>
#if defined(CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION) && CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION
#include <platform/ConfigurationManager.h>
#include <platform/ConnectivityManager.h>
#endif

// Pull in the non-inline definitions for the generic Windows PlatformManager
// implementation from which PlatformManagerImpl inherits.
#include <platform/internal/GenericPlatformManagerImpl_Windows.ipp>

#include <system/SystemClock.h>

namespace chip {
namespace DeviceLayer {

PlatformManagerImpl PlatformManagerImpl::sInstance;

CHIP_ERROR PlatformManagerImpl::_InitChipStack()
{
    mStartTime = System::SystemClock().GetMonotonicTimestamp();

    ReturnErrorOnFailure(Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_InitChipStack());

#if defined(CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION) && CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION
    CHIP_ERROR error = ConfigurationMgr().Init();
    if (error != CHIP_NO_ERROR)
    {
        Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_Shutdown();
        return error;
    }

    error = UDPEndPointManager()->Init(SystemLayer());
    if (error != CHIP_NO_ERROR)
    {
        ConfigurationManagerImpl::GetDefaultInstance().Shutdown();
        Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_Shutdown();
        return error;
    }

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    error = TCPEndPointManager()->Init(SystemLayer());
    if (error != CHIP_NO_ERROR)
    {
        UDPEndPointManager()->Shutdown();
        ConfigurationManagerImpl::GetDefaultInstance().Shutdown();
        Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_Shutdown();
        return error;
    }
#endif

    error = ConnectivityMgr().Init();
    if (error != CHIP_NO_ERROR)
    {
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
        TCPEndPointManager()->Shutdown();
#endif
        UDPEndPointManager()->Shutdown();
        ConfigurationManagerImpl::GetDefaultInstance().Shutdown();
        Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_Shutdown();
    }
    return error;
#else
    return CHIP_NO_ERROR;
#endif
}

void PlatformManagerImpl::_Shutdown()
{
#if defined(CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION) && CHIP_WINDOWS_DEVICE_LAYER_COMPOSITION
    UDPEndPointManager()->Shutdown();
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    TCPEndPointManager()->Shutdown();
#endif
    ConfigurationManagerImpl::GetDefaultInstance().Shutdown();
#endif
    Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_Shutdown();
}

} // namespace DeviceLayer
} // namespace chip
