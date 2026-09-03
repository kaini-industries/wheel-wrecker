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

Original product links:

- Power supply: <https://www.amazon.com/dp/B0DSK3XN1M>
- Originally listed motor: <https://www.amazon.com/dp/B091C37FJ2>
- Driver: <https://www.amazon.com/dp/B0DSJ53J9N>

Not yet inventoried: the motor-to-dial coupler, gear/belt ratio (if not 1:1), a
dial index/home sensor, a shaft encoder, a physical emergency stop, or a sensor
that detects an opened lock.

## Driver settings

Switch power off before changing any DIP switch or motor connection. Use the
table printed on this exact driver rather than a table for another TB6600
clone.

The photographed driver's label gives these useful settings:

| Setting | Switches | Result |
| --- | --- | --- |
| 1/16 microstep | S1 OFF, S2 OFF, S3 ON | 3,200 pulses per motor revolution |
| 1/8 microstep | S1 OFF, S2 ON, S3 OFF | 1,600 pulses per motor revolution |
| 2.8 A row | S4 OFF, S5 OFF, S6 ON | Conservative ceiling near the photographed motor's 3 A rating |
| 3.0 A row | S4 OFF, S5 ON, S6 OFF | Do not begin testing here; use only if actually required under load |

Start at the 1.0–1.5 A row printed on the driver for uncoupled testing. Raise
current only enough to avoid missed steps under the real load, while watching
motor and driver temperature. A NEMA 23 can damage a dial or fixture long
before it reaches its rated torque, so use a compliant or torque-limiting
coupler.

The firmware default assumes 1/16 microstepping and direct 1:1 drive:

```text
pulses per dial revolution
  = 200 motor full-steps/revolution
  × 16 microsteps/full-step
  × 1 motor revolution/dial revolution
  = 3200

3200 pulses / 100 dial marks = 32 pulses per mark
```

Change `kStepsPerDialRevolution` in `include/HardwareConfig.h` if the physical
DIP setting or transmission ratio differs. The `SET SPR` serial command is a
runtime-only way to test a value.

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
`include/HardwareConfig.h`; do not try to correct an electrical polarity error
by changing the steps-per-revolution value.

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
