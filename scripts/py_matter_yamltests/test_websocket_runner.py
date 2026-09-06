#
#    Copyright (c) 2026 Project CHIP Authors
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

import sys
import unittest

from matter.yamltests.websocket_runner import WebSocketRunner, WebSocketRunnerConfig


class TestWebSocketRunner(unittest.IsolatedAsyncioTestCase):
    async def test_server_readiness_from_subprocess_pipe(self):
        runner = WebSocketRunner(WebSocketRunnerConfig())
        command = [
            sys.executable,
            '-u',
            '-c',
            'import time; print("== WebSocket Server Ready"); time.sleep(30)',
        ]

        server = await runner._start_server(command, 'ws://localhost:9002')
        try:
            self.assertIsNotNone(server)
            self.assertIsNone(server.poll())
        finally:
            await runner._stop_server(server)


if __name__ == '__main__':
    unittest.main()
