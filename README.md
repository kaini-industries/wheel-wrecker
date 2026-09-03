# Wheel Wrecker

Wheel Wrecker is now a conservative precision-motion foundation for an
owner-authorized safe-lock test fixture. It targets the photographed Arduino
UNO R4 WiFi, 1.8° NEMA 23 motor, and STEP/DIR/ENA TB-style driver.

The firmware boots with no motion scheduled, ENA requesting offline,
unreferenced, and disarmed. Reset-time ENA safety still requires the hardware
pull-down documented in `HARDWARE.md`. A nonblocking motion loop keeps serial
`STOP` or Ctrl-C responsive, integer microstep ticks model the 100-mark dial,
and directed moves and conservative combination sequences are supported. It
does **not** claim that commanded steps equal measured shaft position, and it
does not run an unattended search without position and open-detection feedback.

Only use this project on locks you own or have explicit permission to test.

## Why the old sketch was inaccurate

The original build configuration targeted the classic AVR Uno rather than the
UNO R4 WiFi. The sketch also treated AccelStepper `moveTo()` targets as relative
moves. Its live targets were 1600, 3200, 4800, and 7200 steps; at 3200
steps/revolution those produce relative moves of 1/2, 1/2, 1/2, and 3/4 of a
turn, followed by a 1-3/4-turn reversal when the loop repeats. The `oneRev`
constant was not used by that code path, and the configured speed and
acceleration were high enough to make lost steps plausible.

The replacement has one source of truth for geometry:

```text
steps per dial revolution = 200 × microstep × motor-revs-per-dial-rev
```

At the default 1/16 setting with a direct coupler, one dial revolution is 3200
steps and one of 100 dial marks is exactly 32 steps.

## Project layout

- `include/HardwareConfig.h` — pin polarity, steps/revolution, speed, and safety
  timing defaults.
- `include/DialMath.h` / `src/DialMath.cpp` — hardware-independent integer dial
  geometry and combination planning.
- `src/main.cpp` — UNO R4 motion service and fixed-buffer serial console.
- `test/native/test_dial_math.cpp` — host tests for wraparound, quantization,
  direction, pass counts, and 4/3/2-arrival combination planning.
- `HARDWARE.md` — observed parts, wiring assumptions, DIP settings, and power
  safety checks.
- `docs/COMMISSIONING.md` — staged bring-up and calibration procedure.
- `docs/ARCHITECTURE.md` — coordinate/state invariants and feedback extension
  points.

## Build

Install PlatformIO, then run:

```sh
pio run
pio run --target upload
pio device monitor --baud 115200
```

The build target is `renesas-ra / uno_r4_wifi`. The current firmware build uses
about 21% of flash and 10% of RAM on the RA4M1.

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
test. Make the verified value permanent in `HardwareConfig.h`.

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

Runtime settings are deliberately not saved. Once verified, update
`HardwareConfig.h` so every power cycle starts from an auditable configuration.
When the HOLD timer releases motor torque, the firmware also invalidates its
position reference; run `SETPOS` again before the next move. Issue the next
command before HOLD expires or lengthen HOLD during a supervised sequence if
you need continuity between commands.

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

## Reference project

The linked [LockManipulator Auto Dialer](https://github.com/LockManipulator/Locksport/tree/main/Safe%20manipulation/Auto%20Dialer)
informed the coordinate model and highlighted the importance of controlled
acceleration and explicit wheel direction. Its implementation is for an
ESP32-S3 plus TMC5160 and uses that driver's StallGuard feature for opening
detection, so it is not directly portable to this UNO R4 plus TB-style driver.
Its source is under the PolyForm Noncommercial 1.0.0 license. This repository's
replacement is an independent implementation rather than a copied port.
