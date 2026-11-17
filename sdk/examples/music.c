/*
 * music.c - plays a tune on the 3.5 mm jack (headphones or an active speaker),
 * then a siren made with tone(). On a Zero 2 W the sound is on GPIO18/19.
 * Ctrl+C stops it.
 */
#include "robos.h"

int main(void)
{
    println("music: Ode to Joy, then a siren (Ctrl+C stops)");
    soundVolume(60);
    playNotes("T120 L4 O4 E E F G G F E D C C D E E.D8 D2"
              "E E F G G F E D C C D E D.C8 C2");
    delay(300);
    for (int round = 0; round < 3; round++) {
        for (int hz = 600; hz <= 1200; hz += 20) { tone(hz, 0); delay(8); }
        for (int hz = 1200; hz >= 600; hz -= 20) { tone(hz, 0); delay(8); }
    }
    noTone();
    return 0;
}
