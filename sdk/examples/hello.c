/* hello.c - the simplest program: main() style */
#include "robos.h"

int main(void)
{
    textColor(1);
    println("Hello from an SD card program!");
    textColor(0);
    printf("Running for %u ms since boot, CPU %d.%d C\n",
           (uint32_t)millis(), cpuTemperature() / 1000, cpuTemperature() % 1000 / 100);
    return 0;
}
