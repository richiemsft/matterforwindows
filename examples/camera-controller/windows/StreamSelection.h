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

#include <clusters/CameraAvStreamManagement/Commands.h>
#include <lib/support/CodeUtils.h>

namespace CameraController {

namespace Av = chip::app::Clusters::CameraAvStreamManagement;

inline CHIP_ERROR ValidateSelection(const Av::Commands::VideoStreamAllocate::Type & request)
{
    VerifyOrReturnError(request.streamUsage == chip::app::Clusters::Globals::StreamUsageEnum::kLiveView &&
                            request.videoCodec == Av::VideoCodecEnum::kH264,
                        CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
    VerifyOrReturnError(request.minResolution.width != 0 && request.minResolution.height != 0 &&
                            request.maxResolution.width >= request.minResolution.width &&
                            request.maxResolution.height >= request.minResolution.height && request.minFrameRate != 0 &&
                            request.maxFrameRate >= request.minFrameRate && request.minBitRate != 0 &&
                            request.maxBitRate >= request.minBitRate && request.keyFrameInterval != 0,
                        CHIP_ERROR_INVALID_ARGUMENT);
    return CHIP_NO_ERROR;
}

inline CHIP_ERROR ValidateSensor(const Av::Commands::VideoStreamAllocate::Type & request,
                                 const Av::Structs::VideoSensorParamsStruct::Type & sensor,
                                 const Av::Structs::VideoResolutionStruct::Type & viewport)
{
    ReturnErrorOnFailure(ValidateSelection(request));
    VerifyOrReturnError(request.maxResolution.width <= sensor.sensorWidth && request.maxResolution.height <= sensor.sensorHeight &&
                            request.maxFrameRate <= sensor.maxFPS && request.minResolution.width >= viewport.width &&
                            request.minResolution.height >= viewport.height,
                        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    return CHIP_NO_ERROR;
}

inline CHIP_ERROR ValidateAllocatedStream(const Av::Commands::VideoStreamAllocate::Type & request,
                                          const Av::Structs::VideoStreamStruct::Type & stream)
{
    ReturnErrorOnFailure(ValidateSelection(request));
    VerifyOrReturnError(stream.streamUsage == request.streamUsage && stream.videoCodec == request.videoCodec &&
                            stream.minFrameRate >= request.minFrameRate && stream.maxFrameRate <= request.maxFrameRate &&
                            stream.minFrameRate <= stream.maxFrameRate &&
                            stream.minResolution.width >= request.minResolution.width &&
                            stream.minResolution.height >= request.minResolution.height &&
                            stream.maxResolution.width <= request.maxResolution.width &&
                            stream.maxResolution.height <= request.maxResolution.height &&
                            stream.minResolution.width <= stream.maxResolution.width &&
                            stream.minResolution.height <= stream.maxResolution.height && stream.minBitRate >= request.minBitRate &&
                            stream.maxBitRate <= request.maxBitRate && stream.minBitRate <= stream.maxBitRate &&
                            stream.keyFrameInterval == request.keyFrameInterval,
                        CHIP_IM_GLOBAL_STATUS(ConstraintError));
    return CHIP_NO_ERROR;
}

} // namespace CameraController
