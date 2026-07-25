#include <Arduino.h>

#define COIL_PIN 19



void setup() {
  pinMode(COIL_PIN, OUTPUT);
}

void loop() {

    // Read the current state, invert it (!), and write it back
  digitalWrite(COIL_PIN, 1); 
  
  delay(500);  // Wait for 1 second
  digitalWrite(COIL_PIN, 0); 
  
  delay(500);  // Wait for 1 second

  // put your main code here, to run repeatedly:
}
