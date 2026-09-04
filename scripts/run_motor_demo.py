#!/usr/bin/env python3
"""Run the bounded Wheel Wrecker motor demonstration safely.

The script uses Arduino CLI only to discover an UNO R4 WiFi.  Serial traffic
uses the Python standard library so Ctrl-C can be translated into the
firmware's textual STOP command instead of merely closing a serial monitor.
"""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import termios
import time
import tty
from decimal import Decimal, InvalidOperation
from typing import Iterable, Optional, Pattern, Sequence, Union


EXPECTED_FQBN = "arduino:renesas_uno:unor4wifi"
BAUD_RATE = 115200
DEFAULT_DIRECTION = "L"
DEFAULT_COUNT = 4
DEFAULT_POSITION = "0"
ACK_TIMEOUT_SECONDS = 3.0
STOP_ATTEMPTS = 2
STOP_TIMEOUT_SECONDS = 2.0

STOP_ACK_LINE = b"STOPPED: operator request"
DISARM_ACK_LINES = (
    b"DISARMED: driver offline requested; position is unknown",
    b"DISARMED: pulses stopped, but ENA is uncontrolled; position is unknown",
    b"STOPPED: operator disarm",
)
ARM_ACK_LINE = b"ARMED: motion commands are enabled; STOP/Ctrl-C aborts"
HELP_DEMO_LINE = b"  DEMO <L|R> <count>  run 1..64 coarse 3-wheel candidates"
STATUS_LINE = re.compile(rb"state=.*")
SAFE_STATUS_LINE = re.compile(
    rb"state=IDLE armed=NO driver=(?:OFF REQUESTED|N/C \(UNCONTROLLED\)) "
    rb"position=UNKNOWN"
)
SETPOS_ACK_LINE = re.compile(
    rb"OK: dial reference=[0-9]+\.[0-9]{4} \(tick [0-9]+\)"
)

LineMatcher = Union[bytes, Pattern[bytes]]


class DemoRunnerError(Exception):
    """An expected, user-facing runner failure."""


class SerialConnectionLost(DemoRunnerError):
    """The serial link closed or failed."""


class AbortRequested(Exception):
    """A process signal requested a safe stop."""

    def __init__(self, signum: int):
        super().__init__(signum)
        self.signum = signum


def safe_stderr(message: str) -> None:
    """Report a safety message without letting a closed terminal skip cleanup."""

    try:
        print(message, file=sys.stderr, flush=True)
    except (BrokenPipeError, OSError):
        pass


def parse_direction(value: str) -> str:
    direction = value.upper()
    if direction not in ("L", "R"):
        raise argparse.ArgumentTypeError("direction must be L or R")
    return direction


def parse_count(value: str) -> int:
    try:
        count = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("count must be an integer from 1 to 64") from error
    if not 1 <= count <= 64:
        raise argparse.ArgumentTypeError("count must be from 1 to 64")
    return count


def parse_position(value: str) -> str:
    try:
        position = Decimal(value)
    except InvalidOperation as error:
        raise argparse.ArgumentTypeError(
            "position must be a dial mark from 0 through less than 100"
        ) from error
    if not position.is_finite() or position < 0 or position >= 100:
        raise argparse.ArgumentTypeError(
            "position must be a dial mark from 0 through less than 100"
        )

    if position == 0:
        return "0"
    normalized = format(position, "f")
    if "." in normalized:
        normalized = normalized.rstrip("0").rstrip(".")
    return normalized or "0"


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run the firmware's bounded, supervised three-wheel motor demo. "
            "The firmware must already be uploaded."
        ),
        epilog=(
            "Close Arduino IDE Serial Monitor before running this script. "
            "Keep the physical 24 V motor-power cutoff within reach."
        ),
    )
    parser.add_argument(
        "--port",
        metavar="PATH",
        help="serial port; otherwise auto-detect one Arduino UNO R4 WiFi",
    )
    parser.add_argument(
        "--direction",
        type=parse_direction,
        default=DEFAULT_DIRECTION,
        metavar="L|R",
        help="first combination direction (default: L)",
    )
    parser.add_argument(
        "--count",
        type=parse_count,
        default=DEFAULT_COUNT,
        metavar="1..64",
        help="number of coarse-grid candidates (default: 4)",
    )
    parser.add_argument(
        "--position",
        type=parse_position,
        default=DEFAULT_POSITION,
        metavar="MARK",
        help="physically aligned starting dial mark, 0 <= mark < 100 (default: 0)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="show and validate the plan without opening a serial port",
    )
    return parser


def find_arduino_cli() -> Optional[str]:
    configured = os.environ.get("ARDUINO_CLI")
    if configured:
        resolved = shutil.which(configured)
        if resolved:
            return resolved
        candidate = Path(configured).expanduser()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
        raise DemoRunnerError(
            f"ARDUINO_CLI does not name an executable: {configured}"
        )

    resolved = shutil.which("arduino-cli")
    if resolved:
        return resolved

    bundled = Path(
        "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/"
        "resources/arduino-cli"
    )
    if bundled.is_file() and os.access(bundled, os.X_OK):
        return str(bundled)
    return None


def uno_r4_ports(board_list: object) -> list[str]:
    if not isinstance(board_list, dict):
        raise DemoRunnerError("Arduino CLI returned an unexpected board-list document")

    matches: list[str] = []
    detected_ports = board_list.get("detected_ports", [])
    if not isinstance(detected_ports, list):
        raise DemoRunnerError("Arduino CLI returned an unexpected detected_ports value")

    for entry in detected_ports:
        if not isinstance(entry, dict):
            continue
        boards = entry.get("matching_boards", [])
        if not isinstance(boards, list) or not any(
            isinstance(board, dict) and board.get("fqbn") == EXPECTED_FQBN
            for board in boards
        ):
            continue
        port = entry.get("port", {})
        address = port.get("address") if isinstance(port, dict) else None
        if isinstance(address, str) and address and address not in matches:
            matches.append(address)
    return matches


def discover_port() -> str:
    cli = find_arduino_cli()
    if cli is None:
        raise DemoRunnerError(
            "arduino-cli was not found. Install it, set ARDUINO_CLI to its path, "
            "or pass --port explicitly."
        )

    try:
        result = subprocess.run(
            [
                cli,
                "board",
                "list",
                "--json",
                "--discovery-timeout",
                "3s",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DemoRunnerError(f"could not run Arduino CLI board discovery: {error}") from error

    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown error"
        raise DemoRunnerError(f"Arduino CLI board discovery failed: {detail}")
    try:
        board_list = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise DemoRunnerError("Arduino CLI returned invalid JSON during board discovery") from error

    ports = uno_r4_ports(board_list)
    if not ports:
        raise DemoRunnerError(
            "no Arduino UNO R4 WiFi was detected. Connect it, close Serial Monitor, "
            "or pass its serial device with --port."
        )
    if len(ports) > 1:
        listed = ", ".join(ports)
        raise DemoRunnerError(
            f"more than one Arduino UNO R4 WiFi was detected ({listed}); choose one with --port."
        )
    return ports[0]


class PosixSerial:
    """Small POSIX serial transport with complete-line firmware waits."""

    def __init__(self, port: str):
        self.port = port
        self.fd: Optional[int] = None
        self.received = bytearray()
        self.lines: list[bytes] = []
        self._partial_line = bytearray()

    def open(self) -> None:
        if os.name != "posix":
            raise DemoRunnerError("the demo runner currently supports macOS and Linux")
        try:
            self.fd = os.open(
                self.port,
                os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK,
            )
        except OSError as error:
            if error.errno in (errno.EBUSY, errno.EACCES):
                raise DemoRunnerError(
                    f"cannot open {self.port}: {error.strerror}. Close Arduino IDE "
                    "Serial Monitor and other serial programs."
                ) from error
            raise DemoRunnerError(f"cannot open {self.port}: {error}") from error

        try:
            exclusive = getattr(termios, "TIOCEXCL", None)
            if exclusive is not None:
                fcntl.ioctl(self.fd, exclusive)

            tty.setraw(self.fd, termios.TCSANOW)
            attributes = termios.tcgetattr(self.fd)
            attributes[2] &= ~(
                termios.PARENB | termios.CSTOPB | termios.CSIZE
            )
            attributes[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
            attributes[4] = termios.B115200
            attributes[5] = termios.B115200
            attributes[6][termios.VMIN] = 0
            attributes[6][termios.VTIME] = 1
            termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except (OSError, termios.error) as error:
            self.close()
            raise DemoRunnerError(
                f"could not configure {self.port} for {BAUD_RATE} baud: {error}"
            ) from error

    def close(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def __enter__(self) -> "PosixSerial":
        self.open()
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def _require_fd(self) -> int:
        if self.fd is None:
            raise SerialConnectionLost("serial port is not open")
        return self.fd

    def write(self, data: bytes, timeout: float = 1.0) -> None:
        fd = self._require_fd()
        view = memoryview(data)
        deadline = time.monotonic() + timeout
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise SerialConnectionLost("timed out writing to the controller")
            try:
                _, writable, _ = select.select([], [fd], [], remaining)
                if not writable:
                    raise SerialConnectionLost("timed out writing to the controller")
                written = os.write(fd, view)
                if written <= 0:
                    raise SerialConnectionLost("the serial connection closed while writing")
                view = view[written:]
            except OSError as error:
                if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EINTR):
                    continue
                raise SerialConnectionLost(f"serial write failed: {error}") from error

    def _discard_stale_input(self) -> int:
        """Discard bytes received before the next command transaction."""

        fd = self._require_fd()
        while self._read_ready(0.0):
            pass
        try:
            termios.tcflush(fd, termios.TCIFLUSH)
        except (OSError, termios.error) as error:
            raise SerialConnectionLost(
                f"could not discard stale serial input: {error}"
            ) from error
        # A fragment cannot be attributed to the command that has not yet
        # been sent. Do not let it join a later acknowledgement line.
        self._partial_line.clear()
        return len(self.lines)

    def send_line(self, command: str) -> int:
        cursor = self._discard_stale_input()
        self.write(command.encode("ascii") + b"\n")
        return cursor

    def write_line(self, command: str) -> None:
        """Write during an active transaction without discarding its output."""

        self.write(command.encode("ascii") + b"\n")

    def _record_complete_lines(self, data: bytes) -> None:
        self._partial_line.extend(data)
        while True:
            # The firmware prompt is deliberately not newline terminated. It
            # may therefore prefix the next response in the byte stream.
            while self._partial_line.startswith(b"> "):
                del self._partial_line[:2]

            newline = self._partial_line.find(b"\n")
            if newline < 0:
                return
            raw_line = bytes(self._partial_line[:newline])
            del self._partial_line[: newline + 1]
            if raw_line.endswith(b"\r"):
                raw_line = raw_line[:-1]
            while raw_line.startswith(b"> "):
                raw_line = raw_line[2:]
            if raw_line:
                self.lines.append(raw_line)

    def _read_ready(self, timeout: float) -> bool:
        fd = self._require_fd()
        try:
            readable, _, _ = select.select([fd], [], [], max(0.0, timeout))
        except (OSError, ValueError) as error:
            raise SerialConnectionLost(f"serial wait failed: {error}") from error
        if not readable:
            return False

        try:
            data = os.read(fd, 4096)
        except OSError as error:
            if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EINTR):
                return False
            raise SerialConnectionLost(f"serial read failed: {error}") from error
        if not data:
            raise SerialConnectionLost("the serial connection closed")

        self.received.extend(data)
        self._record_complete_lines(data)
        try:
            sys.stdout.write(
                data.decode("utf-8", errors="replace").replace("\r", "")
            )
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            # Serial cleanup must still run if the controlling terminal vanishes.
            pass
        return True

    def wait_for_any(
        self,
        matchers: Iterable[LineMatcher],
        cursor: int,
        timeout: float,
        *,
        reject_error: bool = True,
    ) -> bytes:
        wanted = tuple(matchers)
        if not wanted:
            raise ValueError("at least one firmware response line is required")
        deadline = time.monotonic() + timeout
        next_line = cursor
        while True:
            while next_line < len(self.lines):
                line = self.lines[next_line]
                next_line += 1
                if reject_error and line.startswith(b"ERR:"):
                    detail = line.decode("utf-8", errors="replace")
                    raise DemoRunnerError(
                        f"firmware rejected a command: {detail}"
                    )
                for matcher in wanted:
                    if isinstance(matcher, bytes):
                        matched = line == matcher
                    else:
                        matched = matcher.fullmatch(line) is not None
                    if matched:
                        return line

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                names = " or ".join(
                    (
                        matcher.decode("ascii", errors="replace")
                        if isinstance(matcher, bytes)
                        else matcher.pattern.decode("ascii", errors="replace")
                    )
                    for matcher in wanted
                )
                raise DemoRunnerError(f"timed out waiting for firmware response: {names}")
            self._read_ready(min(remaining, 0.25))

    def send_and_expect(
        self,
        command: str,
        matchers: Iterable[LineMatcher],
        timeout: Optional[float] = None,
        *,
        reject_error: bool = True,
    ) -> bytes:
        cursor = self.send_line(command)
        return self.wait_for_any(
            matchers,
            cursor,
            ACK_TIMEOUT_SECONDS if timeout is None else timeout,
            reject_error=reject_error,
        )


def force_stop(serial: PosixSerial, *, urgent_on_failure: bool) -> bool:
    last_error: Optional[Exception] = None
    for _attempt in range(STOP_ATTEMPTS):
        try:
            serial.send_and_expect(
                "STOP",
                (STOP_ACK_LINE,),
                STOP_TIMEOUT_SECONDS,
                reject_error=False,
            )
            return True
        except (DemoRunnerError, SerialConnectionLost) as error:
            last_error = error

    if urgent_on_failure:
        safe_stderr(
            "\n!!! STOP WAS NOT ACKNOWLEDGED — CUT 24 V MOTOR POWER NOW !!!"
        )
    if last_error is not None:
        safe_stderr(f"[runner] {last_error}")
    return False


def verify_firmware(serial: PosixSerial) -> None:
    status = serial.send_and_expect("STATUS", (STATUS_LINE,))
    if SAFE_STATUS_LINE.fullmatch(status) is None:
        raise DemoRunnerError(
            "the controller did not report IDLE, disarmed, and position UNKNOWN "
            "on one status line"
        )

    serial.send_and_expect("HELP", (HELP_DEMO_LINE,))


def print_plan(args: argparse.Namespace, port: str) -> None:
    print("Wheel Wrecker bounded motor demo")
    print(f"  Port:              {port}")
    print(f"  Starting position: {args.position} (physical dial alignment)")
    print(f"  First direction:   {args.direction}")
    print(f"  Candidates:        {args.count} of 64 coarse-grid candidates")
    print("  Opening detection: none")
    print()
    print("The live runner will:")
    print("  1. Send STOP and verify the expected idle, disarmed firmware.")
    print(f"  2. Declare SETPOS {args.position}; this does not home or measure the dial.")
    print(f"  3. Send ARM, then DEMO {args.direction} {args.count}.")
    print("  4. Send DISARM immediately after normal completion.")
    print("  5. Translate Ctrl-C, termination, or input closure into STOP.")


def confirmation_phrase(args: argparse.Namespace) -> str:
    return f"RUN {args.direction} {args.count}"


def read_confirmation(args: argparse.Namespace) -> bool:
    expected = confirmation_phrase(args)
    print("[runner] The controller is stopped, disarmed, and running DEMO-capable firmware.")
    print(
        f"[runner] Physically align dial mark {args.position} under the index now."
    )
    print("[runner] Keep the physical 24 V cutoff within reach.")
    try:
        entered = input(f"Type exactly '{expected}' to begin motion: ")
    except EOFError:
        return False
    return entered.strip() == expected


def disarm_after_completion(serial: PosixSerial) -> None:
    print("\n[runner] Demo completed; requesting driver-off now.")
    serial.send_and_expect(
        "DISARM",
        DISARM_ACK_LINES,
    )
    print("\n[runner] Controller disarmed. The open-loop position is now unknown.")


def run_interactive(serial: PosixSerial, args: argparse.Namespace) -> int:
    serial.send_and_expect(
        f"SETPOS {args.position}", (SETPOS_ACK_LINE,)
    )
    serial.send_and_expect("ARM", (ARM_ACK_LINE,))

    demo_start = (
        f"DEMO START count={args.count} grid=0/25/50/75; "
        "STOP/Ctrl-C aborts"
    ).encode("ascii")
    first_candidate = (
        f"CANDIDATE 1/{args.count}  0-0-0  "
        "(commanded only; no open detection)"
    ).encode("ascii")
    direction_name = b"LEFT" if args.direction == "L" else b"RIGHT"
    first_move = re.compile(
        rb"MOVE "
        + direction_name
        + rb"  steps=[1-9][0-9]*  target=0\.0000  passes=3"
    )
    done_line = (
        f"DONE DEMO count={args.count}; no open detection was performed"
    ).encode("ascii")
    next_line = serial.send_line(f"DEMO {args.direction} {args.count}")
    protocol_stage = 0
    activity_deadline = time.monotonic() + ACK_TIMEOUT_SECONDS

    print("\n[runner] Demo is active. Type STOP, STATUS, or DISARM and press Enter.")
    print("[runner] Ctrl-C is translated to STOP; do not disconnect USB to stop.")
    try:
        stdin_fd = sys.stdin.fileno()
    except (AttributeError, OSError, ValueError) as error:
        raise DemoRunnerError("run the live demo from an interactive terminal") from error

    while True:
        while next_line < len(serial.lines):
            line = serial.lines[next_line]
            next_line += 1

            if line.startswith(b"ERR:"):
                detail = line.decode("utf-8", errors="replace")
                raise DemoRunnerError(
                    f"firmware reported an error during DEMO: {detail}"
                )
            if line.startswith(b"STOPPED:") or line in DISARM_ACK_LINES[:2]:
                print("\n[runner] Demo ended by operator request.")
                return 130

            if protocol_stage == 0:
                if line == demo_start:
                    protocol_stage = 1
                    continue
                if line.startswith(
                    (b"DEMO START", b"CANDIDATE ", b"MOVE ", b"DONE DEMO count=")
                ):
                    raise DemoRunnerError(
                        "firmware returned an unexpected or out-of-order DEMO start"
                    )
            elif protocol_stage == 1:
                if line == first_candidate:
                    protocol_stage = 2
                    continue
                if line.startswith((b"CANDIDATE ", b"MOVE ", b"DONE DEMO count=")):
                    raise DemoRunnerError(
                        "firmware returned an unexpected or out-of-order first candidate"
                    )
            elif protocol_stage == 2:
                if first_move.fullmatch(line) is not None:
                    protocol_stage = 3
                    continue
                if line.startswith((b"MOVE ", b"DONE DEMO count=")):
                    raise DemoRunnerError(
                        "firmware did not report the expected first DEMO move"
                    )

            if line == done_line:
                disarm_after_completion(serial)
                return 0
            if line.startswith(b"DONE DEMO count="):
                raise DemoRunnerError(
                    "firmware reported completion for a different DEMO count"
                )

        if protocol_stage < 3 and time.monotonic() >= activity_deadline:
            raise DemoRunnerError(
                "timed out waiting for ordered DEMO start, candidate, and MOVE lines"
            )

        fd = serial._require_fd()
        try:
            readable, _, _ = select.select([fd, stdin_fd], [], [], 0.25)
        except (OSError, ValueError) as error:
            raise DemoRunnerError(f"could not monitor terminal and controller: {error}") from error

        if fd in readable:
            serial._read_ready(0.0)
        if stdin_fd in readable:
            line = sys.stdin.readline()
            if line == "":
                raise AbortRequested(0)
            command = line.strip().upper()
            if not command:
                continue
            if command == "STOP":
                if not force_stop(serial, urgent_on_failure=True):
                    raise DemoRunnerError("STOP was not acknowledged")
                print("\n[runner] Demo stopped and controller disarmed.")
                return 130
            if command == "DISARM":
                serial.send_and_expect(
                    "DISARM",
                    DISARM_ACK_LINES,
                )
                print("\n[runner] Demo stopped and controller disarmed.")
                return 130
            if command == "STATUS":
                serial.write_line("STATUS")
                continue
            print("[runner] While DEMO runs, enter only STOP, STATUS, or DISARM.")


def install_signal_handlers() -> dict[int, object]:
    previous: dict[int, object] = {}

    def request_abort(signum: int, _frame: object) -> None:
        raise AbortRequested(signum)

    for name in ("SIGINT", "SIGTERM", "SIGHUP", "SIGQUIT", "SIGTSTP"):
        signum = getattr(signal, name, None)
        if signum is not None:
            previous[signum] = signal.getsignal(signum)
            signal.signal(signum, request_abort)
    return previous


def restore_signal_handlers(previous: dict[int, object]) -> None:
    for signum, handler in previous.items():
        signal.signal(signum, handler)


def ignore_stop_signals(previous: dict[int, object]) -> None:
    for signum in previous:
        signal.signal(signum, signal.SIG_IGN)


def run_live(args: argparse.Namespace, port: str) -> int:
    serial = PosixSerial(port)
    previous_handlers = install_signal_handlers()
    controller_safe = False
    port_opened_once = False
    cleanup_attempted = False
    try:
        serial.open()
        port_opened_once = True
        # Opening a port may attach to a controller left moving by another
        # process. Until STOP is acknowledged, its state is unknown.
        print(f"[runner] Connected to {port} at {BAUD_RATE} baud; forcing STOP.")
        if not force_stop(serial, urgent_on_failure=True):
            cleanup_attempted = True
            raise DemoRunnerError(
                "the device did not acknowledge STOP; verify that current Wheel "
                "Wrecker firmware is uploaded and that the selected port is correct"
            )
        controller_safe = True
        verify_firmware(serial)

        if not read_confirmation(args):
            print("[runner] Confirmation did not match; no motion commands were sent.")
            return 0

        controller_safe = False
        result = run_interactive(serial, args)
        # Every normal return from run_interactive follows an acknowledged
        # STOP or DISARM response.
        controller_safe = True
        return result
    except AbortRequested as abort:
        ignore_stop_signals(previous_handlers)
        name = signal.Signals(abort.signum).name if abort.signum else "terminal EOF"
        safe_stderr(f"\n[runner] {name} received; sending textual STOP.")
        acknowledged = force_stop(serial, urgent_on_failure=True)
        cleanup_attempted = True
        controller_safe = acknowledged
        return 130 if acknowledged else 1
    except KeyboardInterrupt:
        ignore_stop_signals(previous_handlers)
        safe_stderr("\n[runner] Ctrl-C received; sending textual STOP.")
        acknowledged = force_stop(serial, urgent_on_failure=True)
        cleanup_attempted = True
        controller_safe = acknowledged
        return 130 if acknowledged else 1
    except SerialConnectionLost as error:
        safe_stderr(f"\n[runner] Serial connection lost: {error}")
        if not controller_safe and serial.fd is not None:
            ignore_stop_signals(previous_handlers)
            controller_safe = force_stop(serial, urgent_on_failure=True)
            cleanup_attempted = True
        elif not controller_safe:
            safe_stderr(
                "!!! SOFTWARE CANNOT DELIVER STOP — CUT 24 V MOTOR POWER NOW !!!"
            )
            cleanup_attempted = True
        return 1
    except DemoRunnerError as error:
        safe_stderr(f"\n[runner] ERROR: {error}")
        return 1
    finally:
        try:
            if not controller_safe and not cleanup_attempted:
                ignore_stop_signals(previous_handlers)
                if serial.fd is not None:
                    controller_safe = force_stop(
                        serial, urgent_on_failure=True
                    )
                elif port_opened_once:
                    safe_stderr(
                        "!!! SOFTWARE CANNOT DELIVER STOP — "
                        "CUT 24 V MOTOR POWER NOW !!!"
                    )
        except BaseException as cleanup_error:
            # Never allow an error in the last-resort cleanup path to hide the
            # fact that driver state is unknown.
            safe_stderr(
                "!!! STOP CLEANUP FAILED — CUT 24 V MOTOR POWER NOW !!!"
            )
            safe_stderr(f"[runner] cleanup error: {cleanup_error}")
        finally:
            serial.close()
            restore_signal_handlers(previous_handlers)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    try:
        if args.port:
            port = args.port
        elif args.dry_run:
            port = "<auto-detected UNO R4 WiFi during a live run>"
        else:
            port = discover_port()

        print_plan(args, port)
        if args.dry_run:
            print(f"Dry run only. Live confirmation would be: {confirmation_phrase(args)}")
            print("No serial port was opened and no command was sent.")
            return 0
        return run_live(args, port)
    except DemoRunnerError as error:
        safe_stderr(f"[runner] ERROR: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
