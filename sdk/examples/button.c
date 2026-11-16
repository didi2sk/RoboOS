/*
 * button.c - a push button switches an LED.
 * Button between GPIO27 (pin 13) and GND (pin 14), internal pull-up.
 * LED + 330 ohm resistor on GPIO17 (pin 11).
 */
#include "robos.h"

#define LED_PIN    17
#define BUTTON_PIN 27

static int last = -1;

void setup(void)
{
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    println("button: press the button on GPIO27, Ctrl+C to stop");
}

void loop(void)
{
    int pressed = digitalRead(BUTTON_PIN) == LOW;
    digitalWrite(LED_PIN, pressed ? HIGH : LOW);
    if (pressed != last) {
        printf("%u ms: button %s\n", (uint32_t)millis(), pressed ? "pressed" : "released");
        last = pressed;
    }
    delay(10);
}
