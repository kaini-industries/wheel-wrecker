#pragma once

#include <Arduino.h>

namespace hardware {

constexpr uint8_t kStepPin = 2;
constexpr uint8_t kDirectionPin = 3;
constexpr uint8_t kEnablePin = 4;

// Optional 128x64 SSD1306 status display on the UNO R4's main I2C bus:
// SDA/A4 and SCL/A5. It is informational and never authorizes motion.
constexpr bool kOledEnabled = true;
constexpr uint8_t kOledWidth = 128;
constexpr uint8_t kOledHeight = 64;
constexpr uint8_t kOledPrimaryAddress = 0x3C;
constexpr uint8_t kOledAlternateAddress = 0x3D;
constexpr uint32_t kOledI2cClock = 400000;
constexpr unsigned int kOledI2cTimeoutMicros = 25000;
constexpr uint16_t kOledStartupDelayMillis = 50;

// The photographed wiring appears to use common-anode STEP/DIR inputs:
// PUL+/DIR+ -> +5 V and the Arduino sinks PUL-/DIR-. Confirm continuity.
constexpr bool kStepPulseInverted = true;

// Positive motion represents LEFT/increasing dial numbers. Flip this after
// the unloaded direction test if positive motion turns the dial right.
constexpr bool kDirectionPinInverted = false;

// For the common-anode ENA wiring described in HARDWARE.md, HIGH at D4 removes
// the TB-style driver's active-low "offline" request and energizes the motor.
// Set kEnablePinConnected false if ENA is intentionally left disconnected.
constexpr bool kEnablePinConnected = true;
constexpr bool kEnableOutputHigh = true;

// 1.8-degree motor (200 full steps), 1/16 driver setting, direct 1:1 coupling.
// This must match the physical DIP setting and transmission ratio.
constexpr int32_t kStepsPerDialRevolution = 3200;
constexpr float kDialUnitsPerRevolution = 100.0f;

// Conservative bring-up rates in physical dial units. Changing microsteps
// does not silently change dial speed.
constexpr float kMaxDialRevolutionsPerSecond = 0.30f;
constexpr float kDialRevolutionsPerSecondSquared = 0.50f;
// Conservative software ceiling for AccelStepper plus digitalWrite on the R4.
// The mechanical/load limit will normally be much lower.
constexpr float kMaximumStepRate = 10000.0f;
constexpr unsigned int kMinimumStepPulseMicros = 5;
constexpr unsigned int kDirectionSetupMicros = 10;
constexpr unsigned int kDriverEnableSettleMicros = 1000;
constexpr unsigned int kDriverDisableSettleMicros = 1000;
constexpr uint16_t kTargetSettleMillis = 150;
constexpr uint16_t kIdleDisableMillis = 2000;

constexpr uint8_t kMaximumPasses = 10;
constexpr float kMaximumRelativeRevolutions = 20.0f;

static_assert(kStepPin != kDirectionPin && kStepPin != kEnablePin &&
                  kDirectionPin != kEnablePin,
              "STEP, DIR, and ENA pins must be distinct");
static_assert(kStepsPerDialRevolution > 0,
              "Steps per dial revolution must be positive");
static_assert(kMaxDialRevolutionsPerSecond * kStepsPerDialRevolution <=
                  kMaximumStepRate,
              "Default motion rate exceeds the configured step-rate limit");

}  // namespace hardware
