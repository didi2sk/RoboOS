10 REM Knight Rider on GPIO 5, 6, 13, 19, 26, 21 (+ and - change speed)
20 DIM P(5) : P(0) = 5 : P(1) = 6 : P(2) = 13 : P(3) = 19 : P(4) = 26 : P(5) = 21
30 FOR I = 0 TO 5 : PINMODE P(I), OUT : NEXT I
40 S = 80
50 FOR I = 0 TO 5 : GOSUB 100 : NEXT I
60 FOR I = 4 TO 1 STEP -1 : GOSUB 100 : NEXT I
70 GOTO 50
100 FOR J = 0 TO 5 : GPIO P(J), J = I : NEXT J
110 K = INKEY
120 IF K = 43 AND S > 10 THEN S = S - 10
130 IF K = 45 AND S < 500 THEN S = S + 10
140 WAIT S : RETURN
