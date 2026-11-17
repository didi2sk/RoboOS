10 REM TCP server on port 23: from a PC type  nc <address> 23
20 REM then send 1 (LED on) or 0 (LED off). LED on GPIO17 + the green ACT LED.
30 PINMODE 17, OUT
40 L = TCPLISTEN(23) : IF L < 0 THEN PRINT "TCPLISTEN error "; L : END
50 PRINT "waiting for a connection on port 23"
60 C = TCPACCEPT(L) : IF C = 0 THEN 60
70 PRINT "connected" : SOCKPRINT C, "RoboOS LED server: send 1 or 0"
80 K = SOCKGET(C) : IF K = -2 THEN 120
90 IF K = 49 THEN GPIO 17, HIGH : LED ON : SOCKPRINT C, "LED on, CPU "; TEMP; " C"
100 IF K = 48 THEN GPIO 17, LOW : LED OFF : SOCKPRINT C, "LED off"
110 GOTO 80
120 PRINT "disconnected" : SOCKCLOSE C : GOTO 50
