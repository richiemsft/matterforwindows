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

#include "NamedPipeCommands.h"

#include <cctype>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemError.h>

using namespace chip;

std::wstring NamedPipeCommands::PipePath(const std::string & name)
{
    std::wstring result = LR"(\\.\pipe\matter-)";
    for (unsigned char character : name)
    {
        result.push_back(std::isalnum(character) || character == '.' || character == '-' || character == '_'
                             ? static_cast<wchar_t>(character)
                             : L'_');
    }
    return result;
}

CHIP_ERROR NamedPipeCommands::Start(const std::string & name, NamedPipeCommandDelegate * delegate)
{
    VerifyOrReturnError(!name.empty() && delegate != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(!mRunning.exchange(true), CHIP_ERROR_INCORRECT_STATE);

    mPipePath = PipePath(name);
    mDelegate = delegate;
    mListener = CreateThread(nullptr, 0, ListenerThread, this, 0, nullptr);
    if (mListener == nullptr)
    {
        mRunning = false;
        mDelegate = nullptr;
        return CHIP_ERROR_WINDOWS(GetLastError());
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR NamedPipeCommands::Stop()
{
    if (!mRunning.exchange(false))
    {
        return CHIP_NO_ERROR;
    }

    if (mListener != nullptr)
    {
        while (WaitForSingleObject(mListener, 10) == WAIT_TIMEOUT)
        {
            (void) CancelSynchronousIo(mListener);
            HANDLE wakePipe = CreateFileW(mPipePath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (wakePipe != INVALID_HANDLE_VALUE)
            {
                CloseHandle(wakePipe);
            }
        }
        CloseHandle(mListener);
        mListener = nullptr;
    }
    mDelegate = nullptr;
    return CHIP_NO_ERROR;
}

DWORD WINAPI NamedPipeCommands::ListenerThread(void * context)
{
    static_cast<NamedPipeCommands *>(context)->Listen();
    return 0;
}

void NamedPipeCommands::Listen()
{
    while (mRunning)
    {
        HANDLE pipe =
            CreateNamedPipeW(mPipePath.c_str(), PIPE_ACCESS_INBOUND,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            ChipLogError(NotSpecified, "CreateNamedPipe failed: %lu", GetLastError());
            break;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && mRunning)
        {
            std::string pending;
            std::vector<char> buffer(1024);
            DWORD bytesRead = 0;
            while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
            {
                pending.append(buffer.data(), bytesRead);
                size_t newline = 0;
                while ((newline = pending.find('\n')) != std::string::npos)
                {
                    std::string command = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!command.empty())
                    {
                        mDelegate->OnEventCommandReceived(command.c_str());
                    }
                }
            }
            if (!pending.empty())
            {
                mDelegate->OnEventCommandReceived(pending.c_str());
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
