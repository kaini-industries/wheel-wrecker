# Firmware architecture and invariants

## Source and build model

`arduino/WheelWrecker` is the single canonical firmware source tree. The
`WheelWrecker.ino` entry point contains only Arduino's `setup()` and `loop()`
adapters; they delegate to `wheelWreckerSetup()` and `wheelWreckerLoop()` in
`Firmware.cpp`. Controller logic therefore remains ordinary C++ instead of
being duplicated in an IDE-specific sketch.

Arduino IDE, the pinned Arduino CLI profile in `sketch.yaml`, and PlatformIO
all compile these same files. `DialMath.cpp` is also compiled into the native
host tests. A change must not be copied between build-system directories: if a
source file exists outside the canonical sketch, it is test or build support,
not another firmware implementation.

The sketch rejects non-UNO-R4-WiFi targets at compile time. This matters because
the pin behavior, memory budget, and main `Wire` bus assumptions are specific
to the UNO R4 WiFi, not the classic AVR Uno or another Arduino board.

## Coordinate model

`DialGeometry` is independent of Arduino and treats one dial revolution as an
integer ring of microstep ticks. With the default configuration:

```text
0..99 printed marks <-> 0..3199 ticks
LEFT  = positive/increasing ticks
RIGHT = negative/decreasing ticks
```

Hardware direction polarity is kept outside this model. Reversing a motor phase
or flipping a DIR signal must not change dial arithmetic or tests.

Every target mark is rounded once to its nearest tick. Moves are then calculated
between integer endpoints. This is why a sequence such as
`0 -> 33.33 -> 66.66 -> 0` totals exactly 3200 ticks instead of accumulating a
fractional conversion error on each leg.

A target move has three explicit inputs:

1. Direction.
2. Printed target mark.
3. Number of target passes before the final stop.

If motion starts on the target and passes were requested, the first post-start
arrival is one full revolution away. That rule prevents a conventional
fourth/third/second-arrival sequence from becoming a turn short when adjacent
combination numbers happen to match.

AccelStepper's internal coordinate is rebased to the wrapped destination only
after each stopped segment. Multi-turn distances remain explicit signed
relative moves while long-running sessions cannot drift toward a 32-bit
position overflow.

## Runtime state

Motion requires two independent conditions: the operator has issued `ARM`, and
the physical dial has been declared with `SETPOS`. Neither condition moves the
motor.

Normal command flow is:

```text
boot/disarmed/unknown
  -> SETPOS (referenced)
  -> ARM (motion permitted)
  -> queued directed segment(s)
  -> target settling dwell
  -> HOLD timeout
  -> ENA offline + reference invalidated
```

`STOP`, Ctrl-C, and `DISARM` cancel the queue and invalidate position. `STOP`
also resets AccelStepper's target to its current commanded step before ENA is
released. This is a software stop, not a safety-rated emergency stop.

The fixed-size line parser and move queue do not allocate heap memory. Serial
input is bounded to eight bytes per loop pass and motor service runs on both
sides of it, preventing a receive burst from monopolizing pulse generation.
Verbose help/status output is suppressed during motion; `STATUS` returns only a
short nonblocking `BUSY`. AccelStepper `run()` replaces the original blocking
`runToPosition()` calls.

## Display isolation

The SSD1306 is an optional observer of controller state. It cannot arm motion,
provide a position reference, or report successful opening. Startup probes
`0x3C` and `0x3D`; absence leaves the serial and motion paths operational. A
runtime NACK latches the display in `FAULT` until the operator disarms and
issues `OLED RETRY`, avoiding continuous recovery traffic on a broken bus. The
retry restarts the UNO R4 I²C peripheral before probing again.

An SSD1306 full-frame I²C transfer is long compared with the interval between
STEP pulses. `serviceDisplay()` therefore refuses to transfer while a motor
segment or its settling dwell is active. The loop services the motor, accepts a
bounded amount of serial input, services the motor again, paints a pending
stationary frame, and then checks serial and motor state again before the next
queued segment may start. This displays the upcoming target without allowing
screen traffic to interrupt pulse timing or delaying a stop until after a new
segment begins.

The status snapshot explicitly calls position `CMD REF`: it is the commanded
open-loop coordinate described below. ENA text is also a request, not feedback
from the driver. The Adafruit library allocates a 1,024-byte 128×64 framebuffer
only after a responding OLED has been found; the motion queue and serial parser
remain fixed-size static buffers.

The UNO R4 core's ordinary `pinMode(pin, OUTPUT)` initially selects a LOW output
level. Startup therefore configures each Renesas GPIO's direction and safe level
in one operation: ENA is made offline first, then active-low STEP is made idle.
The external ENA fail-safe described in `HARDWARE.md` still covers reset and
unpowered high-impedance time before firmware executes.

## What “position” means today

The current value is commanded open-loop position. It remains valid only if all
pulses caused real motion and the coupler did not move while torque was absent.
The controller invalidates the reference whenever it deliberately releases
holding torque, but it cannot observe a stall or slip while energized.

An encoder/index integration should expose at least:

- `referenceAvailable()` — whether an absolute/index reference has been found.
- `measuredTick()` — dial-side position, after transmission ratio.
- `followingErrorTicks()` — commanded minus measured position.
- A latched fault that immediately stops pulse generation and invalidates the
  reference when following error exceeds a speed-dependent limit.

Open detection is a separate signal. An encoder may reveal a stall, but does not
by itself distinguish a valid opening stop from a jam, collision, or slipping
coupler.

## Why there is no unattended search loop

A search engine can generate candidate numbers, but this hardware cannot decide
whether an attempt opened the lock and cannot prove that the dial reached each
candidate. Automatically advancing after an unobserved stall would corrupt all
later wheel positions and could damage the lock. Search, checkpoint/resume, and
travel-minimizing wheel-state optimization belong after measured position,
open-state sensing, and a physical normally closed stop are integrated and
tested on a known-combination fixture.
