# Hardware inventory and wiring

This document records the hardware visible in the project photos. Treat the
labels on the physical parts as authoritative: the motor in the photos does
not match the motor in the original shopping-list link.

## Installed hardware

| Part | Observed hardware | Important detail |
| --- | --- | --- |
| Controller | Arduino UNO R4 WiFi | RA4M1, 48 MHz, 5 V GPIO; this is not a classic AVR Uno |
| Driver | Generic upgraded TB-style microstep driver, 9–42 V, 4 A peak | PUL/DIR/ENA only; no encoder, SPI, stall telemetry, or usable fault output |
| Motor | `57HS8030A4D8`, 1.8°, 3 A | Label: black A+, green A−, red B+, blue B− |
| Motor originally listed | STEPPERONLINE `23HS32-4004S`, 2.4 Nm, 4 A | Conflicts with the photographed 3 A motor; do not use this listing to set current |
| Supply | S-350-24, 24 V, 14.6 A (350 W) | Electrically oversized for one driver; requires branch protection and a covered, earthed enclosure |
| Status display | ideaspark 0.96-inch 128×64 SSD1306 OLED in protector case, four-pin I²C | User-reported installed; optional and informational only |

Original product links:

- Power supply: <https://www.amazon.com/dp/B0DSK3XN1M>
- Originally listed motor: <https://www.amazon.com/dp/B091C37FJ2>
- Driver: <https://www.amazon.com/dp/B0DSJ53J9N>
- OLED display: <https://www.amazon.com/dp/B0F8HRSF31>

Not yet inventoried: the motor-to-dial coupler, gear/belt ratio (if not 1:1), a
dial index/home sensor, a shaft encoder, a physical emergency stop, or a sensor
that detects an opened lock.

## Driver settings

Switch power off before changing any DIP switch or motor connection. Use the
table printed on this exact driver rather than a table for another TB6600
clone.

### Reading the switches in the photos

On this driver's red switch block, **ON is down**, toward the printed switch
numbers and the `ON ↓` legend. In the notation below, `↑` is OFF and `↓` is ON.
Do not use the photographed switch positions as a baseline; set and verify all
six switches from the required pattern.

For the first uncoupled test, use:

```text
switch:   S1   S2   S3    S4   S5   S6
setting:  OFF  OFF  ON    ON   OFF  ON
lever:     ↑    ↑    ↓     ↓    ↑    ↓
```

This selects 1/16 microstepping (3,200 pulses/revolution) and 1.0 A nominal /
1.2 A peak.

Remove both USB power and 24 V driver power before moving the switches, and
wait for the supply/driver indicators to go dark. Never change a DIP switch or
motor connection while energized.

The photographed driver's label gives these useful settings:

| Setting | Switches | Result |
| --- | --- | --- |
| 1/16 microstep | S1 OFF, S2 OFF, S3 ON | 3,200 pulses per motor revolution |
| 1/8 microstep | S1 OFF, S2 ON, S3 OFF | 1,600 pulses per motor revolution |
| 2.8 A row | S4 OFF, S5 OFF, S6 ON | Conservative ceiling near the photographed motor's 3 A rating |
| 3.0 A row | S4 OFF, S5 ON, S6 OFF | Do not begin testing here; use only if actually required under load |

Start at the 1.0 A row above for uncoupled testing. If more torque is actually
needed under load, increase one row at a time: 1.5 A (`S4 ON, S5 ON, S6 OFF`),
then 2.0 A (`ON, OFF, OFF`), then 2.5 A (`OFF, ON, ON`). Treat the 2.8 A row as
a conservative ceiling for the photographed 3 A motor, not a starting point.
After each change, check for missed steps and monitor motor and driver
temperature. A NEMA 23 can damage a dial or fixture long before it reaches its
rated torque, so use a compliant or torque-limiting coupler.

The firmware default assumes 1/16 microstepping and direct 1:1 drive:

```text
pulses per dial revolution
  = 200 motor full-steps/revolution
  × 16 microsteps/full-step
  × 1 motor revolution/dial revolution
  = 3200

3200 pulses / 100 dial marks = 32 pulses per mark
```

Change `kStepsPerDialRevolution` in
`arduino/WheelWrecker/HardwareConfig.h` if the physical DIP setting or
transmission ratio differs. The `SET SPR` serial command is a runtime-only way
to test a value.

## Signal wiring assumed by the firmware

The default configuration assumes common-anode control, which is the likely
topology in the photos. Verify every endpoint with continuity mode before
powering the driver; wire colors alone are not evidence.

| Arduino UNO R4 WiFi | Driver terminal | Function |
| --- | --- | --- |
| 5V pin | PUL+ | Common-anode STEP supply |
| D2 | PUL− | Active-low STEP pulse |
| 5V pin | DIR+ | Common-anode direction supply |
| D3 | DIR− | Direction |
| 5V pin | ENA+ | Optional common-anode offline/enable supply |
| D4 | ENA− | Optional: HIGH energizes this driver, LOW requests offline |

If ENA is disconnected, set `kEnablePinConnected = false`. If using a
common-cathode topology instead, change the STEP and ENA polarity constants in
`arduino/WheelWrecker/HardwareConfig.h`; do not try to correct an electrical
polarity error by changing the steps-per-revolution value.

Add an external pull-down from ENA− to controller ground for this common-anode
topology. It keeps the driver's offline input asserted while D4 is high
impedance during reset or while the Arduino is unpowered. Select and bench-test
the resistor so ENA is reliably offline at reset while D4's HIGH-state source
current remains below the UNO R4 limit; the unknown clone input prevents naming
a trustworthy value from its case label alone. Use a properly designed buffer or
hardware interlock if those conditions cannot both be verified. Firmware cannot
make a floating reset-time pin fail-safe. If ENA is left disconnected, software
`STOP` stops pulses but cannot remove holding torque.

The UNO R4 WiFi data sheet specifies an 8 mA GPIO limit. Confirm this module's
opto-input current is below that limit. A transistor/open-collector buffer is
the safer interface if the current is unknown or if long/noisy cables are used.

## OLED status display

The four-pin SSD1306 module is an I²C display. The photos confirm the module and
connector are present, but its pin legends and the far ends of its wires are
obscured. Do not infer its pin order from this table or identify a signal by
wire color. Read the silk screen on the actual module and verify each endpoint
before applying power.

The firmware uses the UNO R4 WiFi's main `Wire` bus:

| OLED pin label | Arduino UNO R4 WiFi | Notes |
| --- | --- | --- |
| `VCC` | `5V` | The product listing specifies 3.3–5 V operation; verify the actual board marking first |
| `GND` | `GND` | Logic-power ground; keep motor current out of this lead |
| `SDA` | `SDA` / `A4` | Main I²C data |
| `SCL` | `SCL` / `A5` | Main I²C clock |

`SDA`/`A4` and `SCL`/`A5` are duplicate labels for the same main-bus signals;
use one physical connection for each signal. The UNO R4's Qwiic connector is a
separate 3.3 V `Wire1` bus and is not used by this firmware. Do not use D4 for
the display: D4 is reserved for the motor driver's ENA− input.

At boot, the firmware probes address `0x3C` and then `0x3D`. If neither
responds, it reports `OLED=MISSING` and continues as a headless serial
controller. `OLED RETRY` restarts the I²C peripheral and repeats detection while
disarmed. The display shows:

- `CMD REF`, the controller's open-loop commanded dial mark, or `UNKNOWN`.
- Armed/disarmed state and the requested ENA state.
- The queued operation, segment, direction, target, and pass count.
- The configured steps per dial revolution and a Ctrl-C stop reminder.

The screen is deliberately not a safety indicator. It cannot verify shaft
position, driver enable, or that a lock opened. Display transfers occur only
while the motion service is stationary, and disconnecting or omitting the OLED
does not prevent serial control.

`ENA:OFF?` means the controller requested offline; it is not confirmation from
the driver. If `kEnablePinConnected` is false, the screen instead shows
`ENA:N/C` because software has no enable control.

Power the Arduino off before connecting or moving the display leads. Keep its
I²C wiring short and separated from motor phase wiring and the exposed 24 V and
mains terminals.

## Motor and power wiring

Based on the label on the photographed motor:

| Motor lead | Driver terminal |
| --- | --- |
| Black | A+ |
| Green | A− |
| Red | B+ |
| Blue | B− |

Connect the driver's VCC/GND power input to the 24 V motor branch. Do not feed
24 V into the Arduino 5V pin. Although 24 V is the upper documented VIN limit
for the UNO R4, it leaves no margin for supply adjustment or transients; power
the Arduino by USB-C or a regulated buck supply instead.

## Mandatory power-safety checks

The photos show exposed mains and high-current DC terminals. Do not energize
that supply in the photographed state.

- Put the supply and terminals in a nonconductive or properly earthed enclosure.
- Verify the 115/230 V selector before connecting mains.
- Bond protective earth to the supply chassis and enclosure as applicable.
- Add an appropriately rated switch, strain relief, terminal cover, and a
  roughly 5 A fused motor branch.
- Add a reachable, physical motor-power cutoff; software `STOP` is not an
  emergency stop.
- Disconnect mains before touching wiring, changing DIP switches, or connecting
  and disconnecting the motor. Never hot-plug a stepper motor.

## What this hardware cannot observe

STEP/DIR commands report what the controller requested, not what the shaft did.
With the listed hardware, the firmware cannot detect a missed step, coupler
slip, the absolute dial index after reset, or whether a lock opened. Manual
`SETPOS` is therefore required after startup or an abort, and unattended search
is intentionally not enabled.

For reliable autonomous operation, add both:

1. Dial-side position feedback (preferably an absolute encoder, or at minimum a
   repeatable Hall/optical index).
2. Independent open detection, such as a handle/bolt contact or carefully
   limited torque/load sensing.

A TMC5160-class driver can provide tuned load/stall information, as in the
reference project, but that is a driver change rather than a firmware feature
the TB-style module can emulate.
