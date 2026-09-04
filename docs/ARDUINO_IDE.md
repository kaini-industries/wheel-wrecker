# Arduino IDE setup and smoke test

This is the primary setup path for Wheel Wrecker. The repository already has a
complete Arduino sketch; no source files need to be copied, renamed, or
generated.

Only use this project on locks you own or have explicit permission to test.

## Before connecting the Arduino

1. Turn off and disconnect the 24 V motor supply.
2. Remove the motor-to-dial coupler.
3. Keep the physical motor-power cutoff within reach.
4. Complete the power, enclosure, earth, fuse, ENA fail-safe, and wiring checks
   in [`HARDWARE.md`](../HARDWARE.md).
5. Use a data-capable USB-C cable. Some charging-only cables power the board but
   cannot upload firmware.

The first compile, upload, and serial checks do not require motor power.

## 1. Open the canonical sketch

Install Arduino IDE 2, then choose **File > Open** and select:

```text
arduino/WheelWrecker/WheelWrecker.ino
```

The IDE should show `WheelWrecker.ino`, `Firmware.cpp`, `Firmware.h`,
`HardwareConfig.h`, `DialMath.cpp`, `DialMath.h`, `StatusDisplay.cpp`, and
`StatusDisplay.h` as tabs. Open the `.ino` file rather than an individual
`.cpp` file, and do not move or copy the tabs into a separate sketch. Keeping
the checkout intact ensures Arduino IDE, Arduino CLI, PlatformIO, and the host
tests all use the same implementation.

## 2. Install the board package

1. Open **Tools > Board > Boards Manager**.
2. Search for **Arduino UNO R4 Boards**.
3. Install version `1.6.0`.
4. Select **Tools > Board > Arduino UNO R4 Boards > Arduino UNO R4 WiFi**.

Do not select **Arduino Uno**, **UNO R4 Minima**, or a third-party UNO target.
The firmware intentionally produces a clear compile error for an unsupported
board.

## 3. Install the libraries

Open **Tools > Manage Libraries** and install this known-good set:

| Library | Author | Version |
| --- | --- | --- |
| AccelStepper | Mike McCauley | `1.64` |
| Adafruit GFX Library | Adafruit | `1.12.6` |
| Adafruit SSD1306 | Adafruit | `2.5.17` |
| Adafruit BusIO | Adafruit | `1.17.4` |

Arduino IDE may offer to install dependencies while installing the Adafruit
libraries. Accept that prompt, then confirm the installed versions in Library
Manager. The OLED is optional at runtime, but its libraries are compile-time
dependencies and must still be installed.

## 4. Verify and upload safely

1. Confirm again that 24 V motor power is disconnected.
2. Connect the UNO R4 WiFi over USB-C.
3. Use the board selector or **Tools > Port** to select the detected UNO R4
   WiFi port. The port name varies by computer, so the project does not store
   one.
4. Click **Verify** and wait for compilation to finish without errors.
5. Click **Upload**.

If upload succeeds, leave motor power off for the software smoke test.

## 5. Configure Serial Monitor

Open **Tools > Serial Monitor** and set:

```text
Baud rate:   115200
Line ending: New Line
```

Press RESET once if the boot messages appeared before Serial Monitor opened.
The boot report must say that motion is disarmed, position is unknown, and ENA
offline was requested. `OLED=READY@0x3C` and `OLED=READY@0x3D` are both valid;
`OLED=MISSING` is also safe and leaves serial control available.

## 6. Run the motor-power-off smoke test

Send one line at a time:

```text
STATUS
TURN L 1
ARM
TURN L 1
DISARM
OLED RETRY
```

Expected results:

- The first `TURN` is rejected because the controller is disarmed.
- `ARM` permits motion commands but does not itself move or energize the motor.
- The second `TURN` is rejected because no physical position was declared.
- `DISARM` requests driver-off and leaves position unknown.
- `OLED RETRY` probes `0x3C` and `0x3D`; a missing display remains nonfatal.

Next, exercise the stop path while the driver is still unpowered:

```text
SETPOS 0
ARM
TURN L 10
STOP
STATUS
```

Send `STOP` while the turn is in progress. It must cancel motion, disarm,
request driver-off, and mark position unknown. Type the textual command and
press Enter; some terminal software intercepts Ctrl-C instead of sending it to
the board.

Still with 24 V motor power disconnected, check the bounded demonstration and
its cancellation path:

```text
SETPOS 0
ARM
DEMO L 4
STOP
```

`DEMO L 4` starts the first four three-wheel candidates over
`{0,25,50,75}`—`(0,0,0)`, `(0,0,25)`, `(0,0,50)`, and `(0,0,75)`—with the last
number changing fastest. Each candidate uses normal fourth/third/second-arrival
combination motion. Send `STOP` before it finishes and confirm that it cancels
the entire demonstration, disarms, requests driver-off, and invalidates the
position reference. This unpowered check validates command handling and
cancellation; it cannot validate physical motion.

Restore the position reference and arming state, then check the parser limits:

```text
SETPOS 0
ARM
DEMO L 0
DEMO L 65
DEMO X 1
DEMO L 1.5
```

Each `DEMO` line must be rejected without starting a sequence. Finally, run
`DEMO R 2` to completion. Confirm the serial log advances from candidate 1 to
2, the OLED (if present) advances from `DEMO 1/2 1/3` through
`DEMO 2/2 3/3`, and an immediate prompt follows `DONE DEMO count=2`. Normal
completion leaves the controller armed and starts the configured `HOLD` timer.
At expiry it reports the driver-off request, invalidates the position
reference, and prints a fresh prompt. Issue `DISARM` when the check is complete.

Do not energize the driver if any safety gate or stop behavior differs from
these expectations. Continue with the uncoupled motor procedure in
[`COMMISSIONING.md`](COMMISSIONING.md) only after this test passes.

## 7. Run the supervised demo helper

The executable runner uses only the Python 3 standard library and controls
firmware that has already been uploaded; it does not compile or upload the
sketch. Finish the motor-power-off checks above first. Close Arduino IDE's
**Serial Monitor** before running the helper because only one process can own
the serial port.

From the repository root, preview the default session:

```sh
./scripts/run_motor_demo.py --dry-run
```

Dry-run mode validates the arguments and prints the planned session without
opening a serial port, arming the controller, or moving the motor. A live run
with the defaults uses left-first motion, four candidates, and a declared
starting position of mark zero:

```sh
./scripts/run_motor_demo.py
```

When `--port` is omitted, the runner asks Arduino CLI to find exactly one UNO
R4 WiFi. Specify a port if auto-detection is unavailable or ambiguous, and
override the motion arguments as needed:

```sh
./scripts/run_motor_demo.py \
  --port /dev/cu.usbmodem1101 \
  --direction R \
  --count 2 \
  --position 0
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--port PATH` | Use this serial port instead of Arduino CLI auto-detection | One exact UNO R4 WiFi |
| `--direction L\|R` | First combination direction | `L` |
| `--count 1..64` | Number of coarse candidates | `4` |
| `--position 0..<100` | Mark physically aligned under the index | `0` |
| `--dry-run` | Print the planned session without opening the port | Off |

`--position` declares the current open-loop coordinate; it does not find or
home the dial. Physically align the indicated mark under the index before
confirming a live run.

The runner communicates directly at 115200 baud. Its preflight sends `STOP`,
then verifies an idle, disarmed, unknown-position response and checks that the
uploaded firmware's `HELP` output includes `DEMO`. It prints the commands it
will send and requires the exact confirmation `RUN <direction> <count>`—for
example, `RUN L 4`—before sending `SETPOS`, `ARM`, and `DEMO`.

During the run, type `STOP`, `STATUS`, or `DISARM` and press Enter to send that
text to the firmware. The runner translates Ctrl-C, SIGTERM, SIGHUP, and
terminal EOF into a textual `STOP`; after a normal `DONE DEMO`, it also sends
`DISARM`. Wait for the corresponding firmware response before assuming the
motor is stopped or offline.

USB loss, cable removal, computer failure, and SIGKILL can prevent the runner
from sending its cleanup commands. Keep the physical motor-power cutoff within
reach for every powered run and use it immediately if serial control is lost.

## Reproduce the build with Arduino CLI

The checked-in `sketch.yaml` pins the same board core and libraries used above.
From the repository root:

```sh
arduino-cli compile --profile uno_r4_wifi arduino/WheelWrecker
```

The tested Arduino CLI build reports 91,688 bytes (34%) of flash and 9,852
bytes (30%) of static RAM. With `--warnings all`, core version `1.6.0` emits
some warnings from its own `Wire`, interrupt, and USB sources; the Wheel Wrecker
sources compile without warnings and the build completes successfully.

The profile intentionally does not store a serial port. To upload with Arduino
CLI, determine the current port with `arduino-cli board list` and pass that
specific port explicitly.

## Troubleshooting

### The sketch opens as a single file or files appear missing

Close it and open `arduino/WheelWrecker/WheelWrecker.ino` in place. Do not copy
only the `.ino`; the companion `.cpp` and `.h` files must remain beside it.

### `AccelStepper.h` or an Adafruit header cannot be found

Open Library Manager and install the exact libraries in the table above. The
OLED may be physically absent, but the Adafruit libraries are still required
to compile the firmware.

### The build reports that the selected board is unsupported

Select **Arduino UNO R4 WiFi**, not the classic **Arduino Uno** or **UNO R4
Minima**. If UNO R4 WiFi is not listed, install **Arduino UNO R4 Boards** `1.6.0`
through Boards Manager.

### No upload port appears

- Try a known data-capable USB-C cable and a direct computer USB port.
- Disconnect the 24 V motor supply while diagnosing USB.
- Close other serial-terminal applications.
- Double-press RESET to enter the UNO R4 bootloader, reselect the port if it
  changes, and upload again.

### Upload reports that the port is busy

Close Serial Monitor and any other program using the port, then upload again.
Reopen Serial Monitor afterward at `115200` baud.

### Upload succeeds but Serial Monitor is blank or unreadable

Select `115200` baud, set **New Line**, and press RESET once. Confirm that the
monitor is attached to the same port used for upload.

### The OLED reports `MISSING` or `FAULT`

This does not prevent serial operation. Keep the motor disarmed and follow the
OLED wiring and recovery table in [`COMMISSIONING.md`](COMMISSIONING.md). Never
infer the module pin order from wire color.
