10 REM I2C display (OLED 0x3C or LCD 0x27): time and temperature, refreshed every second
20 DISPLAY CLS
30 DISPLAY 1, "RoboOS BASIC"
40 T = SENSOR(1): IF T = -999999 THEN T = SENSOR(0)
50 DISPLAY 2, HOUR; ":"; MINUTE / 10; MINUTE MOD 10; ":"; SECOND / 10; SECOND MOD 10
60 DISPLAY 3, "temp "; T / 100; "."; T MOD 100 / 10; " C"
70 WAIT 1000: GOTO 40
