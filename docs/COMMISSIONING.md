# Commissioning and calibration

Bring the system up in stages. A scale mismatch, a direction mismatch, random
lost steps, coupler backlash, and wheel-pack fly/backlash are different errors;
changing one `stepsPerRevolution` number cannot fix all of them.

## 1. Power-off inspection

1. Confirm you own the test lock or have explicit authorization.
2. Complete every enclosure, earth, fuse, strain-relief, and cutoff check in
   `HARDWARE.md`.
3. Read the installed motor label. The photographed unit is 1.8° and 3 A; it is
   not the 4 A motor in the original product link.
4. With all power disconnected, use continuity mode to identify PUL, DIR, and
   ENA endpoints. Confirm they match
   `arduino/WheelWrecker/HardwareConfig.h`.
5. Install the ENA− pull-down described in `HARDWARE.md`, and verify that the
   driver is offline during controller reset. A serial stop is not a substitute
   for the physical motor-power cutoff.
6. Set the driver to the first-test pattern documented in `HARDWARE.md`:
   `S1..S6 = OFF, OFF, ON, ON, OFF, ON` (`↑ ↑ ↓ ↓ ↑ ↓`). This is 1/16
   microstepping at the 1.0 A row and matches the firmware's 3,200-step default.
7. If using the OLED, follow its printed `VCC`, `GND`, `SDA`, and `SCL` labels
   to the main UNO R4 I²C bus as documented in `HARDWARE.md`. Do not rely on
   wire color or assumed connector order. Confirm D4 remains dedicated to ENA.
8. Remove the motor-to-dial coupler. Mark the motor shaft and housing so full
   revolutions are easy to count.

## 2. Compile, upload, and test interlocks

Keep the 24 V motor supply disconnected and the motor-to-dial coupler removed.
Follow [`ARDUINO_IDE.md`](ARDUINO_IDE.md) to install the pinned UNO R4 core and
libraries, open `arduino/WheelWrecker/WheelWrecker.ino`, verify it, and upload
it through Arduino IDE.

Open Serial Monitor at `115200` baud with **New Line** selected and press RESET.
Every boot must report `ENA offline requested`, `motion disarmed`, and
`position unknown`. With the OLED attached, `STATUS` should report
`OLED=READY@0x3C` or `OLED=READY@0x3D`; a missing OLED is nonfatal.

With motor power still disconnected, verify the command gates:

```text
STATUS
TURN L 1
ARM
TURN L 1
DISARM
OLED RETRY
```

The first `TURN` must be rejected because motion is disarmed. After `ARM`, the
second `TURN` must be rejected because position is unknown. `DISARM` must leave
the driver-off state requested and the position unknown.

Then exercise cancellation without motor power:

```text
SETPOS 0
ARM
TURN L 10
STOP
STATUS
```

Send `STOP` while the command is active. The controller must stop scheduling
pulses, cancel the sequence, disarm, request driver-off, and invalidate its
position. Use the text command for this check because some serial terminals
intercept Ctrl-C. Correct any failure before energizing the motor driver.

## 3. Unloaded direction and one-turn test

Power the Arduino by USB first and confirm the safe boot state again. With the
OLED attached, the screen should show `DISARMED`, `ENA:OFF?`, and
`CMD REF UNKNOWN`. If it reports `OLED=MISSING`, leave the motor disarmed and
use the troubleshooting checks below; motion firmware remains available.

With the motor driver power cutoff within reach:

```text
SETPOS 0
ARM
TURN L 1
```

The shaft should make exactly one smooth counterclockwise turn when viewed from
the end that will face the dial. If it turns clockwise:

```text
STOP
SET REVERSE ON
SETPOS 0
ARM
TURN L 1
```

Once confirmed, make the direction setting permanent with
`kDirectionPinInverted` in `arduino/WheelWrecker/HardwareConfig.h`. Do not swap
one lead of a motor phase; reverse either a complete phase pair or the DIR
polarity.

The default HOLD timer requests driver-off two seconds after a completed
sequence and then invalidates the software position. If that happens between
these test commands, physically realign and run `SETPOS 0` again. During a
closely supervised calibration session, `SET HOLD 60000` gives one minute to
issue the next command while retaining holding torque; reduce it afterward.

If the shaft moves 1/2, 1/4, 2, or 4 turns instead of one, the DIP microstep
setting and firmware SPR do not agree. Correct the DIP/configuration mismatch;
do not use an arbitrary empirical value to hide it.

## 4. Multi-turn scale and return test

Run ten slow turns in each direction and count the shaft marks:

```text
TURN L 10
TURN R 10
```

Both commands should produce ten turns and return to the starting mark. Interpret
the result before changing anything:

| Observation | Likely cause | Action |
| --- | --- | --- |
| Repeatable integer factor error | Wrong microstep DIP or transmission ratio | Correct DIP or compute the real mechanical ratio |
| Error grows by the same amount every turn | Fixed scale ratio | Set `SPR = 200 × microstep × motor-revs/dial-rev` |
| Different result on repeated trials | Lost steps, loose coupler, wiring noise, or excessive speed/acceleration | Fix mechanics/electrical issue; do not calibrate it out |
| Returns differently after reversing | Backlash, loose coupling, or inertial overrun | Reduce speed/acceleration, improve coupling, measure directional take-up |

For a known fixed ratio only, a trial correction can be calculated after a
multi-turn measurement:

```text
CAL SCALE 10 9.95
```

This applies:

```text
new SPR = old SPR × commanded revolutions / observed revolutions
```

The command invalidates position and lasts only until reset. Re-test, then put
the derived mechanical value in `arduino/WheelWrecker/HardwareConfig.h`. A
direct-coupled stepper should normally use the exact DIP-derived value, not a
correction for random error.

## 5. Dial-side test

Attach the coupler to a visible, unloaded dial or printed 0–99 test wheel. Keep
the driver current low.

```text
SETPOS 0
ARM
GOTO L 25
GOTO L 50
GOTO L 75
GOTO L 0
```

At 3200 SPR, each leg is 800 pulses. Repeat in the opposite direction. Then run
several L/R full-turn returns and check the physical index with a magnifier or
camera. One pulse is 0.03125 dial mark (0.1125°) at this setting, but microstep
resolution is not the same as guaranteed mechanical accuracy.

Increase speed gradually only after repeatability is proven under the final
load. The defaults are 0.30 dial rev/s maximum and 0.50 dial rev/s². A heavier
wheel pack benefits from slower acceleration and a longer target settle dwell.

## 6. Known-combination fixture test

Use a transparent practice lock or an open, known-combination lock where wheel
pickup can be observed. Confirm the lock's first direction; it is not universal.

Example:

```text
SETPOS 0
ARM
COMBO L 20 40 60
```

For a three-wheel lock this means:

1. Turn left, pass 20 three times, stop on its fourth arrival.
2. Turn right, pass 40 twice, stop on its third arrival.
3. Turn left, pass 60 once, stop on its second arrival.

The firmware then stops. Operate the opening step manually. Do not command the
stepper hard into the lock's contact point: the current driver cannot sense
load or know when to stop.

## 7. Conditions for autonomous searching

Do not add an unattended combination loop until all of these are true:

- A dial-side encoder or index verifies that commanded motion actually occurred.
- An independent sensor reliably identifies the open state.
- A normally closed physical stop removes driver enable or motor power.
- Known-combination trials have bounded the loaded position error in both
  directions over many repetitions.
- The mounting and torque limiter cannot damage the lock when software or a
  sensor fails.
- Progress/checkpoint behavior has been tested through power loss.

The upstream project uses TMC5160 StallGuard during a carefully calibrated
opening sweep. The TB-style driver has no equivalent telemetry; software cannot
infer it from STEP and DIR alone.

## OLED troubleshooting

Keep the motor disarmed while changing or diagnosing the display wiring.

| Symptom | Check |
| --- | --- |
| `OLED=MISSING` | Power off, verify the module's own pin labels lead to 5V, GND, SDA/A4, and SCL/A5; inspect for swapped SDA/SCL |
| Still missing after wiring correction | Power the controller, issue `OLED RETRY`, then `STATUS`; the firmware checks both `0x3C` and `0x3D` |
| `OLED=FAULT` | Disarm, inspect for a loose connector or a held-low I²C line, then issue `OLED RETRY` |
| Screen works but motor timing changes | Stop testing and report it as a firmware defect; screen transfers are designed to occur only while stationary |
| Screen text disagrees with the dial | Treat the screen as commanded state only; realign physically and use `SETPOS`, then investigate missed steps or slip |

Also perform one supervised run with the OLED unplugged. Boot must report
`OLED=MISSING`, and the unloaded one-turn serial test must still work normally.
