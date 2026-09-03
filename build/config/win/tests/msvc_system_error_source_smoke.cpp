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

#include <system/SystemError.h>

#include <Windows.h>

#include <cstring>

int main()
{
    const unsigned int expectedWin32Line = __LINE__ + 1;
    const CHIP_ERROR win32Error          = CHIP_ERROR_WINDOWS(ERROR_ACCESS_DENIED);
    if (win32Error.GetLine() != expectedWin32Line ||
        std::strstr(win32Error.GetFile(), "msvc_system_error_source_smoke.cpp") == nullptr)
    {
        return 1;
    }

    const unsigned int expectedHResultLine = __LINE__ + 1;
    const CHIP_ERROR hresultError           = CHIP_ERROR_HRESULT(E_FAIL);
    if (hresultError.GetLine() != expectedHResultLine ||
        std::strstr(hresultError.GetFile(), "msvc_system_error_source_smoke.cpp") == nullptr)
    {
        return 1;
    }

    return 0;
}
