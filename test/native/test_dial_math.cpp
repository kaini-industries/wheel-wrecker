#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "DialMath.h"

using wheelwrecker::CombinationPlan;
using wheelwrecker::DialGeometry;
using wheelwrecker::DialMove;
using wheelwrecker::Direction;

namespace {

int failures = 0;

void expectTrue(bool actual, const char* label) {
  if (!actual) {
    std::cerr << "FAIL: " << label << '\n';
    ++failures;
  }
}

template <typename Expected, typename Actual>
void expectEqual(Expected expected, Actual actual, const char* label) {
  if (expected != actual) {
    std::cerr << "FAIL: " << label << " expected=" << expected
              << " actual=" << actual << '\n';
    ++failures;
  }
}

void expectNear(double expected,
                double actual,
                double tolerance,
                const char* label) {
  if (std::fabs(expected - actual) > tolerance) {
    std::cerr << "FAIL: " << label << " expected=" << expected
              << " actual=" << actual << '\n';
    ++failures;
  }
}

void testNormalizationAndConversion() {
  DialGeometry geometry(3200, 100.0f);

  expectNear(0.0, geometry.normalizeMark(100.0f), 1e-6, "100 wraps to 0");
  expectNear(1.0, geometry.normalizeMark(101.0f), 1e-6, "101 wraps to 1");
  expectNear(99.0, geometry.normalizeMark(-1.0f), 1e-6, "-1 wraps to 99");
  expectNear(50.5, geometry.normalizeMark(250.5f), 1e-6,
             "large fractional mark wraps");

  expectEqual(0, geometry.normalizeStep(3200), "full revolution step wraps");
  expectEqual(3199, geometry.normalizeStep(-1), "negative step wraps");
  expectEqual(800, geometry.markToStep(25.0f), "quarter revolution");
  expectEqual(3168, geometry.markToStep(99.0f), "mark 99 conversion");
  expectEqual(395, geometry.markToStep(12.34f), "nearest-step rounding");
  expectEqual(1, geometry.markToStep(0.015625f),
              "half step rounds upward");
  expectEqual(0, geometry.markToStep(99.984375f),
              "rounded final tick wraps to zero");
  expectEqual(0, geometry.markToStep(99.99f),
              "near-seam mark wraps after rounding");
  expectEqual(0, geometry.markToStep(-0.01f),
              "negative near-seam mark rounds to zero");
  expectEqual(0, geometry.markToStep(
                     std::numeric_limits<float>::quiet_NaN()),
              "non-finite mark converts safely");
  expectNear(12.34375, geometry.stepToMark(395), 1e-6,
             "quantized mark display");
  expectNear(0.03125, geometry.unitsPerStep(), 1e-9, "dial resolution");

  for (int mark = 0; mark < 100; ++mark) {
    const int32_t step = geometry.markToStep(static_cast<float>(mark));
    expectNear(mark, geometry.stepToMark(step), 1e-6,
               "integer mark round-trip");
  }

  expectTrue(!geometry.configure(0, 100.0f), "reject zero steps/revolution");
  expectTrue(!geometry.configure(3200, 0.0f), "reject zero dial size");
  expectTrue(!geometry.configure(
                 3200, std::numeric_limits<float>::quiet_NaN()),
             "reject non-finite dial size");
  expectEqual(3200, geometry.stepsPerRevolution(),
              "invalid configuration leaves geometry unchanged");
}

void testDirectedMoves() {
  const DialGeometry geometry(3200, 100.0f);

  DialMove move =
      geometry.planToMark(geometry.markToStep(10), 20, Direction::Left);
  expectEqual<int64_t>(320, move.deltaSteps, "10 to 20 left");

  move = geometry.planToMark(geometry.markToStep(10), 20, Direction::Right);
  expectEqual<int64_t>(-2880, move.deltaSteps, "10 to 20 right");

  move = geometry.planToMark(geometry.markToStep(99), 0, Direction::Left);
  expectEqual<int64_t>(32, move.deltaSteps, "99 to 0 left wrap");

  move = geometry.planToMark(geometry.markToStep(0), 99, Direction::Right);
  expectEqual<int64_t>(-32, move.deltaSteps, "0 to 99 right wrap");

  move = geometry.planToMark(geometry.markToStep(42), 42, Direction::Left);
  expectEqual<int64_t>(0, move.deltaSteps, "same target without passes");

  move = geometry.planToMark(geometry.markToStep(10), 20, Direction::Left, 3);
  expectEqual<int64_t>(9920, move.deltaSteps, "three passes plus target arrival");

  move = geometry.planToMark(geometry.markToStep(42), 42, Direction::Left, 3);
  expectEqual<int64_t>(12800, move.deltaSteps,
                       "same target preserves fourth arrival");

  move = geometry.planToMark(geometry.markToStep(42), 42, Direction::Right, 2);
  expectEqual<int64_t>(-9600, move.deltaSteps,
                       "same target preserves third arrival right");

  move = geometry.planToMark(1, 1.0f, Direction::Left);
  expectEqual<int64_t>(31, move.deltaSteps, "arbitrary tick to mark left");
  expectEqual(32, move.destinationStep, "arbitrary tick destination left");

  move = geometry.planToMark(1, 1.0f, Direction::Right);
  expectEqual<int64_t>(-3169, move.deltaSteps,
                       "arbitrary tick to mark right");
  expectEqual(32, move.destinationStep, "arbitrary tick destination right");

  move = geometry.planToMark(-1, 0.0f, Direction::Left);
  expectEqual<int64_t>(1, move.deltaSteps, "unwrapped current step left");

  move = geometry.planToMark(-1, 0.0f, Direction::Right);
  expectEqual<int64_t>(-3199, move.deltaSteps, "unwrapped current step right");

  const DialMove first = geometry.planToMark(
      geometry.markToStep(90.0f), 10.0f, Direction::Left);
  const DialMove second = geometry.planToMark(
      first.destinationStep, 90.0f, Direction::Right);
  expectEqual<int64_t>(0, first.deltaSteps + second.deltaSteps,
                       "direction reversal returns by the same arc");

  move = geometry.planToMark(
      123, std::numeric_limits<float>::quiet_NaN(), Direction::Left, 3);
  expectEqual<int64_t>(0, move.deltaSteps,
                       "non-finite directed target is a no-op");
  expectEqual(123, move.destinationStep,
              "non-finite directed target preserves position");
}

void testTargetRoundingDoesNotDrift() {
  const DialGeometry geometry(3200, 100.0f);
  int32_t cursor = 0;
  int64_t total = 0;

  const float targets[] = {33.33f, 66.66f, 0.0f};
  const int64_t expected[] = {1067, 1066, 1067};
  for (size_t index = 0; index < 3; ++index) {
    const DialMove move =
        geometry.planToMark(cursor, targets[index], Direction::Left);
    expectEqual(expected[index], move.deltaSteps, "rounded target segment");
    cursor = move.destinationStep;
    total += move.deltaSteps;
  }
  expectEqual<int64_t>(3200, total, "rounded segments total one revolution");
  expectEqual(0, cursor, "rounded segments return to zero");
}

void testRelativeMoves() {
  const DialGeometry geometry(3200, 100.0f);
  const int32_t start = geometry.markToStep(75.0f);

  DialMove move = geometry.planRelative(start, 100.0f, Direction::Left);
  expectEqual<int64_t>(3200, move.deltaSteps, "one relative turn left");
  expectEqual(start, move.destinationStep,
              "one relative turn preserves wrapped position");

  move = geometry.planRelative(start, 100.0f, Direction::Right);
  expectEqual<int64_t>(-3200, move.deltaSteps, "one relative turn right");
  expectEqual(start, move.destinationStep,
              "right turn preserves wrapped position");

  move = geometry.planRelative(start, 25.0f, Direction::Left);
  expectEqual<int64_t>(800, move.deltaSteps, "quarter turn left");
  expectEqual(0, move.destinationStep, "quarter turn wraps at zero");

  move = geometry.planRelative(0, 0.1f, Direction::Left);
  expectEqual<int64_t>(3, move.deltaSteps,
                       "fractional relative move rounds once");
  expectEqual(3, move.destinationStep, "rounded relative endpoint");

  move = geometry.planRelative(geometry.markToStep(99.0f), 1.0f,
                               Direction::Left);
  expectEqual<int64_t>(32, move.deltaSteps, "one dial unit left");
  expectEqual(0, move.destinationStep, "one dial unit wraps left");

  move = geometry.planRelative(0, 1.0f, Direction::Right);
  expectEqual<int64_t>(-32, move.deltaSteps, "one dial unit right");
  expectEqual(geometry.markToStep(99.0f), move.destinationStep,
              "one dial unit wraps right");

  move = geometry.planRelative(0, 12.34f, Direction::Left);
  expectEqual<int64_t>(395, move.deltaSteps,
                       "fractional dial-unit conversion");
  expectNear(12.34375, move.targetMark, 1e-6,
             "fractional move reports quantized target");

  move = geometry.planRelative(
      123, std::numeric_limits<float>::infinity(), Direction::Right);
  expectEqual<int64_t>(0, move.deltaSteps,
                       "non-finite relative move is a no-op");
  expectEqual(123, move.destinationStep,
              "non-finite relative move preserves position");
}

void testCombinationPlans() {
  const DialGeometry geometry(3200, 100.0f);
  const float marks[] = {20.0f, 40.0f, 60.0f};
  CombinationPlan plan;

  expectTrue(wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Right, marks, 3, plan),
             "build R/L/R combination");
  expectEqual<size_t>(3, plan.count, "three combination segments");
  expectEqual<int64_t>(-12160, plan.moves[0].deltaSteps,
                       "combination first segment");
  expectEqual<int64_t>(7040, plan.moves[1].deltaSteps,
                       "combination second segment");
  expectEqual<int64_t>(-5760, plan.moves[2].deltaSteps,
                       "combination third segment");
  expectEqual(geometry.markToStep(60), plan.destinationStep,
              "combination final mark");
  expectTrue(plan.moves[0].direction == Direction::Right,
             "combination first direction");
  expectTrue(plan.moves[1].direction == Direction::Left,
             "combination second direction");
  expectTrue(plan.moves[2].direction == Direction::Right,
             "combination third direction");
  expectEqual(3, plan.moves[0].passesBeforeStop,
              "combination first pass count");
  expectEqual(2, plan.moves[1].passesBeforeStop,
              "combination second pass count");
  expectEqual(1, plan.moves[2].passesBeforeStop,
              "combination third pass count");

  const float repeatedMarks[] = {25.0f, 25.0f, 25.0f};
  expectTrue(wheelwrecker::buildCombinationPlan(
                 geometry, geometry.markToStep(25), Direction::Right,
                 repeatedMarks, 3, plan),
             "build same-mark combination");
  expectEqual<int64_t>(-12800, plan.moves[0].deltaSteps,
                       "same-mark fourth arrival");
  expectEqual<int64_t>(9600, plan.moves[1].deltaSteps,
                       "same-mark third arrival");
  expectEqual<int64_t>(-6400, plan.moves[2].deltaSteps,
                       "same-mark second arrival");

  expectTrue(wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Left, marks, 3, plan),
             "build mirrored L/R/L combination");
  expectEqual<int64_t>(10240, plan.moves[0].deltaSteps,
                       "mirrored first segment");
  expectEqual<int64_t>(-8960, plan.moves[1].deltaSteps,
                       "mirrored second segment");
  expectEqual<int64_t>(3840, plan.moves[2].deltaSteps,
                       "mirrored third segment");

  expectTrue(!wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Left, nullptr, 0, plan),
             "reject empty combination");

  const float fiveMarks[] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
  expectTrue(wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Right, fiveMarks, 5, plan),
             "build maximum-size combination");
  const int64_t expectedDeltas[] = {-18880, 13120, -12480, 6720, -6080};
  for (size_t index = 0; index < 5; ++index) {
    expectEqual(expectedDeltas[index], plan.moves[index].deltaSteps,
                "maximum-size combination segment");
    expectEqual(static_cast<uint8_t>(5 - index),
                plan.moves[index].passesBeforeStop,
                "maximum-size combination pass count");
  }
  expectEqual(geometry.markToStep(50.0f), plan.destinationStep,
              "maximum-size combination endpoint");

  const float sixMarks[] = {10, 20, 30, 40, 50, 60};
  expectTrue(!wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Right, sixMarks, 6, plan),
             "reject over-size combination");
  expectEqual<size_t>(0, plan.count,
                      "rejected over-size combination has no moves");

  const float invalidMarks[] = {
      20.0f, std::numeric_limits<float>::quiet_NaN(), 60.0f};
  expectTrue(!wheelwrecker::buildCombinationPlan(
                 geometry, 0, Direction::Left, invalidMarks, 3, plan),
             "reject non-finite combination mark");
  expectEqual<size_t>(0, plan.count,
                      "rejected non-finite combination has no moves");
}

void testDirectedMoveProperties() {
  const DialGeometry geometry(3200, 100.0f);

  for (int from = 0; from < 100; ++from) {
    for (int to = 0; to < 100; ++to) {
      const int32_t start = geometry.markToStep(static_cast<float>(from));
      const int32_t target = geometry.markToStep(static_cast<float>(to));
      const DialMove left =
          geometry.planToMark(start, static_cast<float>(to), Direction::Left);
      const DialMove right =
          geometry.planToMark(start, static_cast<float>(to), Direction::Right);

      expectEqual(target, geometry.normalizeStep(start + left.deltaSteps),
                  "left endpoint invariant");
      expectEqual(target, geometry.normalizeStep(start + right.deltaSteps),
                  "right endpoint invariant");
      expectTrue(left.deltaSteps >= 0, "left delta sign");
      expectTrue(right.deltaSteps <= 0, "right delta sign");

      if (from != to) {
        expectEqual<int64_t>(3200, left.deltaSteps - right.deltaSteps,
                             "opposite directed arcs total one revolution");
      }
    }
  }
}

}  // namespace

int main() {
  testNormalizationAndConversion();
  testDirectedMoves();
  testTargetRoundingDoesNotDrift();
  testRelativeMoves();
  testCombinationPlans();
  testDirectedMoveProperties();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "All dial-math tests passed\n";
  return EXIT_SUCCESS;
}
