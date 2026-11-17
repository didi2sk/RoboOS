10 REM Sounds on the audio jack (GPIO18/19 on a Zero 2 W). Ctrl+C stops.
20 VOLUME 60
30 PRINT "a rising tone" : FOR F = 200 TO 2000 STEP 100 : SOUND F, 40 : NEXT F
40 PRINT "a tune" : PLAY "T150 L8 C D E F G4 G4 A A A A G2 A A A A G2 F F F F E4 E4 D D D D C2"
50 PRINT "a beep for every key, Ctrl+C stops"
60 K = INKEY : IF K THEN SOUND 200 + K * 10, 100
70 GOTO 60
