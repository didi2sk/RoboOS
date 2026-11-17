10 REM Sends the CPU temperature every 2 s as a UDP broadcast to port 5000.
20 REM On a PC in the same network:  nc -ul 5000
30 H = UDPOPEN(0, "255.255.255.255", 5000) : IF H < 0 THEN PRINT "UDPOPEN error "; H : END
40 SOCKPRINT H, "RoboOS: CPU "; TEMP; " C, up "; MILLIS / 1000; " s"
50 PRINT "sent"
60 WAIT 2000 : GOTO 40
