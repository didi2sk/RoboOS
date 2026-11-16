/*
 * knight.c - "Knight Rider" running light on 6 LEDs.
 * LEDs (each with a 330 ohm resistor) on GPIO 5, 6, 13, 19, 26, 21.
 * Keys + and - change the speed.
 */
#include "robos.h"

static const int pins[] = { 5, 6, 13, 19, 26, 21 };
#define N (int)(sizeof(pins) / sizeof(pins[0]))
static int speed = 80;

void setup(void)
{
    for (int i = 0; i < N; i++) pinMode(pins[i], OUTPUT);
    println("knight: + / - change speed, Ctrl+C to stop");
}

static void step(int i)
{
    for (int j = 0; j < N; j++) digitalWrite(pins[j], j == i);
    int k = getKey();
    if (k == '+' && speed > 10)  speed -= 10;
    if (k == '-' && speed < 500) speed += 10;
    delay(speed);
}

void loop(void)
{
    for (int i = 0; i < N; i++)      step(i);
    for (int i = N - 2; i > 0; i--)  step(i);
}
