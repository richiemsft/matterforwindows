/*
 * Copyright (c) 2026 Project CHIP Authors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "WebView2Receiver.h"

#include <Windows.h>
#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace CameraController {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr wchar_t kPage[]         = L"https://matter-camera.example.invalid/receiver.html";
constexpr size_t kMaxMessages     = 32;
constexpr size_t kMaxMessageBytes = 65536;
Json::Value Message(const char * type)
{
    Json::Value result;
    result["type"] = type;
    return result;
}
} // namespace

class WebView2Receiver::Impl
{
public:
    std::thread thread;
    std::mutex mutex;
    std::deque<Json::Value> incoming;
    std::deque<std::wstring> outgoing;
    std::optional<Json::Value> failure;
    std::atomic<bool> stop{ false };
    std::atomic<bool> finished{ false };
    Mode mode    = Mode::Receive;
    bool started = false;
    HWND window  = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;

    void Fail(const char * operation, HRESULT error)
    {
        ChipLogError(chipTool, "Native camera receiver %s failed (HRESULT 0x%08lx).", operation, static_cast<unsigned long>(error));
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure.has_value())
        {
            failure               = Message("error");
            (*failure)["message"] = operation;
        }
    }

    void Event(Json::Value value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (incoming.size() == kMaxMessages)
        {
            if (!failure.has_value())
            {
                failure               = Message("error");
                (*failure)["message"] = "Receiver event queue exhausted";
                ChipLogError(chipTool, "Native camera receiver event queue exhausted.");
            }
            return;
        }
        incoming.push_back(std::move(value));
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE)
        {
            auto * create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto * owner = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (owner != nullptr)
        {
            if (message == WM_SIZE && owner->controller)
            {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                const HRESULT error = owner->controller->put_Bounds(bounds);
                if (FAILED(error))
                {
                    owner->Fail("resize", error);
                }
            }
            if (message == WM_CLOSE)
            {
                owner->stop.store(true);
                owner->Event(Message("closed"));
                return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HRESULT Configure()
    {
        ComPtr<ICoreWebView2_3> mapping;
        HRESULT error = view.As(&mapping);
        if (FAILED(error))
        {
            return error;
        }
        std::vector<wchar_t> executable(32768);
        const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (size == 0 || size == executable.size())
        {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        const auto assets = std::filesystem::path(executable.data()).parent_path() / L"camera-receiver";
        error             = mapping->SetVirtualHostNameToFolderMapping(L"matter-camera.example.invalid", assets.c_str(),
                                                                       COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
        if (FAILED(error))
        {
            return error;
        }
        ComPtr<ICoreWebView2Settings> settings;
        error = view->get_Settings(&settings);
        if (FAILED(error))
        {
            return error;
        }
        if (FAILED(error = settings->put_IsScriptEnabled(TRUE)) || FAILED(error = settings->put_IsWebMessageEnabled(TRUE)) ||
            FAILED(error = settings->put_AreDevToolsEnabled(FALSE)) ||
            FAILED(error = settings->put_AreDefaultContextMenusEnabled(FALSE)) ||
            FAILED(error = settings->put_AreHostObjectsAllowed(FALSE)))
        {
            return error;
        }
        EventRegistrationToken token;
        error =
            view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                              [this](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs * args) -> HRESULT {
                                                  BOOL success         = FALSE;
                                                  const HRESULT result = args->get_IsSuccess(&success);
                                                  if (FAILED(result) || !success)
                                                  {
                                                      Fail("load trusted receiver page", FAILED(result) ? result : E_FAIL);
                                                  }
                                                  return S_OK;
                                              })
                                              .Get(),
                                          &token);
        if (FAILED(error))
        {
            return error;
        }
        error = view->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2 *,
                                                                     ICoreWebView2NavigationStartingEventArgs * args) -> HRESULT {
                LPWSTR uri           = nullptr;
                const HRESULT result = args->get_Uri(&uri);
                const bool trusted   = SUCCEEDED(result) && uri != nullptr && std::wcscmp(uri, kPage) == 0;
                CoTaskMemFree(uri);
                return args->put_Cancel(!trusted);
            }).Get(),
            &token);
        if (FAILED(error))
        {
            return error;
        }
        error = view->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2 *,
                                                                     ICoreWebView2NewWindowRequestedEventArgs * args) {
                return args->put_Handled(TRUE);
            }).Get(),
            &token);
        if (FAILED(error))
        {
            return error;
        }
        error = view->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2 *,
                                                                      ICoreWebView2PermissionRequestedEventArgs * args) {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(),
            &token);
        if (FAILED(error))
        {
            return error;
        }
        error = view->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>([this](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) {
                Fail("browser process", E_FAIL);
                return S_OK;
            }).Get(),
            &token);
        if (FAILED(error))
        {
            return error;
        }
        error = view->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](
                                                                      ICoreWebView2 *,
                                                                      ICoreWebView2WebMessageReceivedEventArgs * args) -> HRESULT {
                LPWSTR source      = nullptr;
                HRESULT result     = args->get_Source(&source);
                const bool trusted = SUCCEEDED(result) && source != nullptr && std::wcscmp(source, kPage) == 0;
                CoTaskMemFree(source);
                if (!trusted)
                {
                    Fail("untrusted bridge message", E_ACCESSDENIED);
                    return S_OK;
                }
                LPWSTR text = nullptr;
                result      = args->TryGetWebMessageAsString(&text);
                if (FAILED(result) || text == nullptr)
                {
                    CoTaskMemFree(text);
                    Fail("bridge message type", E_INVALIDARG);
                    return S_OK;
                }
                const size_t count = std::wcslen(text);
                if (count == 0 || count > kMaxMessageBytes)
                {
                    CoTaskMemFree(text);
                    Fail("bridge message size", E_INVALIDARG);
                    return S_OK;
                }
                const int bytes =
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
                if (bytes <= 0 || bytes > static_cast<int>(kMaxMessageBytes) ||
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(count), utf8.data(), bytes, nullptr,
                                        nullptr) != bytes)
                {
                    CoTaskMemFree(text);
                    Fail("bridge encoding", E_INVALIDARG);
                    return S_OK;
                }
                CoTaskMemFree(text);
                Json::CharReaderBuilder builder;
                builder["rejectDupKeys"] = true;
                builder["failIfExtra"]   = true;
                builder["stackLimit"]    = 16;
                Json::Value message;
                std::string errors;
                std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                if (!reader->parse(utf8.data(), utf8.data() + utf8.size(), &message, &errors) || !message.isObject() ||
                    !message["type"].isString())
                {
                    Fail("invalid bridge JSON", E_INVALIDARG);
                    return S_OK;
                }
                if (message["type"].asString() == "ready")
                {
                    ChipLogProgress(chipTool, "Native camera media engine ready.");
                    auto begin            = Message("begin");
                    begin["selfTest"]     = mode == Mode::SelfTest;
                    begin["testProvider"] = mode == Mode::TestProvider;
                    Json::StreamWriterBuilder writer;
                    const auto json = Json::writeString(writer, begin);
                    std::wstring wide(json.begin(), json.end());
                    result = view->PostWebMessageAsJson(wide.c_str());
                    if (FAILED(result))
                    {
                        Fail("begin media", result);
                    }
                }
                else
                {
                    Event(std::move(message));
                }
                return S_OK;
            }).Get(),
            &token);
        if (FAILED(error))
        {
            return error;
        }
        return view->Navigate(kPage);
    }

    void Run()
    {
        HRESULT error = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(error))
        {
            Fail("COM initialization", error);
            return;
        }
        WNDCLASSW klass{};
        klass.lpfnWndProc   = WindowProc;
        klass.hInstance     = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"MatterNativeCameraReceiver";
        klass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        if (RegisterClassW(&klass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Fail("window class", HRESULT_FROM_WIN32(GetLastError()));
            CoUninitialize();
            return;
        }
        window = CreateWindowExW(0, klass.lpszClassName, L"Matter native camera - receive-only H.264", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 960, 600, nullptr, nullptr, klass.hInstance, this);
        if (window == nullptr)
        {
            Fail("window creation", HRESULT_FROM_WIN32(GetLastError()));
            CoUninitialize();
            return;
        }
        ShowWindow(window, SW_SHOW);
        // Media storage is deliberately separate from Matter fabric/credential storage.
        wchar_t localAppData[32768];
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, 32768);
        if (count == 0 || count >= 32768)
        {
            Fail("LOCALAPPDATA", E_FAIL);
            stop.store(true);
        }
        else
        {
            const auto profile = std::filesystem::path(localAppData) / L"MatterNativeCamera" / L"WebView2";
            error              = CreateCoreWebView2EnvironmentWithOptions(
                nullptr, profile.c_str(), nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result,
                                                                                            ICoreWebView2Environment * environment)
                                                                                         -> HRESULT {
                    if (FAILED(result) || environment == nullptr)
                    {
                        Fail("WebView2 environment (Evergreen runtime required)", result);
                        return S_OK;
                    }
                    const HRESULT creation = environment->CreateCoreWebView2Controller(
                        window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT status, ICoreWebView2Controller * created) -> HRESULT {
                                if (FAILED(status) || created == nullptr)
                                {
                                    Fail("WebView2 controller", status);
                                    return S_OK;
                                }
                                controller = created;
                                status     = controller->get_CoreWebView2(&view);
                                if (SUCCEEDED(status))
                                {
                                    RECT bounds;
                                    GetClientRect(window, &bounds);
                                    status = controller->put_Bounds(bounds);
                                }
                                if (SUCCEEDED(status))
                                {
                                    status = Configure();
                                }
                                if (FAILED(status))
                                {
                                    Fail("WebView2 configuration", status);
                                }
                                return S_OK;
                            })
                            .Get());
                    if (FAILED(creation))
                    {
                        Fail("create WebView2 controller", creation);
                    }
                    return S_OK;
                }).Get());
            if (FAILED(error))
            {
                Fail("create WebView2 environment", error);
            }
        }
        while (!stop.load())
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::deque<std::wstring> messages;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (view)
                {
                    messages.swap(outgoing);
                }
            }
            for (const auto & json : messages)
            {
                error = view->PostWebMessageAsJson(json.c_str());
                if (FAILED(error))
                {
                    Fail("deliver signaling", error);
                }
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        }
        if (controller)
        {
            error = controller->Close();
            if (FAILED(error))
            {
                Fail("close WebView2 controller", error);
            }
        }
        view.Reset();
        controller.Reset();
        DestroyWindow(window);
        window = nullptr;
        CoUninitialize();
    }
};

WebView2Receiver::WebView2Receiver() : mImpl(std::make_shared<Impl>()) {}
WebView2Receiver::~WebView2Receiver()
{
    RequestStop();
    LogErrorOnFailure(Join());
}
CHIP_ERROR WebView2Receiver::Start(Mode mode)
{
    VerifyOrReturnError(!mImpl->started, CHIP_ERROR_INCORRECT_STATE);
    mImpl->mode    = mode;
    mImpl->started = true;
    mImpl->thread  = std::thread([state = mImpl] {
        state->Run();
        state->finished.store(true);
    });
    return CHIP_NO_ERROR;
}
CHIP_ERROR WebView2Receiver::Send(Json::Value message)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const auto json       = Json::writeString(writer, message);
    VerifyOrReturnError(!json.empty() && json.size() <= kMaxMessageBytes, CHIP_ERROR_INVALID_ARGUMENT);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(), static_cast<int>(json.size()), nullptr, 0);
    VerifyOrReturnError(count > 0, CHIP_ERROR_INVALID_ARGUMENT);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    VerifyOrReturnError(
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(), static_cast<int>(json.size()), wide.data(), count) == count,
        CHIP_ERROR_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    VerifyOrReturnError(mImpl->started && !mImpl->stop.load(), CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(mImpl->outgoing.size() < kMaxMessages, CHIP_ERROR_NO_MEMORY);
    mImpl->outgoing.push_back(std::move(wide));
    return CHIP_NO_ERROR;
}
std::optional<Json::Value> WebView2Receiver::TakeEvent()
{
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    if (mImpl->failure.has_value())
    {
        auto result = std::move(mImpl->failure);
        mImpl->failure.reset();
        return result;
    }
    if (mImpl->incoming.empty())
    {
        return std::nullopt;
    }
    Json::Value result = std::move(mImpl->incoming.front());
    mImpl->incoming.pop_front();
    return result;
}
void WebView2Receiver::RequestStop()
{
    mImpl->stop.store(true);
}
CHIP_ERROR WebView2Receiver::Join()
{
    if (mImpl->thread.joinable())
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!mImpl->finished.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!mImpl->finished.load())
        {
            // The worker retains its media-only state; it never borrows Matter objects.
            mImpl->thread.detach();
            ChipLogError(chipTool, "Native camera UI shutdown exceeded ten seconds.");
            return CHIP_ERROR_TIMEOUT;
        }
        mImpl->thread.join();
    }
    return CHIP_NO_ERROR;
}

int RunMediaSelfTest()
{
    WebView2Receiver receiver;
    if (receiver.Start(WebView2Receiver::Mode::SelfTest) != CHIP_NO_ERROR)
    {
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool decoded        = false;
    while (std::chrono::steady_clock::now() < deadline && !decoded)
    {
        if (auto event = receiver.TakeEvent(); event.has_value())
        {
            const auto type = (*event)["type"].asString();
            ChipLogDetail(chipTool, "Media self-test event: %s.", type.c_str());
            if (type == "self-test-stats")
            {
                ChipLogDetail(chipTool, "Media stats: %s, frames=%llu, rendered=%u, %ux%u.", (*event)["kind"].asCString(),
                              static_cast<unsigned long long>((*event)["frames"].asUInt64()), (*event)["seen"].asBool(),
                              (*event)["width"].asUInt(), (*event)["height"].asUInt());
            }
            if (type == "decoded" && (*event)["codec"] == "video/H264" && (*event)["frames"].isUInt64() &&
                (*event)["frames"].asUInt64() > 0)
            {
                decoded = true;
            }
            else if (type == "closed" || type == "error")
            {
                ChipLogError(chipTool, "Media self-test %s: %s.", type.c_str(),
                             (*event)["message"].isString() ? (*event)["message"].asCString() : "window closed");
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    receiver.RequestStop();
    if (receiver.Join() != CHIP_NO_ERROR)
    {
        decoded = false;
    }
    while (auto event = receiver.TakeEvent())
    {
        if ((*event)["type"] == "error")
        {
            decoded = false;
        }
    }
    ChipLogProgress(chipTool, "Native WebRTC H.264 decoded-frame self-test: %s (not a Matter camera interoperability test).",
                    decoded ? "PASS" : "FAIL");
    return decoded ? 0 : 1;
}
} // namespace CameraController
