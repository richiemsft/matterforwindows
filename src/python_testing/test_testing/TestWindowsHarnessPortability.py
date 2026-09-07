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

CHIP_ROOT = next(path for path in Path(__file__).parents if (path / "SPECIFICATION_VERSION").is_file())
sys.path.insert(0, str(CHIP_ROOT / "scripts" / "tests"))

from matter.native import Library  # noqa: E402
from matter.testing.runner import convert_args_to_matter_config, matter_test_args_parser  # noqa: E402
from matter.tracing import StartTracingTo, TraceType  # noqa: E402
from run_python_test import (  # noqa: E402
    FactoryResetType,
    factory_reset_config_removal,
    normalize_windows_path_options,
    path_option_values,
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


if __name__ == "__main__":
    unittest.main()
