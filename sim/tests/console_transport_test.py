#!/usr/bin/env python3
"""Exercise modem_console against the simulator's abstract socket protocol."""

import os
from pathlib import Path
import socket
import subprocess
import sys
import unittest


CONSOLE = Path(sys.argv.pop(1)).resolve()


class ConsoleTransportTest(unittest.TestCase):
    def test_raw_sim_query_reaches_the_full_socket_name(self):
        names = (
            "modem_simulator_console",
            f"lutm_console_test_{os.getpid()}",
            "m" * 107,
        )
        for name in names:
            with self.subTest(socket_name=name), socket.socket(
                socket.AF_UNIX, socket.SOCK_STREAM
            ) as server:
                server.bind("\0" + name)
                server.listen(1)
                server.settimeout(2)
                command = [str(CONSOLE)]
                if name != "modem_simulator_console":
                    command.extend(["--socket", name])
                process = subprocess.Popen(
                    command + ["raw", "AT+CPIN?"],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                try:
                    try:
                        connection, _ = server.accept()
                    except socket.timeout:
                        stdout, stderr = process.communicate(timeout=5)
                        self.fail(f"Console did not connect: {stdout}{stderr}")
                    with connection:
                        connection.settimeout(2)
                        received = b""
                        while not received.endswith(b"AT+CPIN?\r"):
                            chunk = connection.recv(4096)
                            self.assertTrue(chunk, "Console closed before the SIM query")
                            received += chunk
                        self.assertEqual(received, b"REM0\rAT+CPIN?\r")
                        connection.sendall(b"+CPIN: READY\rOK\r")
                        stdout, stderr = process.communicate(timeout=5)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertEqual(stdout.splitlines(), ["+CPIN: READY", "OK"])
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()


if __name__ == "__main__":
    unittest.main()
