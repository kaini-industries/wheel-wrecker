#include "DialMath.h"

#include <math.h>

namespace wheelwrecker {

namespace {

int32_t roundToInt32(double value) {
  return static_cast<int32_t>(value >= 0.0 ? value + 0.5 : value - 0.5);
}

}  // namespace

Direction opposite(Direction direction) {
  return direction == Direction::Left ? Direction::Right : Direction::Left;
}

const char* directionName(Direction direction) {
  return direction == Direction::Left ? "LEFT" : "RIGHT";
}

DialGeometry::DialGeometry(int32_t stepsPerRevolution,
                           float dialUnitsPerRevolution)
    : stepsPerRevolution_(3200), dialUnitsPerRevolution_(100.0f) {
  configure(stepsPerRevolution, dialUnitsPerRevolution);
}

bool DialGeometry::configure(int32_t stepsPerRevolution,
                             float dialUnitsPerRevolution) {
  if (stepsPerRevolution <= 0 || !isfinite(dialUnitsPerRevolution) ||
      dialUnitsPerRevolution <= 0.0f) {
    return false;
  }

  stepsPerRevolution_ = stepsPerRevolution;
  dialUnitsPerRevolution_ = dialUnitsPerRevolution;
  return true;
}

int32_t DialGeometry::stepsPerRevolution() const {
  return stepsPerRevolution_;
}

float DialGeometry::dialUnitsPerRevolution() const {
  return dialUnitsPerRevolution_;
}

float DialGeometry::unitsPerStep() const {
  return dialUnitsPerRevolution_ / static_cast<float>(stepsPerRevolution_);
}

float DialGeometry::normalizeMark(float mark) const {
  if (!isfinite(mark)) {
    return 0.0f;
  }
  float normalized = fmodf(mark, dialUnitsPerRevolution_);
  if (normalized < 0.0f) {
    normalized += dialUnitsPerRevolution_;
  }
  if (normalized >= dialUnitsPerRevolution_) {
    normalized = 0.0f;
  }
  return normalized;
}

int32_t DialGeometry::normalizeStep(int64_t step) const {
  int64_t normalized = step % stepsPerRevolution_;
  if (normalized < 0) {
    normalized += stepsPerRevolution_;
  }
  return static_cast<int32_t>(normalized);
}

int32_t DialGeometry::markToStep(float mark) const {
  const double normalized = normalizeMark(mark);
  const double scaled = normalized * static_cast<double>(stepsPerRevolution_) /
                        static_cast<double>(dialUnitsPerRevolution_);
  return normalizeStep(roundToInt32(scaled));
}

float DialGeometry::stepToMark(int64_t step) const {
  return static_cast<float>(normalizeStep(step)) * dialUnitsPerRevolution_ /
         static_cast<float>(stepsPerRevolution_);
}

DialMove DialGeometry::planToMark(int32_t currentStep,
                                  float targetMark,
                                  Direction direction,
                                  uint8_t passesBeforeStop) const {
  const int32_t current = normalizeStep(currentStep);
  if (!isfinite(targetMark)) {
    DialMove noMove = {
        direction, 0, current, stepToMark(current), passesBeforeStop};
    return noMove;
  }
  const int32_t destination = markToStep(targetMark);

  int32_t firstArrival = 0;
  if (direction == Direction::Left) {
    firstArrival = normalizeStep(static_cast<int64_t>(destination) - current);
  } else {
    firstArrival = normalizeStep(static_cast<int64_t>(current) - destination);
  }

  // If already at the target, its first post-start arrival is one revolution
  // away. Do not silently shorten a requested 4/3/2-arrival lock sequence.
  if (firstArrival == 0 && passesBeforeStop > 0) {
    firstArrival = stepsPerRevolution_;
  }

  const int64_t magnitude =
      static_cast<int64_t>(firstArrival) +
      static_cast<int64_t>(passesBeforeStop) * stepsPerRevolution_;
  const int64_t signedDelta =
      direction == Direction::Left ? magnitude : -magnitude;

  DialMove move = {direction,
                   signedDelta,
                   destination,
                   stepToMark(destination),
                   passesBeforeStop};
  return move;
}

DialMove DialGeometry::planRelative(int32_t currentStep,
                                    float dialUnits,
                                    Direction direction) const {
  const int32_t current = normalizeStep(currentStep);
  if (!isfinite(dialUnits)) {
    DialMove noMove = {direction, 0, current, stepToMark(current), 0};
    return noMove;
  }
  const double scaled =
      static_cast<double>(dialUnits) * stepsPerRevolution_ /
      static_cast<double>(dialUnitsPerRevolution_);
  const double absoluteScaled = fabs(scaled);
  if (!isfinite(absoluteScaled) ||
      absoluteScaled > static_cast<double>(INT32_MAX) - 0.5) {
    DialMove noMove = {direction, 0, current, stepToMark(current), 0};
    return noMove;
  }
  const int64_t magnitude = static_cast<int64_t>(absoluteScaled + 0.5);

  const int64_t signedDelta =
      direction == Direction::Left ? magnitude : -magnitude;
  const int32_t destination =
      normalizeStep(static_cast<int64_t>(current) + signedDelta);

  DialMove move = {direction,
                   signedDelta,
                   destination,
                   stepToMark(destination),
                   0};
  return move;
}

bool buildCombinationPlan(const DialGeometry& geometry,
                          int32_t currentStep,
                          Direction firstDirection,
                          const float* marks,
                          size_t markCount,
                          CombinationPlan& plan) {
  plan.count = 0;
  plan.destinationStep = geometry.normalizeStep(currentStep);

  if (marks == nullptr || markCount == 0 ||
      markCount > kMaxCombinationWheels) {
    return false;
  }
  for (size_t index = 0; index < markCount; ++index) {
    if (!isfinite(marks[index])) {
      return false;
    }
  }

  Direction direction = firstDirection;
  int32_t cursor = plan.destinationStep;
  for (size_t index = 0; index < markCount; ++index) {
    const uint8_t passes = static_cast<uint8_t>(markCount - index);
    plan.moves[index] =
        geometry.planToMark(cursor, marks[index], direction, passes);
    cursor = plan.moves[index].destinationStep;
    direction = opposite(direction);
  }

  plan.count = markCount;
  plan.destinationStep = cursor;
  return true;
}

}  // namespace wheelwrecker
