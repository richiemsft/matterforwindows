/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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

#include <platform/ConnectivityManager.h>
#include <platform/internal/GenericConnectivityManagerImpl.h>
#include <platform/internal/GenericConnectivityManagerImpl_NoBLE.h>
#include <platform/internal/GenericConnectivityManagerImpl_NoThread.h>
#include <platform/internal/GenericConnectivityManagerImpl_NoWiFi.h>
#include <platform/internal/GenericConnectivityManagerImpl_TCP.h>
#include <platform/internal/GenericConnectivityManagerImpl_UDP.h>

namespace chip {
namespace DeviceLayer {

class ConnectivityManagerImpl final : public ConnectivityManager,
                                      public Internal::GenericConnectivityManagerImpl_NoBLE<ConnectivityManagerImpl>,
                                      public Internal::GenericConnectivityManagerImpl_NoThread<ConnectivityManagerImpl>,
                                      public Internal::GenericConnectivityManagerImpl_NoWiFi<ConnectivityManagerImpl>,
                                      public Internal::GenericConnectivityManagerImpl_UDP<ConnectivityManagerImpl>,
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
                                      public Internal::GenericConnectivityManagerImpl_TCP<ConnectivityManagerImpl>,
#endif
                                      public Internal::GenericConnectivityManagerImpl<ConnectivityManagerImpl>
{
    friend class ConnectivityManager;

public:
    CHIP_ERROR GetEthernetInterfaceName(char * name, size_t nameSize);
    CHIP_ERROR GetWiFiInterfaceName(char * name, size_t nameSize);
    CHIP_ERROR GetInterfaceStatus(const char * name, bool & isUp);

private:
    CHIP_ERROR _Init();
    void _OnPlatformEvent(const ChipDeviceEvent * event);
    Inet::InterfaceId _GetExternalInterface();

    friend ConnectivityManager & ConnectivityMgr();
    friend ConnectivityManagerImpl & ConnectivityMgrImpl();

    static ConnectivityManagerImpl sInstance;
};

inline ConnectivityManager & ConnectivityMgr()
{
    return ConnectivityManagerImpl::sInstance;
}

inline ConnectivityManagerImpl & ConnectivityMgrImpl()
{
    return ConnectivityManagerImpl::sInstance;
}

} // namespace DeviceLayer
} // namespace chip
