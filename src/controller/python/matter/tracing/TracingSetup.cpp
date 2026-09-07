/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
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

#include <controller/python/matter/native/ChipMainLoopWork.h>
#include <controller/python/matter/native/PyChipError.h>

#include <tracing/json/json_tracing.h>
#if CHIP_PYTHON_ENABLE_PERFETTO
#include <tracing/perfetto/event_storage.h>
#include <tracing/perfetto/file_output.h>
#include <tracing/perfetto/perfetto_tracing.h>
#include <tracing/perfetto/simple_initialize.h>
#endif
#include <tracing/registry.h>

namespace {
chip::Tracing::Json::JsonBackend gJsonBackend;

#if CHIP_PYTHON_ENABLE_PERFETTO
chip::Tracing::Perfetto::FileTraceOutput gPerfettoFileOutput;
chip::Tracing::Perfetto::PerfettoBackend gPerfettoBackend;
#endif

} // namespace

extern "C" void pychip_tracing_start_json_log()
{
    chip::MainLoopWork::ExecuteInMainLoop([] {
        gJsonBackend.CloseFile(); // just in case, ensure no file output
        chip::Tracing::Register(gJsonBackend);
    });
}

extern "C" PyChipError pychip_tracing_start_json_file(const char * file_name)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    chip::MainLoopWork::ExecuteInMainLoop([&err, file_name] {
        err = gJsonBackend.OpenFile(file_name);
        if (err != CHIP_NO_ERROR)
        {
            return;
        }
        chip::Tracing::Register(gJsonBackend);
    });

    return ToPyChipError(err);
}

extern "C" void pychip_tracing_start_perfetto_system()
{
#if CHIP_PYTHON_ENABLE_PERFETTO
    chip::MainLoopWork::ExecuteInMainLoop([] {
        chip::Tracing::Perfetto::Initialize(perfetto::kSystemBackend);
        chip::Tracing::Perfetto::RegisterEventTrackingStorage();
        chip::Tracing::Register(gPerfettoBackend);
    });
#endif
}

extern "C" PyChipError pychip_tracing_start_perfetto_file(const char * file_name)
{
#if CHIP_PYTHON_ENABLE_PERFETTO
    CHIP_ERROR err = CHIP_NO_ERROR;
    chip::MainLoopWork::ExecuteInMainLoop([&err, file_name] {
        chip::Tracing::Perfetto::Initialize(perfetto::kInProcessBackend);
        chip::Tracing::Perfetto::RegisterEventTrackingStorage();

        err = gPerfettoFileOutput.Open(file_name);
        if (err != CHIP_NO_ERROR)
        {
            return;
        }
        chip::Tracing::Register(gPerfettoBackend);
    });

    return ToPyChipError(err);
#else
    static_cast<void>(file_name);
    return ToPyChipError(CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
#endif
}

extern "C" void pychip_tracing_stop()
{
    chip::MainLoopWork::ExecuteInMainLoop([] {
#if CHIP_PYTHON_ENABLE_PERFETTO
        chip::Tracing::Perfetto::FlushEventTrackingStorage();
        gPerfettoFileOutput.Close();
        chip::Tracing::Unregister(gPerfettoBackend);
#endif
        chip::Tracing::Unregister(gJsonBackend);
    });
}
