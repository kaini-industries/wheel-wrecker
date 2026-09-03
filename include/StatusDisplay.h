#pragma once

#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "DialMath.h"

namespace wheelwrecker {

enum class DisplayHealth : uint8_t {
  Disabled,
  Missing,
  Ready,
  Fault,
};

struct DisplaySnapshot {
  bool armed;
  bool positionKnown;
  bool driverControlAvailable;
  bool driverEnabled;
  bool sequenceActive;
  float commandedMark;
  int32_t stepsPerRevolution;

  const char* operation;
  uint8_t segmentNumber;
  uint8_t segmentCount;
  bool targetAvailable;
  Direction targetDirection;
  float targetMark;
  uint8_t passesBeforeStop;
};

class StatusDisplay {
 public:
  StatusDisplay();

  DisplayHealth begin();
  DisplayHealth retry();
  bool render(const DisplaySnapshot& snapshot);

  DisplayHealth health() const;
  const char* healthName() const;
  uint8_t address() const;

 private:
  void beginWire(bool restart);
  bool probe(uint8_t address);
  DisplayHealth discoverAndInitialize();

  Adafruit_SSD1306 display_;
  DisplayHealth health_;
  uint8_t address_;
  bool wireStarted_;
};

}  // namespace wheelwrecker
