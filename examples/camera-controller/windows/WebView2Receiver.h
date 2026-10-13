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
#pragma once

#include <json/json.h>
#include <lib/core/CHIPError.h>

#include <memory>
#include <optional>

namespace CameraController {

// The UI thread owns COM/WebRTC. Only bounded, owned messages cross to the Matter thread.
class WebView2Receiver
{
public:
    enum class Mode : uint8_t
    {
        Receive,
        SelfTest,
        TestProvider,
    };
    WebView2Receiver();
    ~WebView2Receiver();
    WebView2Receiver(const WebView2Receiver &)             = delete;
    WebView2Receiver & operator=(const WebView2Receiver &) = delete;
    CHIP_ERROR Start(Mode mode = Mode::Receive);
    CHIP_ERROR Send(Json::Value message);
    std::optional<Json::Value> TakeEvent();
    void RequestStop();
    CHIP_ERROR Join();

private:
    class Impl;
    std::shared_ptr<Impl> mImpl;
};

int RunMediaSelfTest();

} // namespace CameraController
