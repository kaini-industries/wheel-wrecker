#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wheelwrecker {

// Safe-dial convention: LEFT increases the printed dial number and RIGHT
// decreases it. Hardware pin polarity is configured separately.
enum class Direction : uint8_t {
  Left,
  Right,
};

Direction opposite(Direction direction);
const char* directionName(Direction direction);

struct DialMove {
  Direction direction;
  int64_t deltaSteps;
  int32_t destinationStep;
  float targetMark;
  uint8_t passesBeforeStop;
};

class DialGeometry {
 public:
  DialGeometry(int32_t stepsPerRevolution = 3200,
               float dialUnitsPerRevolution = 100.0f);

  bool configure(int32_t stepsPerRevolution,
                 float dialUnitsPerRevolution = 100.0f);

  int32_t stepsPerRevolution() const;
  float dialUnitsPerRevolution() const;
  float unitsPerStep() const;

  float normalizeMark(float mark) const;
  int32_t normalizeStep(int64_t step) const;
  int32_t markToStep(float mark) const;
  float stepToMark(int64_t step) const;

  // Plan a directional move to a printed dial mark. passesBeforeStop is the
  // number of times the target mark must pass the index before stopping on it.
  // This is deliberately not a shortest-path operation. A non-finite target
  // produces a zero-length move at the current position.
  DialMove planToMark(int32_t currentStep,
                      float targetMark,
                      Direction direction,
                      uint8_t passesBeforeStop = 0) const;

  // Plan a directed relative move in dial units (100 units is one revolution
  // with the default geometry). Non-finite or out-of-range input produces a
  // zero-length move at the current position.
  DialMove planRelative(int32_t currentStep,
                        float dialUnits,
                        Direction direction) const;

 private:
  int32_t stepsPerRevolution_;
  float dialUnitsPerRevolution_;
};

constexpr size_t kMaxCombinationWheels = 5;

struct CombinationPlan {
  DialMove moves[kMaxCombinationWheels];
  size_t count;
  int32_t destinationStep;
};

// Build a conservative full-pickup sequence. For a three-wheel lock this
// stops on the targets on their 4th, 3rd, and 2nd arrivals (three, two, then
// one pass before stopping). Non-finite marks are rejected.
bool buildCombinationPlan(const DialGeometry& geometry,
                          int32_t currentStep,
                          Direction firstDirection,
                          const float* marks,
                          size_t markCount,
                          CombinationPlan& plan);

}  // namespace wheelwrecker
