#!/bin/sh
# Voice files for the alarm demo (sdk/examples/alarm.c), made with espeak-ng + ffmpeg:
#   tools/mkvoice.sh [language] [folder]      e.g. tools/mkvoice.sh sk sd/ALARM   (en for English)
# 16 kHz mono 16-bit WAV. Replace any file with your own recording (same name, 8-48 kHz).
set -e
L=${1:-sk}; D=${2:-sd/ALARM}
mkdir -p "$D"
say() {   # say NAME "text"
    espeak-ng -v "$L" -s 150 -w /tmp/mkvoice.wav "$2"
    ffmpeg -loglevel error -y -i /tmp/mkvoice.wav -ac 1 -ar 16000 -c:a pcm_s16le "$D/$1.WAV"
}
if [ "$L" = sk ]; then
    say ARMING   "Alarm sa zapína. Opustite priestor."
    say ARMED    "Alarm je zapnutý."
    say ENTRY    "Zadajte kód."
    say ALARM    "Poplach!"
    say DISARMED "Alarm je vypnutý."
    say WRONG    "Nesprávny kód."
    say NOARM    "Alarm sa nedá zapnúť, zóna je otvorená."
    n=1; for w in jeden dva tri štyri päť šesť sedem osem deväť desať; do say $n "$w"; n=$((n+1)); done
else
    say ARMING   "Arming. Please leave."
    say ARMED    "The alarm is on."
    say ENTRY    "Enter the code."
    say ALARM    "Alarm!"
    say DISARMED "The alarm is off."
    say WRONG    "Wrong code."
    say NOARM    "Cannot arm, a zone is open."
    n=1; for w in one two three four five six seven eight nine ten; do say $n "$w"; n=$((n+1)); done
fi
rm -f /tmp/mkvoice.wav
echo "voice files in $D"
