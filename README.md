# Wheel Wrecker

Wheel Wrecker is a conservative precision-motion controller for an
owner-authorized safe-lock test fixture. It targets an Arduino UNO R4 WiFi,
1.8° NEMA 23 motor, STEP/DIR/ENA TB-style driver, and optional 128×64 SSD1306
I²C status display.

The firmware boots with no motion scheduled, ENA requesting offline,
unreferenced, and disarmed. Reset-time ENA safety requires the hardware
pull-down documented in `HARDWARE.md`. A nonblocking motion loop keeps serial
`STOP` or Ctrl-C responsive, integer microstep ticks model the 100-mark dial,
and directed moves and conservative combination sequences are supported. It
does **not** claim that commanded steps equal measured shaft position, and it
does not run an unattended search without position and open-detection feedback.

Only use this project on locks you own or have explicit permission to test.

## Motion model

The controller has one source of truth for dial geometry:

```text
steps per dial revolution = 200 × microstep × motor-revs-per-dial-rev
```

At the default 1/16 setting with a direct coupler, one dial revolution is 3200
steps and one of 100 dial marks is exactly 32 steps.

## Hardware

The hardware configuration is based on the photographed build and the
user-reported OLED installation. Treat the labels on the physical parts as
authoritative: generic driver listings and the originally saved motor listing
do not exactly identify every installed part. See [`HARDWARE.md`](HARDWARE.md)
for the full electrical notes and [`docs/COMMISSIONING.md`](docs/COMMISSIONING.md)
for the staged bring-up procedure.

### Installed components

| Component | Hardware in this build | Role and constraints |
| --- | --- | --- |
| Controller | Arduino UNO R4 WiFi | RA4M1 controller with 5 V GPIO; the firmware is R4-WiFi-specific and rejects a classic Uno or R4 Minima |
| Motor driver | Generic upgraded TB-style microstep driver, 9–42 V, 4 A peak | Accepts only PUL/DIR/ENA commands; it provides no encoder, stall/load telemetry, or usable fault feedback |
| Stepper motor | `57HS8030A4D8`, 1.8°, 3 A | 200 full steps per revolution; the photographed label gives black A+, green A−, red B+, and blue B− |
| Motor supply | `S-350-24`, 24 V, 14.6 A, 350 W | Considerably larger than one motor branch requires; it needs branch protection and a covered, properly earthed enclosure |
| Status display | ideaspark 0.96-inch, 128×64 SSD1306 OLED with protector case | Four-pin I²C display used for informational status only; serial operation continues if it is missing |
| Mechanical drive | Motor-to-dial coupler | Firmware currently assumes direct 1:1 drive; measure and configure any gear or belt ratio before relying on dial positions |

Reference listings: [power supply](https://www.amazon.com/dp/B0DSK3XN1M),
[driver](https://www.amazon.com/dp/B0DSJ53J9N), and
[OLED display](https://www.amazon.com/dp/B0F8HRSF31). The
[original motor listing](https://www.amazon.com/dp/B091C37FJ2) describes a
different 4 A motor and must not be used to choose the installed motor's
current setting.

### Motor-driver DIP switches

Disconnect both USB and 24 V power and wait for the indicators to go dark
before changing a DIP switch or motor lead. On the photographed driver's red
switch block, **ON is down**, toward the switch numbers and the `ON ↓` legend.
Do not copy the old photographed positions; deliberately set all six switches
to this initial uncoupled-test pattern:

| Switch | S1 | S2 | S3 | S4 | S5 | S6 |
| --- | --- | --- | --- | --- | --- | --- |
| State | OFF | OFF | ON | ON | OFF | ON |
| Lever | ↑ | ↑ | ↓ | ↓ | ↑ | ↓ |

S1–S3 select 1/16 microstepping, giving 3,200 input pulses per motor
revolution. S4–S6 select the driver's 1.0 A nominal / 1.2 A peak row, which is
the low-current starting point for an uncoupled motor. If the final mechanism
needs more torque, increase current only one labeled row at a time while
checking for missed steps and monitoring motor and driver temperature. The
2.8 A row is a conservative ceiling near the photographed motor's 3 A rating,
not a starting setting. Use the table printed on this exact driver because DIP
patterns are not interchangeable among visually similar TB-style modules.

With a 1.8° motor, 1/16 microstepping, and a direct coupler, the matching
firmware value is:

```text
200 full steps/rev × 16 microsteps × 1:1 drive = 3200 pulses/dial rev
3200 pulses/dial rev ÷ 100 dial marks = 32 pulses/mark
```

The persistent value is `kStepsPerDialRevolution` in
[`HardwareConfig.h`](arduino/WheelWrecker/HardwareConfig.h). `SET SPR` is only
a temporary calibration aid for the current boot; it does not save a setting.

### Arduino-to-driver signals

The current firmware configuration assumes the common-anode wiring likely
shown in the photos:

| Arduino UNO R4 WiFi | Driver terminal | Purpose |
| --- | --- | --- |
| `5V` | `PUL+` | STEP opto-input supply |
| `D2` | `PUL−` | Active-low STEP pulse |
| `5V` | `DIR+` | Direction opto-input supply |
| `D3` | `DIR−` | Direction signal |
| `5V` | `ENA+` | Optional enable/offline opto-input supply |
| `D4` | `ENA−` | HIGH requests enabled; LOW requests offline with the current polarity settings |

Verify every terminal label and endpoint before applying power; wire color is
not evidence of a signal. The UNO R4 WiFi GPIO current limit is 8 mA, so verify
the clone driver's opto-input current or use a suitable transistor/open-
collector buffer. For reset-time safety, the common-anode ENA− arrangement
also needs a bench-tested external pull-down so the driver remains offline
while D4 is high impedance. The unknown input circuit prevents specifying a
reliable resistor value from the case label alone.

If ENA is not wired, set `kEnablePinConnected = false` in
[`HardwareConfig.h`](arduino/WheelWrecker/HardwareConfig.h). In that mode,
`STOP` prevents further step pulses but cannot remove motor holding torque. If
the driver is wired common-cathode instead, update the STEP and ENA polarity
constants rather than changing the steps-per-revolution value.

### Motor and power wiring

The photographed motor label gives this phase mapping:

| Motor lead | Driver output |
| --- | --- |
| Black | `A+` |
| Green | `A−` |
| Red | `B+` |
| Blue | `B−` |

Connect the 24 V supply only to the motor driver's DC input. Do not feed 24 V
into the Arduino `5V` pin; power the controller over USB-C or from an
appropriately regulated converter. Never connect or disconnect the motor, move
a DIP switch, or touch the power wiring while energized.

The photographed mains supply has exposed high-energy terminals and must not
be operated in that state. Before commissioning it, provide an appropriate
enclosure, terminal cover, protective-earth bond, mains-voltage selector
check, strain relief, switch, and fused motor branch. Keep a physical motor-
power cutoff within reach. Firmware `STOP` is an operational stop, not an
emergency stop or a substitute for disconnecting power.

### OLED connection

Read the actual display's silk-screened pin order before wiring it; four-pin
SSD1306 boards do not all arrange their pins alike.

| OLED label | Arduino UNO R4 WiFi | Notes |
| --- | --- | --- |
| `VCC` | `5V` | The listing specifies 3.3–5 V operation; verify the installed board marking |
| `GND` | `GND` | Keep motor return current out of this lead |
| `SDA` | `SDA` / `A4` | Main `Wire` bus data |
| `SCL` | `SCL` / `A5` | Main `Wire` bus clock |

`SDA`/`A4` and `SCL`/`A5` are duplicate labels for the same two signals, so
use only one connection for each. The UNO R4 WiFi Qwiic connector uses the
separate 3.3 V `Wire1` bus and is not used by this firmware. D4 is reserved for
driver ENA and must not be used for the display.

At startup the firmware probes SSD1306 addresses `0x3C` and `0x3D`. A missing
display produces `OLED=MISSING` but does not disable serial control. The OLED
shows commanded state—not measured shaft position, confirmed driver state, or
proof that the lock opened—and therefore is not a safety indicator.

### Open-loop limitations and planned feedback

This hardware can count STEP commands, but it cannot observe missed steps,
coupler slip, absolute dial position after reset, or an opened lock. Manual
`SETPOS` is required after startup, `STOP`, `DISARM`, or automatic torque
release. Unattended searching remains intentionally disabled.

Reliable autonomous operation requires dial-side position feedback, preferably
an absolute encoder or at least a repeatable optical/Hall index, plus an
independent opening detector such as a handle/bolt contact or carefully limited
load sensing. A load-aware driver such as the reference project's TMC5160 would
be a hardware change; the installed PUL/DIR/ENA driver cannot gain that
telemetry through firmware alone.

Complete the motor-power-off and uncoupled tests in
[`docs/COMMISSIONING.md`](docs/COMMISSIONING.md) before attaching the motor to
a dial or lock.

## Project layout

- `arduino/WheelWrecker/WheelWrecker.ino` — the sketch to open in Arduino IDE.
- `arduino/WheelWrecker/Firmware.h` and `Firmware.cpp` — UNO R4 motion service
  and fixed-buffer serial console.
- `arduino/WheelWrecker/HardwareConfig.h` — pin polarity, steps/revolution,
  speed, and safety timing defaults.
- `arduino/WheelWrecker/DialMath.h` and `DialMath.cpp` — hardware-independent
  integer dial geometry and combination planning.
- `arduino/WheelWrecker/StatusDisplay.h` and `StatusDisplay.cpp` — optional,
  fault-tolerant OLED discovery and stationary status rendering.
- `arduino/WheelWrecker/sketch.yaml` — reproducible Arduino CLI board and
  library profile.
- `test/native/test_dial_math.cpp` — host tests for wraparound, quantization,
  direction, pass counts, and 4/3/2-arrival combination planning.
- `HARDWARE.md` — observed parts, wiring assumptions, DIP settings, and power
  safety checks.
- `docs/ARDUINO_IDE.md` — detailed Arduino IDE setup, upload, smoke test, and
  troubleshooting.
- `docs/COMMISSIONING.md` — staged bring-up and calibration procedure.
- `docs/ARCHITECTURE.md` — coordinate/state invariants and feedback extension
  points.

All firmware implementations live in the Arduino sketch directory. Arduino
IDE, Arduino CLI, and PlatformIO compile those same files; there is no second
copy to synchronize.

## Arduino IDE quick start

Before connecting USB, turn off and disconnect the 24 V motor supply and remove
the motor-to-dial coupler. Upload and test the software interlocks before
energizing the driver.

1. In Arduino IDE 2, choose **File > Open** and open
   `arduino/WheelWrecker/WheelWrecker.ino` from this checkout. Do not copy or
   rename the individual tabs.
2. In **Tools > Board > Boards Manager**, install **Arduino UNO R4 Boards**
   version `1.6.0`.
3. Select **Tools > Board > Arduino UNO R4 Boards > Arduino UNO R4 WiFi**.
4. In **Tools > Manage Libraries**, install these known-good versions:

   - **AccelStepper** by Mike McCauley, `1.64`
   - **Adafruit GFX Library**, `1.12.6`
   - **Adafruit SSD1306**, `2.5.17`
   - **Adafruit BusIO**, `1.17.4`

5. Connect the UNO R4 WiFi by a data-capable USB-C cable and select its port.
6. Click **Verify**, then **Upload**.
7. Open **Tools > Serial Monitor**, select `115200` baud and **New Line**, then
   press RESET once. Confirm that boot reports motion disarmed, position
   unknown, and ENA offline requested before issuing any commands.

Continue with the motor-power-off smoke test in
[`docs/ARDUINO_IDE.md`](docs/ARDUINO_IDE.md), then use the complete staged
procedure in [`docs/COMMISSIONING.md`](docs/COMMISSIONING.md). A classic Arduino
Uno is not compatible; the sketch intentionally rejects the wrong board at
compile time.

## Other build and test paths

The checked-in Arduino profile provides a clean command-line build with the
pinned core and library versions:

```sh
arduino-cli compile --profile uno_r4_wifi arduino/WheelWrecker
```

PlatformIO remains available for warnings and automated verification:

```sh
make build
pio run --target upload
pio device monitor --baud 115200
```

The PlatformIO build target is `renesas-ra / uno_r4_wifi` and compiles the same
`arduino/WheelWrecker` sources used by Arduino IDE. The known-good dependencies
are pinned in both `platformio.ini` and `sketch.yaml`. With those versions,
PlatformIO reports 76,480 bytes of flash and 5,952 bytes of static RAM; Arduino
CLI reports about 90.7 KB and 9,844 bytes respectively. Both fit the RA4M1, and
the build systems link and account for the core differently. The display
library also allocates a 1,024-byte framebuffer at runtime when an OLED is
detected.

Run the hardware-independent tests with any C++11 compiler:

```sh
make test
make test-sanitize
```

## Serial command quick start

Complete the power and wiring checks in `HARDWARE.md`, then perform the
uncoupled procedure in `docs/COMMISSIONING.md`. A minimal session looks like:

```text
HELP
SETPOS 0
ARM
TURN L 1
STATUS
```

Motion directions follow safe-dial convention: `LEFT`/`L`/`CCW` increases the
printed dial number; `RIGHT`/`R`/`CW` decreases it. If the unloaded motor moves
the wrong physical way, stop, set `SET REVERSE ON`, realign it, and repeat the
test. Make the verified value permanent in
`arduino/WheelWrecker/HardwareConfig.h`.

### Commands

| Command | Meaning |
| --- | --- |
| `HELP` | Print command help |
| `STATUS` | Show motion, arming, driver, position, and calibration state; returns a short `BUSY` while moving |
| `ARM` | Permit motion commands; does not itself move or enable the motor |
| `STOP` or Ctrl-C | Immediately disable motion, disarm, cancel a sequence, and invalidate position |
| `DISARM` | Disable the driver and invalidate position |
| `SETPOS <mark>` | Declare the physical mark currently under the index (`0 <= mark < 100`) |
| `GOTO <L/R> <mark> [passes]` | Approach a mark in `0 <= mark < 100` only in the stated direction, optionally passing it first |
| `JOG <L/R> <units>` | Move a relative number of dial units |
| `TURN <L/R> <revolutions>` | Move relative complete/fractional revolutions, limited to 20 per command |
| `COMBO <L/R> <n1> ... <n5>` | Dial 2–5 marks in `0 <= mark < 100`, alternating direction from the supplied first direction |
| `SET SPR <steps>` | Change steps per dial revolution for this boot |
| `SET SPEED <rev/s>` | Change maximum physical dial speed for this boot |
| `SET ACCEL <rev/s²>` | Change physical dial acceleration for this boot |
| `SET REVERSE <ON/OFF>` | Test the alternate DIR polarity; invalidates position |
| `SET SETTLE <ms>` | Change the dwell after each target |
| `SET HOLD <ms>` | Change how long ENA stays energized after a completed sequence |
| `CAL SCALE <commanded> <observed>` | Calculate a trial SPR from a measured multi-turn ratio |
| `OLED RETRY` | Restart I²C and re-probe the optional display while disarmed |

Runtime settings are deliberately not saved. Once verified, update
`arduino/WheelWrecker/HardwareConfig.h` so every power cycle starts from an
auditable configuration.
When the HOLD timer releases motor torque, the firmware also invalidates its
position reference; run `SETPOS` again before the next move. Issue the next
command before HOLD expires or lengthen HOLD during a supervised sequence if
you need continuity between commands.

## OLED status display

The display is a local view of the same commanded state available over serial.
It automatically probes the usual SSD1306 I²C addresses, `0x3C` then `0x3D`,
and the `STATUS` command reports `OLED=READY@0x..`, `MISSING`, `FAULT`, or
`DISABLED`. A missing or failed display never blocks motion commands.

The large `CMD REF` value is an open-loop command reference, not encoder
feedback. Likewise, `ENA:OFF?` means the firmware requested the driver's
offline input; the Arduino cannot confirm the driver's electrical state. It
shows `ENA:N/C` when enable control is configured as disconnected. OLED updates
are held until the motor and target-settle interval are both stopped so an I²C
frame transfer cannot introduce gaps in STEP pulse service. Serial input is
serviced again after each refresh, before the next queued move may start.

See `HARDWARE.md` for label-to-label wiring. The connector pin order and wire
colors could not be verified from the project photos.

## Combination semantics

For a three-wheel example:

```text
COMBO L 20 40 60
```

the controller moves L/R/L and stops at the three targets on their fourth,
third, and second post-start arrivals. Every leg is directional; it never
chooses a shortest path. If a leg begins on its target, the first arrival is one
complete turn away, preventing the sequence from being silently shortened.

The command finishes on the final combination number. It does not force the
dial into the contact area or stall the motor to test opening, because this
driver provides no load feedback and the opening direction/travel varies by
lock. Prove the sequence on a transparent or known-combination test lock before
using it on any closed container.

## Related project

The [LockManipulator Auto Dialer](https://github.com/LockManipulator/Locksport/tree/main/Safe%20manipulation/Auto%20Dialer)
is a related design built around an ESP32-S3 and TMC5160. It uses that driver's
StallGuard feature for opening detection. Wheel Wrecker instead targets the UNO
R4 and a basic STEP/DIR/ENA driver, so it does not have equivalent load or stall
feedback. The related project's source is licensed under PolyForm
Noncommercial 1.0.0.
