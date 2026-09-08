#
#    Copyright (c) 2026 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

CHIP_ROOT = next(path for path in Path(__file__).parents if (path / "SPECIFICATION_VERSION").is_file())
sys.path.insert(0, str(CHIP_ROOT / "scripts" / "tests"))

from matter.native import Library  # noqa: E402
from matter.testing.runner import convert_args_to_matter_config, matter_test_args_parser  # noqa: E402
from matter.tracing import StartTracingTo, TraceType  # noqa: E402
from run_python_test import (  # noqa: E402
    FactoryResetType,
    factory_reset_config_removal,
    filter_runs_by_app,
    normalize_windows_path_options,
    path_option_values,
    remove_options,
    replace_option_value,
    use_direct_ip_commissioning,
    windows_named_pipe_path,
)


class TestWindowsHarnessPortability(unittest.TestCase):
    def test_native_library_uses_platform_suffix(self):
        expected_suffix = ".dll" if os.name == "nt" else ".so"

        self.assertTrue(Library.CONTROLLER.value.endswith(expected_suffix))

    @unittest.skipUnless(os.name == "nt", "Windows-specific tracing policy")
    def test_perfetto_is_explicitly_unsupported(self):
        with self.assertRaisesRegex(ValueError, "not supported"):
            StartTracingTo(TraceType.PERFETTO)

    def test_path_option_values_accepts_separate_and_equals_forms(self):
        arguments = "--KVS 'state directory' --storage-directory=storage"

        self.assertEqual(
            list(path_option_values(arguments, {"--KVS", "--storage-directory"})),
            ["state directory", "storage"],
        )

    def test_windows_path_options_are_absolute_and_use_forward_slashes(self):
        original_directory = Path.cwd()
        with tempfile.TemporaryDirectory() as temporary_directory:
            try:
                os.chdir(temporary_directory)
                normalized = normalize_windows_path_options(
                    "--KVS 'state directory' --storage-directory=storage",
                    {"--KVS", "--storage-directory"},
                )
                values = list(path_option_values(normalized, {"--KVS", "--storage-directory"}))

                self.assertEqual(
                    values,
                    [
                        (Path(temporary_directory) / "state directory").as_posix(),
                        (Path(temporary_directory) / "storage").as_posix(),
                    ],
                )
            finally:
                os.chdir(original_directory)

    def test_factory_reset_removes_directory_backed_kvs(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            kvs_path = Path(temporary_directory) / "windows-kvs"
            kvs_path.mkdir()
            (kvs_path / "value").write_text("test", encoding="utf-8")

            factory_reset_config_removal(
                f"--KVS '{kvs_path.as_posix()}'",
                "",
                FactoryResetType.AppOnly,
            )

            self.assertFalse(kvs_path.exists())

    def test_direct_ip_commissioning_arguments(self):
        arguments = matter_test_args_parser().parse_args(
            [
                "--in-test-commissioning-method", "on-network-ip",
                "--ip-addr", "127.0.0.1",
                "--discriminator", "1234",
                "--passcode", "20202021",
            ]
        )
        config = convert_args_to_matter_config(arguments)

        self.assertEqual(config.in_test_commissioning_method, "on-network-ip")
        self.assertEqual(config.commissionee_ip_address_just_for_testing, "127.0.0.1")

    def test_direct_ip_rewrites_both_commissioning_options(self):
        rewritten = use_direct_ip_commissioning(
            "--commissioning-method on-network --in-test-commissioning-method=on-network --discriminator 1234",
            "127.0.0.1",
        )

        self.assertIn("--commissioning-method on-network-ip", rewritten)
        self.assertIn("--in-test-commissioning-method=on-network-ip", rewritten)
        self.assertIn("--ip-addr 127.0.0.1", rewritten)

    def test_direct_ip_does_not_override_existing_ip(self):
        rewritten = use_direct_ip_commissioning(
            "--commissioning-method on-network --ip-addr 192.0.2.1",
            "127.0.0.1",
        )

        self.assertEqual(rewritten.count("--ip-addr"), 1)
        self.assertIn("192.0.2.1", rewritten)

    def test_direct_ip_adds_method_for_setup_code(self):
        rewritten = use_direct_ip_commissioning(
            "--manual-code 10054912339 --storage-path admin_storage.json",
            "127.0.0.1",
        )

        self.assertIn("--manual-code 10054912339", rewritten)
        self.assertIn("--commissioning-method on-network-ip", rewritten)
        self.assertIn("--ip-addr 127.0.0.1", rewritten)

    def test_windows_pipe_name_matches_native_transport(self):
        self.assertEqual(windows_named_pipe_path("/tmp/test pipe"), r"\\.\pipe\matter-_tmp_test_pipe")
        self.assertEqual(
            replace_option_value("--app-pipe /tmp/test --endpoint 1", "--app-pipe", r"\\.\pipe\matter-_tmp_test"),
            r"--app-pipe '\\.\pipe\matter-_tmp_test' --endpoint 1",
        )

    def test_windows_removes_unsupported_trace_options(self):
        self.assertEqual(
            remove_options("--trace-to json:app.json --endpoint 1 --trace-to=perfetto:test.perfetto", {"--trace-to"}),
            "--endpoint 1",
        )

    def test_app_filter_uses_expanded_metadata_paths(self):
        runs = [
            SimpleNamespace(app="out/all-devices.exe"),
            SimpleNamespace(app="out/all-clusters.exe"),
        ]

        filtered = filter_runs_by_app(
            runs,
            "ALL_DEVICES_APP",
            {
                "ALL_DEVICES_APP": "out/all-devices.exe",
                "ALL_CLUSTERS_APP": "out/all-clusters.exe",
            },
        )

        self.assertEqual(filtered, [runs[0]])


if __name__ == "__main__":
    unittest.main()
