#include <Arduino.h>
#include <AccelStepper.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "DialMath.h"
#include "HardwareConfig.h"

using wheelwrecker::CombinationPlan;
using wheelwrecker::DialGeometry;
using wheelwrecker::DialMove;
using wheelwrecker::Direction;

namespace {

constexpr size_t kLineCapacity = 128;
constexpr size_t kMaxArguments = 10;
constexpr size_t kMoveQueueCapacity = wheelwrecker::kMaxCombinationWheels;
constexpr uint8_t kMaxSerialBytesPerLoop = 8;

AccelStepper stepper(AccelStepper::DRIVER,
                     hardware::kStepPin,
                     hardware::kDirectionPin,
                     0,
                     0,
                     false);
DialGeometry geometry(hardware::kStepsPerDialRevolution,
                      hardware::kDialUnitsPerRevolution);

bool armed = false;
bool positionKnown = false;
bool motorEnabled = false;
bool motorMoving = false;
bool directionPinInverted = hardware::kDirectionPinInverted;

float maxRevolutionsPerSecond = hardware::kMaxDialRevolutionsPerSecond;
float accelerationRevolutionsPerSecondSquared =
    hardware::kDialRevolutionsPerSecondSquared;
uint16_t targetSettleMillis = hardware::kTargetSettleMillis;
uint16_t idleDisableMillis = hardware::kIdleDisableMillis;

int32_t plannedDestinationStep = 0;
unsigned long settleDeadline = 0;
unsigned long disableDeadline = 0;
bool settleDeadlineActive = false;
bool disableDeadlineActive = false;

DialMove moveQueue[kMoveQueueCapacity];
size_t moveQueueCount = 0;
size_t moveQueueIndex = 0;
bool queueActive = false;
char queueName[24] = "";

char inputLine[kLineCapacity];
size_t inputLength = 0;
bool inputOverflow = false;

bool deadlinePending(unsigned long deadline) {
  return static_cast<long>(deadline - millis()) > 0;
}

void printPrompt() {
  Serial.print(F("> "));
}

void printBusyNonBlocking() {
  constexpr size_t kMessageLength = 5;
  if (Serial.availableForWrite() >= static_cast<int>(kMessageLength)) {
    Serial.print(F("BUSY\n"));
  }
}

void configureOutputLevel(uint8_t pin, bool outputHigh) {
  uint32_t configuration = IOPORT_CFG_PORT_DIRECTION_OUTPUT;
  if (outputHigh) {
    configuration |= IOPORT_CFG_PORT_OUTPUT_HIGH;
  }
  R_IOPORT_PinCfg(nullptr, digitalPinToBspPin(pin), configuration);
}

void setDriverEnabled(bool enabled) {
  if (!hardware::kEnablePinConnected) {
    motorEnabled = true;
    return;
  }

  const bool outputHigh =
      enabled ? hardware::kEnableOutputHigh : !hardware::kEnableOutputHigh;
  digitalWrite(hardware::kEnablePin, outputHigh ? HIGH : LOW);
  motorEnabled = enabled;
}

void applyMotionRates() {
  const float stepsPerRevolution =
      static_cast<float>(geometry.stepsPerRevolution());
  stepper.setMaxSpeed(maxRevolutionsPerSecond * stepsPerRevolution);
  stepper.setAcceleration(accelerationRevolutionsPerSecondSquared *
                          stepsPerRevolution);
}

void applyPinPolarity() {
  stepper.setPinsInverted(directionPinInverted,
                          hardware::kStepPulseInverted,
                          false);
  digitalWrite(hardware::kStepPin,
               hardware::kStepPulseInverted ? HIGH : LOW);
}

bool motionSettling() {
  if (!settleDeadlineActive) {
    return false;
  }
  if (!deadlinePending(settleDeadline)) {
    settleDeadlineActive = false;
    return false;
  }
  return true;
}

bool motionBusy() {
  return motorMoving || motionSettling();
}

int32_t currentWrappedStep() {
  return geometry.normalizeStep(stepper.currentPosition());
}

void rebaseAt(int32_t wrappedStep) {
  stepper.setCurrentPosition(geometry.normalizeStep(wrappedStep));
}

void cancelQueue() {
  queueActive = false;
  moveQueueCount = 0;
  moveQueueIndex = 0;
  queueName[0] = '\0';
}

void immediateStop(const __FlashStringHelper* reason) {
  stepper.setCurrentPosition(stepper.currentPosition());
  motorMoving = false;
  settleDeadlineActive = false;
  disableDeadlineActive = false;
  cancelQueue();
  setDriverEnabled(false);
  armed = false;
  positionKnown = false;
  digitalWrite(LED_BUILTIN, LOW);
  Serial.print(F("STOPPED: "));
  Serial.println(reason);
  if (!hardware::kEnablePinConnected) {
    Serial.println(F("WARNING: ENA is not controlled; motor torque may remain"));
  }
  Serial.println(F("Position is unknown. Realign the dial and use SETPOS before moving."));
}

bool startMotorMove(const DialMove& move) {
  if (motionBusy() || motorMoving) {
    return false;
  }

  plannedDestinationStep = move.destinationStep;
  if (move.deltaSteps == 0) {
    rebaseAt(plannedDestinationStep);
    return true;
  }

  const bool positive = move.deltaSteps > 0;
  const bool directionHigh = positive != directionPinInverted;
  digitalWrite(hardware::kDirectionPin, directionHigh ? HIGH : LOW);

  if (!motorEnabled) {
    setDriverEnabled(true);
    delayMicroseconds(hardware::kDriverEnableSettleMicros);
  }
  delayMicroseconds(hardware::kDirectionSetupMicros);

  const long moveSteps = static_cast<long>(move.deltaSteps);
  stepper.move(moveSteps);
  motorMoving = true;
  settleDeadlineActive = false;
  disableDeadlineActive = false;

  Serial.print(F("MOVE "));
  Serial.print(wheelwrecker::directionName(move.direction));
  Serial.print(F("  steps="));
  Serial.print(moveSteps < 0 ? -moveSteps : moveSteps);
  Serial.print(F("  target="));
  Serial.print(move.targetMark, 4);
  if (move.passesBeforeStop > 0) {
    Serial.print(F("  passes="));
    Serial.print(move.passesBeforeStop);
  }
  Serial.println();
  return true;
}

void serviceMotor() {
  if (motorMoving) {
    stepper.run();
    if (stepper.distanceToGo() == 0) {
      motorMoving = false;
      rebaseAt(plannedDestinationStep);
      settleDeadline = millis() + targetSettleMillis;
      settleDeadlineActive = targetSettleMillis > 0;
      disableDeadline = millis() + idleDisableMillis;
      disableDeadlineActive = true;
    }
  }

  motionSettling();

  if (motorEnabled && !motorMoving && !queueActive && !motionSettling() &&
      disableDeadlineActive && !deadlinePending(disableDeadline)) {
    setDriverEnabled(false);
    disableDeadlineActive = false;
    if (positionKnown) {
      positionKnown = false;
      if (hardware::kEnablePinConnected) {
        Serial.println(F("DRIVER OFF: dial reference invalidated; use SETPOS before moving"));
      } else {
        Serial.println(F("HOLD EXPIRED: reference invalidated; ENA is not controlled"));
      }
      printPrompt();
    }
  }
}

bool startQueue(const DialMove* moves, size_t count, const char* name) {
  if (moves == nullptr || count == 0 || count > kMoveQueueCapacity ||
      motionBusy() || queueActive) {
    return false;
  }

  for (size_t index = 0; index < count; ++index) {
    moveQueue[index] = moves[index];
  }
  moveQueueCount = count;
  moveQueueIndex = 0;
  queueActive = true;
  strncpy(queueName, name, sizeof(queueName) - 1);
  queueName[sizeof(queueName) - 1] = '\0';

  Serial.print(F("START "));
  Serial.print(queueName);
  Serial.print(F(" ("));
  Serial.print(count);
  Serial.println(F(" segment(s))"));
  return true;
}

void serviceQueue() {
  if (!queueActive || motionBusy()) {
    return;
  }

  if (moveQueueIndex < moveQueueCount) {
    if (!startMotorMove(moveQueue[moveQueueIndex])) {
      immediateStop(F("could not start queued motion"));
      return;
    }
    ++moveQueueIndex;
    return;
  }

  Serial.print(F("DONE "));
  Serial.print(queueName);
  Serial.print(F("  dial="));
  Serial.println(geometry.stepToMark(currentWrappedStep()), 4);
  cancelQueue();
  disableDeadline = millis() + idleDisableMillis;
  disableDeadlineActive = true;
  printPrompt();
}

bool parseFloat(const char* text, float& value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const float parsed = strtof(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parseLong(const char* text, long& value) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

bool parseDirection(const char* text, Direction& direction) {
  if (strcmp(text, "L") == 0 || strcmp(text, "LEFT") == 0 ||
      strcmp(text, "CCW") == 0) {
    direction = Direction::Left;
    return true;
  }
  if (strcmp(text, "R") == 0 || strcmp(text, "RIGHT") == 0 ||
      strcmp(text, "CW") == 0) {
    direction = Direction::Right;
    return true;
  }
  return false;
}

bool parseOnOff(const char* text, bool& value) {
  if (strcmp(text, "ON") == 0 || strcmp(text, "TRUE") == 0 ||
      strcmp(text, "1") == 0) {
    value = true;
    return true;
  }
  if (strcmp(text, "OFF") == 0 || strcmp(text, "FALSE") == 0 ||
      strcmp(text, "0") == 0) {
    value = false;
    return true;
  }
  return false;
}

bool validDialMark(float mark) {
  return mark >= 0.0f && mark < geometry.dialUnitsPerRevolution();
}

void uppercase(char* text) {
  while (*text != '\0') {
    *text = static_cast<char>(toupper(static_cast<unsigned char>(*text)));
    ++text;
  }
}

size_t tokenize(char* line, char* arguments[], size_t capacity) {
  size_t count = 0;
  char* save = nullptr;
  char* token = strtok_r(line, " \t", &save);
  while (token != nullptr && count < capacity) {
    uppercase(token);
    arguments[count++] = token;
    token = strtok_r(nullptr, " \t", &save);
  }
  return count;
}

bool motionCommandAllowed() {
  if (!armed) {
    Serial.println(F("ERR: motion is disarmed; use ARM after completing safety checks"));
    return false;
  }
  if (!positionKnown) {
    Serial.println(F("ERR: dial position is unknown; align it and use SETPOS <mark>"));
    return false;
  }
  if (queueActive || motionBusy()) {
    Serial.println(F("ERR: motion is already active; use STATUS or STOP"));
    return false;
  }
  return true;
}

void printHelp() {
  Serial.println(F("Commands (newline terminated):"));
  Serial.println(F("  STATUS"));
  Serial.println(F("  ARM | DISARM | STOP   (Ctrl-C also stops immediately)"));
  Serial.println(F("  SETPOS <mark>         align physical dial, then set its mark"));
  Serial.println(F("  GOTO <L|R> <mark> [passes-before-stop]"));
  Serial.println(F("  JOG <L|R> <dial-units>"));
  Serial.println(F("  TURN <L|R> <revolutions>"));
  Serial.println(F("  COMBO <L|R> <n1> <n2> ... <n5>"));
  Serial.println(F("  SET SPR <steps/rev>    runtime only; must match driver/coupling"));
  Serial.println(F("  SET SPEED <rev/sec> | SET ACCEL <rev/sec^2>"));
  Serial.println(F("  SET REVERSE <ON|OFF> | SET SETTLE <ms> | SET HOLD <ms>"));
  Serial.println(F("  CAL SCALE <commanded-revs> <observed-revs>"));
  Serial.println(F("LEFT increases printed numbers; RIGHT decreases them."));
}

void printStatus() {
  Serial.print(F("state="));
  if (motorMoving) {
    Serial.print(F("MOVING"));
  } else if (motionSettling()) {
    Serial.print(F("SETTLING"));
  } else {
    Serial.print(F("IDLE"));
  }
  Serial.print(F(" armed="));
  Serial.print(armed ? F("YES") : F("NO"));
  Serial.print(F(" driver="));
  Serial.print(motorEnabled ? F("ON") : F("OFF"));
  Serial.print(F(" position="));
  if (positionKnown) {
    Serial.print(geometry.stepToMark(currentWrappedStep()), 4);
    Serial.print(F(" mark (tick "));
    Serial.print(currentWrappedStep());
    Serial.print(')');
  } else {
    Serial.print(F("UNKNOWN"));
  }
  Serial.println();

  Serial.print(F("SPR="));
  Serial.print(geometry.stepsPerRevolution());
  Serial.print(F(" resolution="));
  Serial.print(geometry.unitsPerStep(), 6);
  Serial.print(F(" mark/step speed="));
  Serial.print(maxRevolutionsPerSecond, 3);
  Serial.print(F(" rev/s accel="));
  Serial.print(accelerationRevolutionsPerSecondSquared, 3);
  Serial.print(F(" rev/s^2 reverse="));
  Serial.println(directionPinInverted ? F("ON") : F("OFF"));
}

void handleSet(char* arguments[], size_t count) {
  if (count != 3) {
    Serial.println(F("ERR: SET expects a setting and one value"));
    return;
  }
  if (queueActive || motionBusy()) {
    Serial.println(F("ERR: configuration cannot change during motion"));
    return;
  }

  if (strcmp(arguments[1], "SPR") == 0) {
    long value = 0;
    if (!parseLong(arguments[2], value) || value < 200 || value > 200000) {
      Serial.println(F("ERR: SPR must be 200..200000 steps per dial revolution"));
      return;
    }
    if (maxRevolutionsPerSecond * static_cast<float>(value) >
        hardware::kMaximumStepRate) {
      Serial.println(F("ERR: current speed times SPR exceeds the 10000 step/s limit"));
      return;
    }
    geometry.configure(static_cast<int32_t>(value),
                       hardware::kDialUnitsPerRevolution);
    rebaseAt(0);
    positionKnown = false;
    applyMotionRates();
    Serial.print(F("OK: SPR="));
    Serial.print(value);
    Serial.println(F("; position is now unknown (runtime setting only)"));
    return;
  }

  if (strcmp(arguments[1], "SPEED") == 0) {
    float value = 0.0f;
    if (!parseFloat(arguments[2], value) || value < 0.02f || value > 2.0f) {
      Serial.println(F("ERR: SPEED must be 0.02..2.0 dial rev/s"));
      return;
    }
    if (value * geometry.stepsPerRevolution() > hardware::kMaximumStepRate) {
      Serial.println(F("ERR: speed times SPR exceeds the 10000 step/s limit"));
      return;
    }
    maxRevolutionsPerSecond = value;
    applyMotionRates();
    Serial.println(F("OK: speed updated (runtime only)"));
    return;
  }

  if (strcmp(arguments[1], "ACCEL") == 0) {
    float value = 0.0f;
    if (!parseFloat(arguments[2], value) || value < 0.02f || value > 10.0f) {
      Serial.println(F("ERR: ACCEL must be 0.02..10.0 dial rev/s^2"));
      return;
    }
    accelerationRevolutionsPerSecondSquared = value;
    applyMotionRates();
    Serial.println(F("OK: acceleration updated (runtime only)"));
    return;
  }

  if (strcmp(arguments[1], "REVERSE") == 0) {
    bool value = false;
    if (!parseOnOff(arguments[2], value)) {
      Serial.println(F("ERR: REVERSE expects ON or OFF"));
      return;
    }
    directionPinInverted = value;
    applyPinPolarity();
    positionKnown = false;
    Serial.println(F("OK: direction polarity changed; use SETPOS before moving"));
    return;
  }

  if (strcmp(arguments[1], "SETTLE") == 0 ||
      strcmp(arguments[1], "HOLD") == 0) {
    long value = 0;
    if (!parseLong(arguments[2], value) || value < 0 || value > 60000) {
      Serial.println(F("ERR: time must be 0..60000 ms"));
      return;
    }
    if (strcmp(arguments[1], "SETTLE") == 0) {
      targetSettleMillis = static_cast<uint16_t>(value);
    } else {
      idleDisableMillis = static_cast<uint16_t>(value);
    }
    Serial.println(F("OK: timing updated (runtime only)"));
    return;
  }

  Serial.println(F("ERR: unknown SET option"));
}

void handleCalibration(char* arguments[], size_t count) {
  if (count != 4 || strcmp(arguments[1], "SCALE") != 0) {
    Serial.println(F("ERR: use CAL SCALE <commanded-revs> <observed-revs>"));
    return;
  }
  if (queueActive || motionBusy()) {
    Serial.println(F("ERR: calibration cannot change during motion"));
    return;
  }

  float commanded = 0.0f;
  float observed = 0.0f;
  if (!parseFloat(arguments[2], commanded) ||
      !parseFloat(arguments[3], observed) || commanded <= 0.0f ||
      observed <= 0.0f) {
    Serial.println(F("ERR: commanded and observed revolutions must be positive"));
    return;
  }

  const double scaled = static_cast<double>(geometry.stepsPerRevolution()) *
                        static_cast<double>(commanded) /
                        static_cast<double>(observed);
  if (!isfinite(scaled) || scaled < 200.0 || scaled > 200000.0) {
    Serial.println(F("ERR: calculated SPR is outside 200..200000"));
    return;
  }
  const long newSteps = static_cast<long>(scaled + 0.5);
  if (maxRevolutionsPerSecond * static_cast<float>(newSteps) >
      hardware::kMaximumStepRate) {
    Serial.println(F("ERR: calculated SPR exceeds the step-rate limit at current speed"));
    return;
  }

  geometry.configure(static_cast<int32_t>(newSteps),
                     hardware::kDialUnitsPerRevolution);
  rebaseAt(0);
  positionKnown = false;
  applyMotionRates();
  Serial.print(F("OK: calculated SPR="));
  Serial.print(newSteps);
  Serial.println(F("; verify mechanically and use SETPOS (runtime only)"));
}

void handleLine(char* line) {
  char* arguments[kMaxArguments];
  const size_t count = tokenize(line, arguments, kMaxArguments);
  if (count == 0) {
    if (!queueActive && !motionBusy()) {
      printPrompt();
    }
    return;
  }

  if (strcmp(arguments[0], "STOP") == 0) {
    immediateStop(F("operator request"));
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "DISARM") == 0) {
    if (queueActive || motionBusy()) {
      immediateStop(F("operator disarm"));
    } else {
      cancelQueue();
      setDriverEnabled(false);
      armed = false;
      positionKnown = false;
      settleDeadlineActive = false;
      disableDeadlineActive = false;
      digitalWrite(LED_BUILTIN, LOW);
      if (hardware::kEnablePinConnected) {
        Serial.println(F("DISARMED: driver offline requested; position is unknown"));
      } else {
        Serial.println(F("DISARMED: pulses stopped, but ENA is uncontrolled; position is unknown"));
      }
    }
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "STATUS") == 0) {
    if (queueActive || motionBusy()) {
      printBusyNonBlocking();
    } else {
      printStatus();
      printPrompt();
    }
    return;
  }

  if (queueActive || motionBusy()) {
    printBusyNonBlocking();
    return;
  }

  if (strcmp(arguments[0], "HELP") == 0 || strcmp(arguments[0], "?") == 0) {
    printHelp();
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "ARM") == 0) {
    armed = true;
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println(F("ARMED: motion commands are enabled; STOP/Ctrl-C aborts"));
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "SETPOS") == 0) {
    float mark = 0.0f;
    if (count != 2 || !parseFloat(arguments[1], mark) ||
        !validDialMark(mark)) {
      Serial.println(F("ERR: SETPOS mark must be in the range 0..99.999"));
    } else {
      const int32_t step = geometry.markToStep(mark);
      rebaseAt(step);
      plannedDestinationStep = step;
      positionKnown = true;
      Serial.print(F("OK: dial reference="));
      Serial.print(geometry.stepToMark(step), 4);
      Serial.print(F(" (tick "));
      Serial.print(step);
      Serial.println(')');
    }
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "SET") == 0) {
    handleSet(arguments, count);
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "CAL") == 0) {
    handleCalibration(arguments, count);
    printPrompt();
    return;
  }

  if (!motionCommandAllowed()) {
    printPrompt();
    return;
  }

  if (strcmp(arguments[0], "GOTO") == 0) {
    Direction direction;
    float mark = 0.0f;
    long passes = 0;
    if ((count != 3 && count != 4) ||
        !parseDirection(arguments[1], direction) ||
        !parseFloat(arguments[2], mark) || !validDialMark(mark) ||
        (count == 4 && !parseLong(arguments[3], passes)) || passes < 0 ||
        passes > hardware::kMaximumPasses) {
      Serial.println(F("ERR: use GOTO <L|R> <mark 0..99.999> [passes 0..10]"));
      printPrompt();
      return;
    }
    const DialMove move = geometry.planToMark(
        currentWrappedStep(), mark, direction, static_cast<uint8_t>(passes));
    startQueue(&move, 1, "GOTO");
    return;
  }

  if (strcmp(arguments[0], "JOG") == 0 ||
      strcmp(arguments[0], "TURN") == 0) {
    Direction direction;
    float amount = 0.0f;
    if (count != 3 || !parseDirection(arguments[1], direction) ||
        !parseFloat(arguments[2], amount) || amount <= 0.0f) {
      Serial.println(F("ERR: use JOG <L|R> <units> or TURN <L|R> <revolutions>"));
      printPrompt();
      return;
    }

    float dialUnits = amount;
    if (strcmp(arguments[0], "TURN") == 0) {
      if (amount > hardware::kMaximumRelativeRevolutions) {
        Serial.println(F("ERR: TURN exceeds the 20-revolution command limit"));
        printPrompt();
        return;
      }
      dialUnits *= geometry.dialUnitsPerRevolution();
    } else if (amount > hardware::kMaximumRelativeRevolutions *
                            geometry.dialUnitsPerRevolution()) {
      Serial.println(F("ERR: JOG exceeds the 20-revolution command limit"));
      printPrompt();
      return;
    }

    const DialMove move =
        geometry.planRelative(currentWrappedStep(), dialUnits, direction);
    startQueue(&move, 1, arguments[0]);
    return;
  }

  if (strcmp(arguments[0], "COMBO") == 0) {
    if (count < 4 || count > wheelwrecker::kMaxCombinationWheels + 2) {
      Serial.println(F("ERR: use COMBO <L|R> <n1> <n2> [n3 ... n5]"));
      printPrompt();
      return;
    }

    Direction firstDirection;
    if (!parseDirection(arguments[1], firstDirection)) {
      Serial.println(F("ERR: combination direction must be L or R"));
      printPrompt();
      return;
    }

    float marks[wheelwrecker::kMaxCombinationWheels];
    const size_t markCount = count - 2;
    for (size_t index = 0; index < markCount; ++index) {
      if (!parseFloat(arguments[index + 2], marks[index]) ||
          !validDialMark(marks[index])) {
        Serial.println(F("ERR: every combination mark must be in 0..99.999"));
        printPrompt();
        return;
      }
    }

    CombinationPlan plan;
    if (!wheelwrecker::buildCombinationPlan(
            geometry, currentWrappedStep(), firstDirection, marks, markCount,
            plan) || !startQueue(plan.moves, plan.count, "COMBINATION")) {
      Serial.println(F("ERR: could not create combination plan"));
      printPrompt();
    }
    return;
  }

  Serial.println(F("ERR: unknown command; use HELP"));
  printPrompt();
}

void serviceSerial() {
  uint8_t processed = 0;
  while (Serial.available() > 0 && processed < kMaxSerialBytesPerLoop) {
    ++processed;
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == 0x03) {
      inputLength = 0;
      inputOverflow = false;
      immediateStop(F("Ctrl-C"));
      printPrompt();
      continue;
    }
    if (incoming == '\r') {
      continue;
    }
    if (incoming == '\n') {
      if (inputOverflow) {
        Serial.println(F("ERR: command line too long"));
        printPrompt();
      } else {
        inputLine[inputLength] = '\0';
        handleLine(inputLine);
      }
      inputLength = 0;
      inputOverflow = false;
      continue;
    }

    if (inputLength + 1 < kLineCapacity) {
      inputLine[inputLength++] = incoming;
    } else {
      inputOverflow = true;
    }
  }
}

}  // namespace

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // The UNO R4 core's pinMode(OUTPUT) initially drives LOW regardless of a
  // preceding digitalWrite. Configure the output level and direction together
  // so an active-low STEP cannot glitch during setup.
  if (hardware::kEnablePinConnected) {
    const bool disabledHigh = !hardware::kEnableOutputHigh;
    configureOutputLevel(hardware::kEnablePin, disabledHigh);
    delayMicroseconds(hardware::kDriverDisableSettleMicros);
  }

  configureOutputLevel(hardware::kStepPin, hardware::kStepPulseInverted);
  configureOutputLevel(hardware::kDirectionPin, false);
  applyPinPolarity();
  setDriverEnabled(false);
  stepper.setMinPulseWidth(hardware::kMinimumStepPulseMicros);
  applyMotionRates();
  rebaseAt(0);

  Serial.begin(115200);
  const unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 1500) {
    // Bounded wait: the motor remains disabled even without a serial host.
  }

  Serial.println();
  Serial.println(F("Wheel Wrecker precision dial controller"));
  Serial.println(F("BOOT: no pulses scheduled, motion disarmed, position unknown"));
  if (hardware::kEnablePinConnected) {
    Serial.println(F("ENA offline requested; reset-time safety also requires a hardware pull-down"));
  } else {
    Serial.println(F("WARNING: ENA is not controlled; motor torque cannot be removed in software"));
  }
  Serial.println(F("Use HELP. Verify wiring/DIP settings before ARM."));
  printStatus();
  printPrompt();
}

void loop() {
  serviceMotor();
  serviceSerial();
  serviceMotor();
  serviceQueue();
}
