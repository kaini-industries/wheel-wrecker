#include <Arduino.h>
#include <AccelStepper.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Define pin numbers for Step, Direction, and Enable
const int stepPin = 2;
const int directionPin = 3;
const int enablePin = 4;

const int pulses = 200;

const int oneRev = 3200;
// const int oneRev = 3000;

/*
const int speed = 100000000;
const int acceleration = 10000000;
*/
const int speed = 10000;
const int acceleration = 1000000;
const int delayInt = 2000;

AccelStepper stepper(AccelStepper::DRIVER, stepPin, directionPin);
// int moveNext = 1600;

void setup() {
  Serial.begin(9600); // Debug terminal

  stepper.setEnablePin(enablePin);
  stepper.setPinsInverted(false, false, true);

  stepper.enableOutputs();

  /*
  stepper.setMaxSpeed(speed); // Adjust as needed
  stepper.setAcceleration(acceleration); // Adjust as needed
  */
  stepper.setMaxSpeed(10000);
  stepper.setSpeed(1000);
  stepper.setAcceleration(100000);

  /*
  stepper.moveTo(0); // Move to position 8000 steps
  stepper.runToPosition(); // Run until the target position is reached
  delay(0);
  */

  // sstepper.disableOutputs();
  Serial.println("end setup()");
}

void loop() {
  stepper.enableOutputs();

  /*
  stepper.moveTo(0);
  stepper.runToPosition();
  delay(0);
  */

  /*
  stepper.moveTo(0);
  stepper.runToPosition();
  delay(100);
  */

  stepper.moveTo(800 * 2);
  stepper.runToPosition();
  delay(delayInt);
  Serial.println("1");

  stepper.moveTo(1600 * 2);
  stepper.runToPosition();
  delay(delayInt);
  Serial.println("2");

  stepper.moveTo(2400 * 2);
  stepper.runToPosition();
  delay(delayInt);
  Serial.println("3");

  stepper.moveTo(3600 * 2);
  stepper.runToPosition();
  delay(delayInt);
  Serial.println("4");

  /*
  stepper.moveTo(oneRev);
  stepper.runToPosition();
  delay(100);

  stepper.moveTo(-1 * oneRev);
  stepper.runToPosition();
  delay(100);

  stepper.moveTo(oneRev);
  stepper.runToPosition();
  delay(100);

  stepper.moveTo(-1 * oneRev);
  stepper.runToPosition();
  delay(100);

  stepper.moveTo(oneRev / 2);
  stepper.runToPosition();
  delay(100);
  */

  /*
  stepper.moveTo(-oneRev);
  stepper.runToPosition();
  delay(10);

  stepper.moveTo(oneRev / 2);
  stepper.runToPosition();
  delay(10);

  stepper.moveTo(oneRev / 2);
  stepper.runToPosition();
  delay(10);

  stepper.moveTo(-oneRev);
  stepper.runToPosition();
  delay(10);
  */

  /*
  stepper.moveTo(oneRev);
  stepper.runToPosition();
  delay(500);

  stepper.moveTo(1600);
  stepper.runToPosition();
  delay(500);

  stepper.moveTo(-1600);
  stepper.runToPosition();
  delay(500);
  /*/

  /*
  stepper.moveTo(oneRev / 2);
  stepper.runToPosition();
  delay(500);

  stepper.moveTo(-oneRev);
  stepper.runToPosition();
  delay(500);

  stepper.moveTo(oneRev);
  stepper.runToPosition();
  delay(500);
  */


  // stepper.disableOutputs();
}
