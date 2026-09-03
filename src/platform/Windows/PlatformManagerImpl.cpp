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

    return Internal::GenericPlatformManagerImpl_Windows<PlatformManagerImpl>::_InitChipStack();
}

} // namespace DeviceLayer
} // namespace chip
