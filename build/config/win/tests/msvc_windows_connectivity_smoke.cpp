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

#include <platform/ConnectivityManager.h>

#include <cstdio>

using namespace chip;
using namespace chip::DeviceLayer;

namespace {

int gChecks = 0;

#define CHECK(condition)                                                                                                           \
    do                                                                                                                             \
    {                                                                                                                              \
        ++gChecks;                                                                                                                 \
        if (!(condition))                                                                                                          \
        {                                                                                                                          \
            std::printf("Windows connectivity smoke failed at check %d: %s\n", gChecks, #condition);                              \
            return 1;                                                                                                              \
        }                                                                                                                          \
    } while (0)

} // namespace

int main()
{
    CHECK(ConnectivityMgr().GetWiFiStationMode() == ConnectivityManager::kWiFiStationMode_NotSupported);
    CHECK(!ConnectivityMgr().IsThreadEnabled());
    CHECK(ConnectivityMgr().GetBleLayer() == nullptr);

    char name[Inet::InterfaceId::kMaxIfNameLength] = {};
    CHIP_ERROR ethernet = ConnectivityMgrImpl().GetEthernetInterfaceName(name, sizeof(name));
    CHIP_ERROR wifi     = ConnectivityMgrImpl().GetWiFiInterfaceName(name, sizeof(name));
    CHECK(ethernet == CHIP_NO_ERROR || ethernet == CHIP_ERROR_KEY_NOT_FOUND);
    CHECK(wifi == CHIP_NO_ERROR || wifi == CHIP_ERROR_KEY_NOT_FOUND);

    bool hasEthernet = false;
    bool hasWiFi     = false;
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceType type;
        if (!iterator.IsLoopback() && iterator.GetInterfaceType(type) == CHIP_NO_ERROR)
        {
            hasEthernet = hasEthernet || type == Inet::InterfaceType::Ethernet;
            hasWiFi     = hasWiFi || type == Inet::InterfaceType::WiFi;
        }
        CHECK(iterator.GetInterfaceName(name, sizeof(name)) == CHIP_NO_ERROR);
        bool isUp = !iterator.IsUp();
        CHECK(ConnectivityMgrImpl().GetInterfaceStatus(name, isUp) == CHIP_NO_ERROR);
        CHECK(isUp == iterator.IsUp());
    }
    CHECK(!hasEthernet || ethernet == CHIP_NO_ERROR);
    CHECK(!hasWiFi || wifi == CHIP_NO_ERROR);

    const Inet::InterfaceId external = ConnectivityMgr().GetExternalInterface();
    if (external.IsPresent())
    {
        CHECK(external.GetInterfaceName(name, sizeof(name)) == CHIP_NO_ERROR);
        bool isUp = false;
        CHECK(ConnectivityMgrImpl().GetInterfaceStatus(name, isUp) == CHIP_NO_ERROR);
        CHECK(isUp);
        bool hasAddress = false;
        for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
        {
            if (iterator.GetInterfaceId() == external && iterator.IsUp() && !iterator.IsLoopback() && iterator.SupportsMulticast())
            {
                hasAddress = true;
                break;
            }
        }
        CHECK(hasAddress);
    }

    bool ignored = false;
    CHECK(ConnectivityMgrImpl().GetInterfaceStatus(nullptr, ignored) == CHIP_ERROR_INVALID_ARGUMENT);
    std::printf("Windows connectivity smoke passed (%d checks)\n", gChecks);
    return 0;
}
