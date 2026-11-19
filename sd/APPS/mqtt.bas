10 REM MQTT: temperature every 5 s to roboos/<name>/temp ('mqtt broker <host>' first)
20 IF NOT MQTTOK THEN PRINT "MQTT not connected": END
30 T = SENSOR(1): IF T = -999999 THEN T = SENSOR(0)
40 MQTTPUB "temp", T / 100; "."; T MOD 100 / 10; T MOD 10
50 PRINT "sent "; T / 100; "."; T MOD 100 / 10; T MOD 10
60 WAIT 5000: GOTO 30
