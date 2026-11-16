/*
 * blink.c - Arduino-style blink.
 * LED + 330 ohm resistor from GPIO17 (pin 11) to GND (pin 9).
 * The green ACT LED on the board blinks along. Ctrl+C stops it.
 */
#include "robos.h"

#define LED_PIN 17

void setup(void)
{
    pinMode(LED_PIN, OUTPUT);
    println("blink: GPIO17 + ACT LED, Ctrl+C to stop");
}

void loop(void)
{
    digitalWrite(LED_PIN, HIGH);
    actLed(1);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    actLed(0);
    delay(500);
}
