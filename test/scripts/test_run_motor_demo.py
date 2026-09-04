#!/usr/bin/env python3
"""Black-box tests for the supervised motor-demo runner.

The tests provide a pseudo-terminal that behaves like the Wheel Wrecker
firmware.  Passing its slave path explicitly guarantees that no discovered
Arduino or other physical serial device can be opened by the test suite.
"""

from __future__ import annotations

import errno
import importlib.util
import io
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import threading
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest.mock import patch


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPOSITORY_ROOT / "scripts" / "run_motor_demo.py"

RUNNER_SPEC = importlib.util.spec_from_file_location("run_motor_demo", RUNNER)
if RUNNER_SPEC is None or RUNNER_SPEC.loader is None:
    raise RuntimeError(f"could not load demo runner from {RUNNER}")
run_motor_demo = importlib.util.module_from_spec(RUNNER_SPEC)
RUNNER_SPEC.loader.exec_module(run_motor_demo)


class FakeFirmware:
    """Minimal line-oriented implementation of the runner's serial contract."""

    def __init__(
        self,
        *,
        status_line: str = (
            "state=IDLE armed=NO driver=OFF REQUESTED position=UNKNOWN"
        ),
        acknowledge_stop: bool = True,
        acknowledge_disarm: bool = True,
        help_supports_demo: bool = True,
        complete_demo: bool = True,
        demo_error: str | None = None,
        arm_response: str | None = (
            "ARMED: motion commands are enabled; STOP/Ctrl-C aborts"
        ),
        demo_start_count: int | None = None,
        demo_done_count: int | None = None,
        stale_done_during_setpos: int | None = None,
        stop_before_done: bool = False,
        emit_demo_move: bool = True,
    ) -> None:
        self.master_fd, self.slave_fd = pty.openpty()
        self.port = os.ttyname(self.slave_fd)
        self.status_line = status_line
        self.acknowledge_stop = acknowledge_stop
        self.acknowledge_disarm = acknowledge_disarm
        self.help_supports_demo = help_supports_demo
        self.complete_demo = complete_demo
        self.demo_error = demo_error
        self.arm_response = arm_response
        self.demo_start_count = demo_start_count
        self.demo_done_count = demo_done_count
        self.stale_done_during_setpos = stale_done_during_setpos
        self.stop_before_done = stop_before_done
        self.emit_demo_move = emit_demo_move
        self.commands: list[str] = []
        self.error: BaseException | None = None
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self) -> "FakeFirmware":
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self._stop.set()
        self._thread.join(timeout=2)
        os.close(self.slave_fd)
        os.close(self.master_fd)
        if exc_type is None and self.error is not None:
            raise self.error

    def wait_for(self, command: str, timeout: float = 12) -> bool:
        deadline = time.monotonic() + timeout
        with self._condition:
            while command not in self.commands:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._condition.wait(remaining)
            return True

    def _write(self, payload: str) -> None:
        data = payload.encode("ascii")
        while data and not self._stop.is_set():
            written = os.write(self.master_fd, data)
            data = data[written:]

    def _reply(self, command: str) -> None:
        if command == "STOP":
            if self.acknowledge_stop:
                self._write(
                    "STOPPED: operator request\r\n"
                    "Position is unknown. Realign the dial and use SETPOS before moving.\r\n"
                    "> "
                )
        elif command == "STATUS":
            self._write(
                self.status_line
                + "\r\nSPR=4000 resolution=0.025000 mark/step speed=0.250 rev/s\r\n> "
            )
        elif command == "HELP":
            demo_help = (
                "  DEMO <L|R> <count>  run 1..64 coarse 3-wheel candidates\r\n"
                if self.help_supports_demo
                else ""
            )
            self._write("Commands (newline terminated):\r\n" + demo_help + "> ")
        elif command.startswith("SETPOS "):
            mark = command.split(maxsplit=1)[1]
            self._write(f"OK: dial reference={float(mark):.4f} (tick 0)\r\n> ")
            if self.stale_done_during_setpos is not None:
                self._write(
                    f"DONE DEMO count={self.stale_done_during_setpos}; "
                    "no open detection was performed\r\n"
                )
        elif command == "ARM":
            if self.arm_response is not None:
                self._write(self.arm_response + "\r\n> ")
        elif command.startswith("DEMO "):
            _, direction, count = command.split()
            start_count = self.demo_start_count or int(count)
            move_direction = "LEFT" if direction == "L" else "RIGHT"
            self._write(
                f"DEMO START count={start_count} grid=0/25/50/75; STOP/Ctrl-C aborts\r\n"
                f"CANDIDATE 1/{start_count}  0-0-0  (commanded only; no open detection)\r\n"
            )
            if self.emit_demo_move:
                self._write(
                    f"MOVE {move_direction}  steps=16000  target=0.0000  passes=3\r\n"
                )
            if self.demo_error is not None:
                # Let the start acknowledgement reach the runner before the
                # asynchronous firmware error that follows it.
                time.sleep(0.05)
                self._write(f"ERR: {self.demo_error}\r\n> ")
            elif self.complete_demo:
                if self.stop_before_done:
                    self._write("STOPPED: operator request\r\n")
                # Deliberately split the completion marker. A serial reader must
                # not rely on one read call returning a complete line or token.
                self._write("DONE DE")
                time.sleep(0.01)
                done_count = self.demo_done_count or int(count)
                self._write(
                    f"MO count={done_count}; no open detection was performed\r\n> "
                )
        elif command == "DISARM":
            if self.acknowledge_disarm:
                self._write(
                    "DISARMED: driver offline requested; position is unknown\r\n> "
                )

    def _serve(self) -> None:
        pending = bytearray()
        try:
            while not self._stop.is_set():
                readable, _, _ = select.select([self.master_fd], [], [], 0.05)
                if not readable:
                    continue
                try:
                    chunk = os.read(self.master_fd, 4096)
                except OSError as error:
                    # Linux PTY masters commonly report EIO when the final
                    # slave descriptor closes; macOS commonly returns EOF.
                    if error.errno == errno.EIO:
                        continue
                    raise
                if not chunk:
                    continue
                pending.extend(chunk)
                while b"\n" in pending:
                    raw_line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    command = raw_line.rstrip(b"\r").decode("ascii")
                    with self._condition:
                        self.commands.append(command)
                        self._condition.notify_all()
                    self._reply(command)
        except BaseException as error:  # surfaced by __exit__ in the test thread
            self.error = error
            with self._condition:
                self._condition.notify_all()


class MotorDemoConfigurationTests(unittest.TestCase):
    def parse_args(self, *arguments: str):
        return run_motor_demo.build_argument_parser().parse_args(arguments)

    def assert_arguments_rejected(self, *arguments: str) -> None:
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                self.parse_args(*arguments)
        self.assertEqual(raised.exception.code, 2)

    def test_uno_r4_matching_ignores_unknown_and_other_boards(self) -> None:
        board_list = {
            "detected_ports": [
                {
                    "port": {
                        "address": "/dev/cu.Bluetooth-Incoming-Port",
                        "protocol": "serial",
                    }
                },
                {
                    "matching_boards": [
                        {"name": "Arduino Uno", "fqbn": "arduino:avr:uno"}
                    ],
                    "port": {"address": "/dev/cu.usbserial-other"},
                },
                {
                    "matching_boards": [
                        {
                            "name": "Arduino UNO R4 WiFi",
                            "fqbn": "arduino:renesas_uno:unor4wifi",
                        }
                    ],
                    "port": {"address": "/dev/cu.usbmodem-wheel-wrecker"},
                },
            ]
        }

        self.assertEqual(
            run_motor_demo.uno_r4_ports(board_list),
            ["/dev/cu.usbmodem-wheel-wrecker"],
        )

    def test_uno_r4_matching_deduplicates_the_same_port(self) -> None:
        matching_board = {
            "name": "Arduino UNO R4 WiFi",
            "fqbn": "arduino:renesas_uno:unor4wifi",
        }
        board_list = {
            "detected_ports": [
                {
                    "matching_boards": [matching_board],
                    "port": {"address": "/dev/cu.usbmodem-first"},
                },
                {
                    "matching_boards": [matching_board],
                    "port": {"address": "/dev/cu.usbmodem-first"},
                },
                {
                    "matching_boards": [matching_board],
                    "port": {"address": "/dev/cu.usbmodem-second"},
                },
            ]
        }

        self.assertEqual(
            run_motor_demo.uno_r4_ports(board_list),
            ["/dev/cu.usbmodem-first", "/dev/cu.usbmodem-second"],
        )

    def test_cli_accepts_count_boundaries_and_normalizes_direction(self) -> None:
        first = self.parse_args("--direction", "l", "--count", "1")
        last = self.parse_args("--direction", "r", "--count", "64")

        self.assertEqual((first.direction, first.count), ("L", 1))
        self.assertEqual((last.direction, last.count), ("R", 64))

    def test_cli_rejects_values_outside_command_boundaries(self) -> None:
        for count in ("0", "65", "1.5", "many"):
            with self.subTest(count=count):
                self.assert_arguments_rejected("--count", count)
        for position in ("-0.001", "100", "NaN", "Infinity"):
            with self.subTest(position=position):
                self.assert_arguments_rejected("--position", position)
        self.assert_arguments_rejected("--direction", "clockwise")

    def test_position_normalization_preserves_integer_trailing_zeroes(self) -> None:
        cases = {
            "0": "0",
            "0.5000": "0.5",
            "12.5000": "12.5",
            "50": "50",
            "50.000": "50",
            "90": "90",
            "99.9900": "99.99",
        }
        for supplied, expected in cases.items():
            with self.subTest(supplied=supplied):
                args = self.parse_args("--position", supplied)
                self.assertEqual(args.position, expected)


@unittest.skipUnless(os.name == "posix", "the runner uses POSIX termios")
class MotorDemoRunnerTests(unittest.TestCase):
    maxDiff = None

    def launch(self, port: str, *arguments: str) -> subprocess.Popen[str]:
        environment = os.environ.copy()
        environment["PYTHONUNBUFFERED"] = "1"
        return subprocess.Popen(
            [
                sys.executable,
                str(RUNNER),
                "--port",
                port,
                *arguments,
            ],
            cwd=REPOSITORY_ROOT,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )

    def finish(
        self, process: subprocess.Popen[str], timeout: float = 15
    ) -> tuple[int, str]:
        try:
            return_code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)
            output = process.stdout.read() if process.stdout is not None else ""
            if process.stdout is not None:
                process.stdout.close()
            self.fail(f"runner did not exit; output was:\n{output}")
        finally:
            if process.stdin is not None:
                process.stdin.close()
        output = process.stdout.read() if process.stdout is not None else ""
        if process.stdout is not None:
            process.stdout.close()
        return return_code, output

    def assert_ordered(self, actual: list[str], expected: list[str]) -> None:
        next_index = 0
        for command in actual:
            if next_index < len(expected) and command == expected[next_index]:
                next_index += 1
        self.assertEqual(
            next_index,
            len(expected),
            f"expected ordered protocol {expected!r}, got {actual!r}",
        )

    def test_dry_run_never_opens_the_supplied_port(self) -> None:
        nonexistent_port = "/wheel-wrecker-test/this-port-must-not-be-opened"
        result = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--port",
                nonexistent_port,
                "--direction",
                "R",
                "--count",
                "3",
                "--position",
                "12.5",
                "--dry-run",
            ],
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SETPOS 12.5", result.stdout)
        self.assertIn("DEMO R 3", result.stdout)
        self.assertIn("RUN R 3", result.stdout)
        self.assertIn("No serial port was opened", result.stdout)

    def test_normal_demo_preflights_then_disarms_after_completion(self) -> None:
        with FakeFirmware() as firmware:
            process = self.launch(
                firmware.port,
                "--direction",
                "R",
                "--count",
                "2",
                "--position",
                "12.5",
            )
            assert process.stdin is not None
            process.stdin.write("RUN R 2\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 0, output)
            self.assert_ordered(
                firmware.commands,
                [
                    "STOP",
                    "STATUS",
                    "HELP",
                    "SETPOS 12.5",
                    "ARM",
                    "DEMO R 2",
                    "DISARM",
                ],
            )

    def test_disarmed_line_cannot_acknowledge_arm(self) -> None:
        with FakeFirmware(
            arm_response="DISARMED: driver offline requested; position is unknown"
        ) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process, timeout=8)

            self.assertEqual(return_code, 1, output)
            self.assertIn("ARM", firmware.commands)
            self.assertNotIn("DEMO L 4", firmware.commands)
            self.assertIn("STOP", firmware.commands[firmware.commands.index("ARM") + 1 :])

    def test_demo_count_prefixes_do_not_match(self) -> None:
        cases = (
            ({"demo_start_count": 10}, "start"),
            ({"demo_done_count": 10}, "completion"),
        )
        for firmware_options, phase in cases:
            with self.subTest(phase=phase), FakeFirmware(**firmware_options) as firmware:
                process = self.launch(firmware.port, "--count", "1")
                assert process.stdin is not None
                process.stdin.write("RUN L 1\n")
                process.stdin.flush()

                return_code, output = self.finish(process)

                self.assertEqual(return_code, 1, output)
                self.assertNotIn("DISARM", firmware.commands)
                demo_index = firmware.commands.index("DEMO L 1")
                self.assertIn("STOP", firmware.commands[demo_index + 1 :])

    def test_stale_done_before_current_demo_is_ignored(self) -> None:
        with FakeFirmware(
            stale_done_during_setpos=4, complete_demo=False
        ) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            self.assertTrue(firmware.wait_for("DEMO L 4"))
            time.sleep(0.1)
            self.assertIsNone(process.poll())
            self.assertNotIn("DISARM", firmware.commands)

            os.killpg(process.pid, signal.SIGINT)
            return_code, output = self.finish(process)
            self.assertEqual(return_code, 130, output)

    def test_completion_requires_an_ordered_move_line(self) -> None:
        with FakeFirmware(emit_demo_move=False) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 1, output)
            self.assertNotIn("DISARM", firmware.commands)
            self.assertIn("expected first DEMO move", output)

    def test_stopped_before_done_wins_in_wire_order(self) -> None:
        with FakeFirmware(stop_before_done=True) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 130, output)
            self.assertNotIn("DISARM", firmware.commands)

    def test_status_safety_fields_must_share_one_line(self) -> None:
        status = (
            "state=IDLE armed=YES driver=ON position=KNOWN\r\n"
            "state=MOVING armed=NO driver=OFF REQUESTED position=UNKNOWN"
        )
        with FakeFirmware(status_line=status) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 1, output)
            self.assertNotIn("ARM", firmware.commands)

    def test_incomplete_line_is_not_an_acknowledgement(self) -> None:
        with FakeFirmware(arm_response=None) as firmware:
            with run_motor_demo.PosixSerial(firmware.port) as serial:
                cursor = serial.send_line("ARM")
                self.assertTrue(firmware.wait_for("ARM"))
                firmware._write(run_motor_demo.ARM_ACK_LINE.decode("ascii"))
                with self.assertRaises(run_motor_demo.DemoRunnerError):
                    serial.wait_for_any(
                        (run_motor_demo.ARM_ACK_LINE,), cursor, timeout=0.05
                    )

    def test_stale_stop_and_disarm_lines_cannot_ack_cleanup(self) -> None:
        with FakeFirmware(
            acknowledge_stop=False, acknowledge_disarm=False
        ) as firmware:
            with run_motor_demo.PosixSerial(firmware.port) as serial:
                firmware._write("STOPPED: operator request\r\n")
                stderr = io.StringIO()
                with (
                    patch.object(run_motor_demo, "STOP_TIMEOUT_SECONDS", 0.05),
                    redirect_stderr(stderr),
                ):
                    self.assertFalse(
                        run_motor_demo.force_stop(serial, urgent_on_failure=False)
                    )

                firmware._write(
                    "DISARMED: driver offline requested; position is unknown\r\n"
                )
                with (
                    patch.object(run_motor_demo, "ACK_TIMEOUT_SECONDS", 0.05),
                    redirect_stdout(io.StringIO()),
                    self.assertRaises(run_motor_demo.DemoRunnerError),
                ):
                    run_motor_demo.disarm_after_completion(serial)

            self.assertEqual(firmware.commands, ["STOP", "STOP", "DISARM"])

    def test_ctrl_c_sends_text_stop_after_demo_has_started(self) -> None:
        with FakeFirmware(complete_demo=False) as firmware:
            process = self.launch(
                firmware.port,
                "--direction",
                "L",
                "--count",
                "4",
                "--position",
                "0",
            )
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            self.assertTrue(
                firmware.wait_for("DEMO L 4"),
                f"demo did not start; commands were {firmware.commands!r}",
            )
            os.killpg(process.pid, signal.SIGINT)
            return_code, output = self.finish(process)

            self.assertEqual(return_code, 130, output)
            demo_index = firmware.commands.index("DEMO L 4")
            self.assertIn("STOP", firmware.commands[demo_index + 1 :])

    def test_stdin_eof_sends_text_stop_after_demo_has_started(self) -> None:
        with FakeFirmware(complete_demo=False) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            self.assertTrue(
                firmware.wait_for("DEMO L 4"),
                f"demo did not start; commands were {firmware.commands!r}",
            )
            process.stdin.close()
            return_code, output = self.finish(process)

            self.assertEqual(return_code, 130, output)
            demo_index = firmware.commands.index("DEMO L 4")
            self.assertIn("STOP", firmware.commands[demo_index + 1 :])

    def test_unacknowledged_preflight_stop_warns_and_never_arms(self) -> None:
        with FakeFirmware(acknowledge_stop=False) as firmware:
            args = run_motor_demo.build_argument_parser().parse_args(
                ["--port", firmware.port]
            )
            stdout = io.StringIO()
            stderr = io.StringIO()
            with (
                patch.object(run_motor_demo, "STOP_TIMEOUT_SECONDS", 0.05),
                redirect_stdout(stdout),
                redirect_stderr(stderr),
            ):
                return_code = run_motor_demo.run_live(args, firmware.port)

            self.assertEqual(return_code, 1, stdout.getvalue() + stderr.getvalue())
            self.assertEqual(firmware.commands, ["STOP", "STOP"])
            self.assertNotIn("ARM", firmware.commands)
            self.assertIn("CUT 24 V MOTOR POWER NOW", stderr.getvalue())

    def test_firmware_error_after_demo_start_stops_without_hanging(self) -> None:
        with FakeFirmware(
            complete_demo=False, demo_error="simulated motion fault"
        ) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 1, output)
            demo_index = firmware.commands.index("DEMO L 4")
            self.assertIn("STOP", firmware.commands[demo_index + 1 :])
            self.assertIn("simulated motion fault", output)

    def test_unexpected_exception_still_attempts_last_resort_stop(self) -> None:
        with FakeFirmware() as firmware:
            args = run_motor_demo.build_argument_parser().parse_args(
                ["--port", firmware.port]
            )
            output = io.StringIO()
            with (
                patch.object(run_motor_demo, "read_confirmation", return_value=True),
                patch.object(
                    run_motor_demo,
                    "run_interactive",
                    side_effect=RuntimeError("unexpected test exception"),
                ),
                redirect_stdout(output),
            ):
                with self.assertRaisesRegex(
                    RuntimeError, "unexpected test exception"
                ):
                    run_motor_demo.run_live(args, firmware.port)

            help_index = firmware.commands.index("HELP")
            self.assertIn("STOP", firmware.commands[help_index + 1 :])

    def test_refuses_to_arm_when_status_is_not_known_safe(self) -> None:
        with FakeFirmware(
            status_line="state=IDLE armed=YES driver=ON position=UNKNOWN"
        ) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertNotEqual(return_code, 0, output)
            self.assertIn("STOP", firmware.commands)
            self.assertIn("STATUS", firmware.commands)
            self.assertNotIn("ARM", firmware.commands)
            self.assertNotIn("DEMO L 4", firmware.commands)

    def test_refuses_firmware_without_demo_command(self) -> None:
        with FakeFirmware(help_supports_demo=False) as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("RUN L 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertNotEqual(return_code, 0, output)
            self.assert_ordered(firmware.commands, ["STOP", "STATUS", "HELP"])
            self.assertNotIn("ARM", firmware.commands)
            self.assertNotIn("DEMO L 4", firmware.commands)

    def test_confirmation_must_match_exactly_before_arming(self) -> None:
        with FakeFirmware() as firmware:
            process = self.launch(firmware.port)
            assert process.stdin is not None
            process.stdin.write("run l 4\n")
            process.stdin.flush()

            return_code, output = self.finish(process)

            self.assertEqual(return_code, 0, output)
            self.assert_ordered(firmware.commands, ["STOP", "STATUS", "HELP"])
            self.assertNotIn("SETPOS 0", firmware.commands)
            self.assertNotIn("ARM", firmware.commands)
            self.assertNotIn("DEMO L 4", firmware.commands)


if __name__ == "__main__":
    unittest.main()
