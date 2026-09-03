#include "StatusDisplay.h"

#include <Wire.h>

#include "HardwareConfig.h"

namespace wheelwrecker {

StatusDisplay::StatusDisplay()
    : display_(hardware::kOledWidth,
               hardware::kOledHeight,
               &Wire,
               -1,
               hardware::kOledI2cClock,
               hardware::kOledI2cClock),
      health_(DisplayHealth::Disabled),
      address_(0),
      wireStarted_(false) {}

void StatusDisplay::beginWire(bool restart) {
  if (wireStarted_ && !restart) {
    return;
  }

  // On the UNO R4 core, Wire.begin() closes an existing peripheral before
  // reopening it. This is important after a stuck-bus timeout.
  Wire.begin();
  Wire.setClock(hardware::kOledI2cClock);
  Wire.setWireTimeout(hardware::kOledI2cTimeoutMicros, false);
  wireStarted_ = true;
}

bool StatusDisplay::probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

DisplayHealth StatusDisplay::discoverAndInitialize() {
  address_ = 0;
  if (probe(hardware::kOledPrimaryAddress)) {
    address_ = hardware::kOledPrimaryAddress;
  } else if (hardware::kOledAlternateAddress !=
                 hardware::kOledPrimaryAddress &&
             probe(hardware::kOledAlternateAddress)) {
    address_ = hardware::kOledAlternateAddress;
  }

  if (address_ == 0) {
    health_ = DisplayHealth::Missing;
    return health_;
  }

  const bool initialized = display_.begin(SSD1306_SWITCHCAPVCC,
                                          address_,
                                          false,
                                          false);
  if (!initialized) {
    health_ = DisplayHealth::Fault;
    return health_;
  }

  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);
  display_.setTextWrap(false);
  display_.display();
  if (!probe(address_)) {
    health_ = DisplayHealth::Fault;
    return health_;
  }
  health_ = DisplayHealth::Ready;
  return health_;
}

DisplayHealth StatusDisplay::begin() {
  if (!hardware::kOledEnabled) {
    health_ = DisplayHealth::Disabled;
    return health_;
  }

  beginWire(false);
  delay(hardware::kOledStartupDelayMillis);
  return discoverAndInitialize();
}

DisplayHealth StatusDisplay::retry() {
  if (!hardware::kOledEnabled) {
    health_ = DisplayHealth::Disabled;
    return health_;
  }

  beginWire(true);
  return discoverAndInitialize();
}

bool StatusDisplay::render(const DisplaySnapshot& snapshot) {
  if (health_ != DisplayHealth::Ready) {
    return false;
  }
  if (!probe(address_)) {
    health_ = DisplayHealth::Fault;
    return false;
  }

  display_.clearDisplay();
  display_.setTextColor(SSD1306_WHITE);
  display_.setTextSize(1);
  display_.setCursor(0, 0);

  if (snapshot.sequenceActive) {
    display_.print(snapshot.operation);
    display_.print(' ');
    display_.print(snapshot.segmentNumber);
    display_.print('/');
    display_.print(snapshot.segmentCount);
  } else {
    display_.print(snapshot.armed ? F("ARMED") : F("DISARMED"));
    display_.print(F(" ENA:"));
    // ENA is an output request, not measured driver feedback.
    if (!snapshot.driverControlAvailable) {
      display_.print(F("N/C"));
    } else {
      display_.print(snapshot.driverEnabled ? F("ON") : F("OFF?"));
    }
  }

  display_.drawFastHLine(0, 9, hardware::kOledWidth, SSD1306_WHITE);
  display_.setCursor(0, 13);
  display_.print(F("CMD REF"));
  display_.setCursor(0, 23);
  display_.setTextSize(2);
  if (snapshot.positionKnown) {
    display_.print(snapshot.commandedMark, 2);
  } else {
    display_.print(F("UNKNOWN"));
  }

  display_.setTextSize(1);
  display_.setCursor(0, 43);
  if (snapshot.targetAvailable) {
    display_.print(F("TGT "));
    display_.print(snapshot.targetDirection == Direction::Left ? 'L' : 'R');
    display_.print(' ');
    display_.print(snapshot.targetMark, 2);
    if (snapshot.passesBeforeStop > 0) {
      display_.print(F(" P"));
      display_.print(snapshot.passesBeforeStop);
    }
  } else if (!snapshot.positionKnown) {
    display_.print(F("ALIGN DIAL + SETPOS"));
  } else {
    display_.print(F("SERIAL CONTROL READY"));
  }

  display_.setCursor(0, 55);
  display_.print(F("SPR:"));
  display_.print(snapshot.stepsPerRevolution);
  if (snapshot.sequenceActive) {
    display_.print(F("  ^C=STOP"));
  }

  display_.display();
  if (!probe(address_)) {
    health_ = DisplayHealth::Fault;
    return false;
  }
  return true;
}

DisplayHealth StatusDisplay::health() const {
  return health_;
}

const char* StatusDisplay::healthName() const {
  switch (health_) {
    case DisplayHealth::Disabled:
      return "DISABLED";
    case DisplayHealth::Missing:
      return "MISSING";
    case DisplayHealth::Ready:
      return "READY";
    case DisplayHealth::Fault:
      return "FAULT";
  }
  return "UNKNOWN";
}

uint8_t StatusDisplay::address() const {
  return address_;
}

}  // namespace wheelwrecker
