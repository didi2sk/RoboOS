# RoboOS

[English](README.md) | **Slovensky**

![Štart RoboOS](docs/screenshot.png)

Mini operačný systém pre Raspberry Pi, ktorý beží priamo na hardvéri, bez Linuxu.
Ovláda sa **USB klávesnicou** alebo cez sériovú linku (UART). Výstup ide naraz na
HDMI aj UART a oba vstupy fungujú súčasne.

Aktuálna verzia: **0.21**

## Podporované dosky

Jeden `kernel8.img` beží na všetkých doskách so 64-bitovým čipom BCM2837. Model sa
zistí pri štarte (vypíše sa pod logom a v prompte, napr. `robo@zero2`).

| Doska | Poznámky |
|-------|----------|
| Raspberry Pi 3 Model B / B+ / A+ | otestované na 3B |
| Raspberry Pi Zero 2 W | mini-HDMI; klávesnica cez **OTG redukciu** do portu `USB` (napájanie do `PWR IN`); hot-plug funguje aj na tomto porte, s USB hubom aj bez neho |
| Raspberry Pi 2 Model B **v1.2** | v1.2 má čip BCM2837 (na doske je vytlačené „Raspberry Pi 2 Model B V1.2“) |
| Compute Module 3 / 3+ | |

Nepodporované: Pi 1, Zero / Zero W, Pi 2 v1.1 (32-bitové čipy), Pi 4 a Pi 5 (iné
periférie, USB za PCIe).

ACT LED je na GPIO expanderi firmvéru (3B, CM3), na GPIO29 (3B+, 3A+, Zero 2 W – aktívna v nule)
alebo na GPIO47 (Pi 2 v1.2).

## Príprava SD karty

1. Naformátuj SD kartu na **FAT32** s tabuľkou oddielov **MBR** (veľkosť nerozhoduje).
   Pri kartách nad 32 GB to vo Windows ide cez Raspberry Pi Imager (*Erase*) alebo `diskpart`.
2. Skopíruj na ňu celý obsah priečinka `sd/`:
   `bootcode.bin`, `start.elf`, `fixup.dat`, `config.txt`, `kernel8.img`
   a priečinky `APPS` (ukážkové programy) a `RUN` (autostart).
3. Vlož kartu do Raspberry Pi a zapni ho.

## Programy a autostart

SD karta je zároveň disk RoboOS (FAT32 alebo FAT16, čítanie aj zápis).
RoboOS má vlastný ovládač SD (firmvér len načíta `kernel8.img` a potom ustúpi).
Ak sa karta nenájde, `sd init` ukáže, ktorý krok zlyhal, a registre radiča.

- Všetko v **`/RUN`** (`*.bin` a `*.bas`) sa po štarte spustí samo, v abecednom poradí.
  Stlačením ľubovoľnej klávesy do 2 s sa autostart preskočí.
- `run <meno>` spustí program z aktuálneho priečinka, z `/RUN` alebo `/APPS`; prípona je
  nepovinná.
- Pohyb po karte: `cd APPS`, `cd ..`, `cd /`, `pwd`; prompt ukazuje, kde si
  (`robo@rpi3:/APPS$`). Všetky príkazy aj BASIC (`LOAD`, `SAVE`, `DIR`) berú cesty relatívne
  k aktuálnemu priečinku. **Tab** dopĺňa mená súborov a priečinkov.
- `mkdir <priečinok>` vytvorí priečinok (meno 8.3), `rm <súbor>` zmaže súbor, `rm <priečinok>`
  prázdny priečinok. Súbory firmvéru a jadra v `/` sú chránené: zmazať sa dajú len cez `rm -f`.
- **Ctrl+C** zastaví bežiaci program a vráti sa do shellu.

### BASIC – programovanie priamo na RPi

```
robo@rpi3:~$ basic
] 10 PINMODE 17, OUT
] 20 GPIO 17, HIGH : WAIT 500
] 30 GPIO 17, LOW : WAIT 500
] 40 GOTO 20
] RUN                      (Ctrl+C ho zastaví)
] SAVE "/RUN/blink"        (uloží sa ako /RUN/blink.bas -> spustí sa po každom štarte)
] BYE
```

- Príkazy: `RUN`, `LIST [a-b]`, `NEW`, `SAVE "meno"`, `LOAD "meno"`, `DIR [cesta]`, `HELP`, `BYE`;
  `MKDIR "priečinok"`, `RMDIR "priečinok"`, `KILL "súbor"` fungujú aj v programoch.
  Samotné číslo riadku riadok zmaže.
- Príkazy v programe: `PRINT`/`?`, `INPUT`, `LET`, `IF..THEN..ELSE`, `GOTO`, `GOSUB`/`RETURN`,
  `FOR..TO..STEP`/`NEXT`, `DIM`, `END`, `REM`/`'`, `CLS`, `RANDOMIZE`; viac príkazov oddeľ `:`.
- GPIO: `PINMODE pin, OUT|IN|PULLUP|PULLDOWN`, `GPIO pin, HIGH|LOW`, `GPIO(pin)`, `LED ON|OFF`, `WAIT ms`.
- Obrázky: `IMAGE "súbor", x, y [, w, h]` (pozri časť o obrázkoch).
- Video: `VIDEO "súbor.mpg"` (pozri časť o videu).
- Zvuk: `SOUND Hz, ms`, `PLAY "noty"`, `VOLUME 0-100`, `PLAYWAV "subor.wav"` (pozri časť o zvuku).
- Sieť: `NETSEND`, `NETOK`, `NETAVAIL`, `NETRECV`; TCP/UDP: `TCPCONNECT`, `TCPLISTEN`,
  `TCPACCEPT`, `UDPOPEN`, `SOCKPRINT`, `UDPSEND`, `SOCKAVAIL`, `SOCKGET`, `SOCKNUM`, `SOCKCLOSE`
  (pozri časti o sieti).
- Funkcie: `ABS()`, `RND(n)`, `MIN(a,b)`, `MAX(a,b)`, `MILLIS`, `MICROS`, `INKEY`, `TEMP`, `CHR$()` (v PRINT).
- Iba celé čísla (64-bit). Kľúčové slová musia byť oddelené medzerami. Súbory `.bas` sú
  obyčajný text, dajú sa písať aj na PC.
- Mená súborov pri ukladaní: 8.3 (max. 8 znakov + prípona).

Ukážky na karte: `APPS/blink.bas`, `APPS/button.bas`, `APPS/knight.bas`,
`APPS/ledsrv.bas` (LED ovládaná cez TCP), `APPS/udptemp.bas` (teplota cez UDP),
`APPS/sound.bas` (tóny a pesnička).

### Textový editor

`edit <súbor>` otvorí celoobrazovkový editor, takže `.bas` a textové súbory sa dajú písať
a meniť priamo na RPi (`edit /RUN/blink.bas`, potom `run blink`).

| Klávesa | Akcia |
|---------|-------|
| šípky, Home, End, Page Up, Page Down | pohyb kurzora |
| Backspace, Delete, Enter, Tab | úpravy (Tab vkladá medzery) |
| Ctrl+S | uložiť |
| Ctrl+Q | koniec (pri neuložených zmenách sa opýta) |
| Ctrl+K / Ctrl+U | vystrihnúť aktuálny riadok / vložiť ho nad riadok s kurzorom |
| Ctrl+L | prekresliť obrazovku |

- Súbory do 1 MB. Nové súbory sa ukladajú s menom 8.3 (max 8 znakov + prípona);
  súbor s dlhým menom sa dá prezerať, ale nie uložiť.
- Konce riadkov ostávajú, aké boli (CR LF alebo LF); nové súbory majú CR LF.
- Na UART termináli používa editor rozmer HDMI textu (128x48), takže okno terminálu
  musí byť aspoň také veľké; bez HDMI používa 80x24.

### Programy v C (SDK)

Priečinok `sdk/` kompiluje programy v C s API v štýle Arduina:

```c
#include "robos.h"

void setup(void) { pinMode(17, OUTPUT); }

void loop(void)
{
    digitalWrite(17, HIGH); delay(500);
    digitalWrite(17, LOW);  delay(500);
}
```

`cd sdk && make` preloží každý `examples/*.c` na `bin/*.bin`; tie skopíruj na kartu
(do `/RUN` pre autostart). API: `pinMode`, `digitalWrite`, `digitalRead`, `delay`,
`delayMicroseconds`, `millis`, `micros`, `print`, `println`, `printf`, `getKey`,
`clearScreen`, `textColor`, `actLed`, `cpuTemperature`, `stopRequested`, `exit`,
`netSend`, `netReceive`, `netName` a TCP/UDP: `tcpConnect`, `tcpListen`, `tcpAccept`,
`udpOpen`, `sockWrite`, `sockPrint`, `sockRead`, `sockAvailable`, `udpSendTo`,
`udpReceiveFrom`, `sockClose`, `sockError`, `ipAddress` a zvuk: `tone(hz, ms)`, `noTone`,
`playNotes`, `soundVolume`, `playWav`, obrázky: `drawImage(path, x, y, w, h)`, `imageError` a
video: `playVideo(path)`, `videoError`
(viď `sdk/robos.h`). Program môže mať `setup()` + `loop()` alebo `main()`.
Ukážky: `hello`, `blink`, `button`, `knight`, `chat`, `tcpserv` (TCP server), `udpecho` (UDP),
`music` (pesnička a siréna).

Programy sa nahrávajú na adresu 0x01000000 (max. 16 MB). Ak program spadne, vypíše sa
výnimka a systém sa vráti do shellu.

**GPIO má 3,3 V** (nie 5 V ako Arduino Uno), max. asi 16 mA na pin.
LED zapájaj cez odpor 220–330 Ω, relé a motory cez tranzistor alebo modul.

## Obrázky

RoboOS zobrazí na HDMI obrazovke JPEG (aj progresívny), PNG (s priehľadnosťou), BMP a GIF
(prvý snímok); veľký obrázok zmenší, aby sa zmestil, a zachová jeho tvar.

- `view <obrázok>` ho ukáže na celú obrazovku; **šípky vľavo/vpravo** (alebo medzera)
  prejdú na predchádzajúci / ďalší obrázok v tom istom priečinku, **Q** alebo Enter vráti
  textovú obrazovku tak, ako bola. `view <priečinok>` začne prvým obrázkom v ňom.
- **Obrázok pri štarte**: súbor `/SPLASH.PNG` (alebo `/SPLASH.JPG`, `/SPLASH.BMP`) sa ukáže,
  kým RoboOS štartuje; hlásenia sa píšu za ním a objavia sa najskôr po 2,5 s (klávesa to
  skráti). Na karte je obrázok s logom RoboOS - nahraď ho vlastným (najlepšie 1024x768),
  alebo ho zmaž a doska naštartuje rovno do textu.
- BASIC: `IMAGE "foto.jpg", x, y` nakreslí obrázok na x, y (pixely; 1024x768, ak ho `screen` nezmenil);
  `IMAGE "foto.jpg", x, y, w, h` ho vtesná do rámčeka w x h. C: `drawImage(path, x, y, w, h)`.
- Obrázky sa dekódujú v pamäti: 12-megapixelová fotka z mobilu trvá pár sekúnd.

## Rozlíšenie obrazovky

Textová obrazovka štartuje v 1024x768. `screen 1280x720` (hocijaká veľkosť od 320x200 po
2048x1280) prepne hneď a uloží sa do `/SCREEN.TXT`, takže ju použije aj ďalší štart;
`screen auto` použije vlastné rozlíšenie monitora (to, ktoré firmvér našiel na HDMI),
samotné `screen` ukáže aktuálnu veľkosť a veľkosť monitora. Ak ostane obraz čierny,
zmaž `/SCREEN.TXT` na PC (alebo doň napíš `1024x768`).

RoboOS si od grafického čipu pýta **dve stránky obrazovky**: text je na prvej, video sa
kreslí do skrytej a potom sa ukáže naraz (page flip), takže sa obraz netrhá.

## Video

`video <súbor.mpg>` prehrá video na celú obrazovku so zvukom (jack, GPIO18/19 alebo I2S DAC -
vyberá `sound out`). **Q** alebo Enter ho zastaví, **medzera** pozastaví, **šípky
vľavo/vpravo** skočia o 10 s späť / dopredu; shell potom vypíše, koľko snímok sa ukázalo
a koľko snímok za sekundu.

Formát je **MPEG-1 video so zvukom MP2** (`.mpg`, ako na Video CD): voľný, patenty dávno
vypršali a dekodér (pl_mpeg) je dosť malý, aby bežal priamo na doske. MP4 (H.264 / AAC) je
licencovaný a potrebuje hardvérový dekodér v GPU, ktorého rozhranie je uzavreté, preto ho
RoboOS neprehrá. Hocijaké video prekonvertuješ na PC cez ffmpeg:

```
ffmpeg -i film.mp4 -vf scale=640:-2 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg FILM.MPG
```

- Pre Pi 3 je dobrá veľkosť 640x360 alebo 640x480 pri 25/30 snímkach za sekundu; obraz sa
  zväčší na obrazovku (najviac 2x) a zachová tvar. `-q:v` 2 (najlepšie) .. 10 (najmenšie).
- Práca je rozdelená medzi jadrá: jadro 0 číta kartu a dekóduje, **jadrá 2 a 3** zároveň
  prepočítavajú farby (YCbCr na RGB) a zväčšujú predchádzajúcu snímku, jadro 1 drží USB
  a sieť. Ak to doska nestíha, snímky sa vynechajú (zvuk nikdy).
- BASIC: `VIDEO "film.mpg"`; C: `playVideo(path)` (API verzia 7, `videoError()`).

## Sieť: správy medzi RoboOS doskami

Pi 3B (LAN9514) a Pi 3B+ (LAN7515) majú ethernetový čip, ktorý je USB zariadenie za hubom
na doske; RoboOS má ovládač pre oba. 3B sa pripojí rýchlosťou 100 Mbit, 3B+ 1000 Mbit
(`net speed 100` ju obmedzí; cez USB 2.0 čip prenesie najviac asi 300 Mbit). Zapoj dosky do
toho istého switchu alebo routera a môžu si posielať krátke textové správy.
Protokol je vlastný, na surových ethernetových rámcoch (EtherType `88b5`), takže PC mu
nerozumie. Na komunikáciu s PC a internetom je TCP/IP (ďalšia časť).

```
robo@rpi3:~$ net who
  rpi3-1c2d        b8:27:eb:11:1c:2d  seen 0 s ago
robo@rpi3:~$ net send rpi3-1c2d hello
delivered
```

- Každá doska má meno: predvolene `rpi3-` + koniec jej MAC adresy.
  `net name <nové>` ho zmení a uloží do `/NAME.TXT` na SD karte.
- `net who` sa opýta, kto je v sieti. `net send <meno> <text>` pošle správu a čaká na
  potvrdenie (3 pokusy); `net send all <text>` ide všetkým, bez potvrdenia.
- Správa, ktorá príde, keď si v shelli na prompte, sa vypíše ako `[NET] meno: text`.
  Kým beží program, správy čakajú v schránke (posledných 8) na program.
- `net` ukáže meno, čip, MAC adresu, stav linky a počítadlá. `net test [sekundy]`
  vypisuje všetky rámce na linke, `net loop on` vracia odoslané správy späť na tú istú
  dosku (skúšanie s jednou doskou alebo v QEMU).
- BASIC: `NETSEND "meno", 42` alebo `NETSEND "ALL", "text"`; `NETOK` povie, či posledné
  odoslanie prešlo; `NETAVAIL` je počet čakajúcich správ a `NETRECV` vráti najstaršiu
  ako číslo. Príklad: jedna doska posiela tlačidlo, druhá rozsvieti LED:

  ```
  10 PINMODE 27, PULLUP : REM doska A        10 PINMODE 17, OUT : REM doska B
  20 NETSEND "ALL", GPIO(27) : WAIT 100      20 IF NETAVAIL THEN GPIO 17, NETRECV = 0
  30 GOTO 20                                 30 GOTO 20
  ```
- Programy v C: `netSend(to, text)`, `netReceive(from, text, max)`, `netName()`;
  pozri `sdk/examples/chat.c` (`run chat` = chat medzi doskami).

Nie je na Zero 2 W a 3A+ (nemajú Ethernet). 3B a 3B+ sa medzi sebou dohovoria.

## Sieť: TCP/IP

RoboOS používa [lwIP](https://savannah.nongnu.org/projects/lwip/) 2.2.1 (v `lwip/`,
licencia BSD), malý TCP/IP stack napísaný pre systémy bez operačného systému.
Po štarte si doska vypýta od routera adresu (DHCP) a pošle mu pritom svoje meno, takže
router ju v zozname zariadení ukáže pod jej RoboOS menom. Keď adresa príde, na prompte
sa objaví `[NET] IP address ...`.

```
robo@rpi3:~$ ip
host:    rpi3-9a4f
link:    up
address: 192.168.1.37 / 255.255.255.0  (DHCP)
gateway: 192.168.1.1
dns:     192.168.1.1
robo@rpi3:~$ ping 192.168.1.1
reply from 192.168.1.1: seq=1 ttl=64 time=0.9 ms
```

- `ip` ukáže adresu; `ip set <adresa> [maska] [brána]` nastaví pevnú adresu,
  `ip dhcp` vráti DHCP. Voľba sa uloží do `/IP.TXT` na SD karte.
- `ping <adresa alebo meno> [počet]` - mená sa prekladajú cez DNS.
- `http on` spustí malú webovú stránku s menom dosky, verziou, dobou behu a teplotou
  CPU: otvor `http://<adresa>/` v prehliadači. `http off` ju vypne. Voľba sa uloží do
  `/HTTP.TXT`, takže stránka (aj API nižšie) sa po reštarte spustí sama.
- Len IPv4.

### JSON API: piny a reštart cez sieť

Keď je nastavený token (`http token <tajne>`, 8-63 znakov, uloží sa do `/HTTP.TXT`),
webový server odpovedá aj na JSON požiadavky pod `/api/`. Každá požiadavka potrebuje
hlavičku `Authorization: Bearer <tajne>`; bez nej príde 401, bez nastaveného tokenu je API
vypnuté (403). `http token` ukáže token, `http token off` API vypne.

| Požiadavka | Čo robí |
|------------|---------|
| `GET /api/status` | meno, doska, verzia, adresa, doba behu, teplota CPU (v tisícinách stupňa) |
| `GET /api/gpio` | všetky piny 0-27: `{"pin":17,"mode":"out","value":1}` ... |
| `GET /api/gpio/17` | jeden pin |
| `POST /api/gpio` | `{"pin":17,"value":1}` - nastaví pin (stane sa výstupom); `value` 0, 1, true, false alebo `"toggle"`; `{"pin":4,"mode":"in","pull":"up"}` - vstup s pull-upom (`pull` up/down/off); zoznam `[{...},{...}]` nastaví viac pinov (najprv sa skontrolujú všetky, potom sa nastavia) |
| `POST /api/reboot` | odpovie `{"ok":true,"rebooting":true}` a o 0,5 s reštartuje |

```
curl -H "Authorization: Bearer tajne123" http://192.168.1.50/api/status
curl -H "Authorization: Bearer tajne123" -d '{"pin":17,"value":1}' http://192.168.1.50/api/gpio
curl -H "Authorization: Bearer tajne123" -X POST http://192.168.1.50/api/reboot
```

Chyby prídu ako `{"ok":false,"error":"..."}` s kódom 400 / 401 / 403 / 404 / 405. API môže
volať aj webová stránka z inej adresy (CORS je povolený). Doska vypíše
`[API] reboot asked by <adresa>` a `[API] wrong or missing token from <adresa>`.
**HTTP nie je šifrované**: kto vidí do tvojej siete, prečíta aj token, takže API používaj
len vo vlastnej sieti, nie z internetu. Piny 14/15 sú UART.

### TCP a UDP v programoch

Spojenia majú čísla 1-8 (program dostane číslo od `TCPCONNECT`, `TCPLISTEN`, `TCPACCEPT`
alebo `UDPOPEN`); záporný výsledok je chyba: -1 nie je sieť, -2 neznáme meno, -3 bez
odpovede / odmietnuté, -4 priveľa spojení, -5 zlé číslo, -6 zatvorené, -7 málo pamäte,
-8 port je obsadený. Všetko, čo program otvoril, sa po jeho skončení zatvorí.

BASIC (len celé čísla, takže dáta sa čítajú po bajtoch alebo ako čísla):

| | |
|---|---|
| `H = TCPCONNECT("host", port)` | pripojenie (adresa alebo meno), čaká najviac 10 s |
| `L = TCPLISTEN(port)`, `C = TCPACCEPT(L)` | server: `TCPACCEPT` dáva 0, kým sa niekto nepripojí |
| `U = UDPOPEN(port)`, `U = UDPOPEN(port, "host", port)` | UDP; bez hostu `SOCKPRINT` odpovedá poslednému odosielateľovi; port 0 = ľubovoľný |
| `SOCKPRINT H, "text"; X; CHR$(13)` | odoslanie ako PRINT (končí CR LF, ak nekončí `;`); jeden UDP datagram |
| `UDPSEND U, "host", port, položky` | UDP na danú adresu (`"255.255.255.255"` = všetkým) |
| `SOCKAVAIL(H)` | počet čakajúcich bajtov, -1 = druhá strana zatvorila |
| `SOCKGET(H)` | ďalší bajt, -1 = zatiaľ nič, -2 = zatvorené |
| `SOCKNUM(H)` | ďalšie číslo v dátach (čaká najviac 2 s), 0 ak nie je |
| `SOCKCLOSE H` | zatvorenie |

```
10 H = TCPCONNECT("192.168.1.10", 5000) : IF H < 0 THEN PRINT "chyba "; H : END
20 SOCKPRINT H, "teplota "; TEMP
30 K = SOCKGET(H) : IF K >= 0 THEN PRINT CHR$(K); : GOTO 30
40 IF K = -1 THEN 30
50 SOCKCLOSE H
```

C (`sdk/robos.h`): `tcpConnect`, `tcpListen`, `tcpAccept`, `udpOpen`, `sockWrite`, `sockPrint`,
`sockRead` (TCP: bajty, 0 = zatiaľ nič, -1 = zatvorené; UDP: jeden datagram), `sockAvailable`,
`udpSendTo`, `udpReceiveFrom`, `sockClose`, `sockError`, `ipAddress`. Ukážky:
`tcpserv` (TCP server pre `nc <adresa> 23`) a `udpecho` (`nc -u <adresa> 5000`).

Sieť (aj USB klávesnica) beží na vlastnom jadre procesora, takže doska odpovedá na ping,
podáva webovú stránku a prijíma správy, aj keď program práve počíta.

Funguje aj štandardný USB sieťový adaptér triedy CDC-ECM (takto sa sieť testuje v QEMU;
na skutočných doskách to zatiaľ testované nie je).

## Zvuk

Žiadne Raspberry Pi nemá zvukový čip: 3,5 mm jack poháňajú dva PWM kanály procesora cez
filter. RoboOS ich používa na tóny (obdĺžnikový signál); vlnu robí hardvér sám, program
beží ďalej. Do jacku zapoj slúchadlá alebo aktívny reproduktor. Dosky bez jacku (Zero 2 W,
CM3) používajú GPIO18 a GPIO19, kam sa dá pripojiť piezo bzučiak (alebo malý zosilňovač
s reproduktorom); `sound out gpio` to isté zapne na hocijakej doske.

- Shell: `beep [Hz] [ms]`, `play <noty>`, `sound` (výstup a hlasitosť),
  `sound volume 0-100`, `sound out jack|gpio`, `sound off`. Aj vypísanie znaku BEL
  (ASCII 7) pípne.
- BASIC: `SOUND 440, 500` (Hz, ms; pri ms 0 tón znie, kým nepríde `SOUND 0, 0`),
  `PLAY "T120 L4 C D E F G2"`, `VOLUME 60`.
- C: `tone(hz, ms)` spustí tón a hneď sa vráti (ms 0 = kým nepríde `noTone()`),
  `playNotes("...")` čaká do konca, `soundVolume(percent)`.
- Noty ako PLAY v QBasicu: `C D E F G A B` s `#` alebo `+` (zvýšenie) a `-` (zníženie), číslo
  za notou = dĺžka (`C8` = osminová), `.` = s bodkou, `P` alebo `R` = pauza, `L4` = základná
  dĺžka, `O4` = oktáva (0-7, `<` `>` nižšie / vyššie), `T120` = tempo, `MS` / `MN` / `ML` =
  staccato / normálne / legato.

Hlasitosť mení šírku impulzu, takže trochu mení aj farbu zvuku.

**Súbory WAV** hrajú aj na jacku (alebo GPIO18/19): `playwav <súbor>` (shell),
`PLAYWAV "súbor"` (BASIC), `playWav(cesta)` (C) - PCM, 8 alebo 16 bitov, mono alebo stereo,
8000-48000 Hz, napr. `playwav /APPS/chime.wav`; Ctrl+C zastaví. PWM pritom beží na 250 MHz
a vzorky k nemu idú cez DMA, takže zvuk hrá, aj keď RoboOS robí niečo iné; jack dá asi
12 bitov kvality pri 44,1 kHz (ako rádio, s trochou šumu). Tóny sa potom vrátia
k jednoduchému obdĺžnikovému signálu.

### I2S DAC modul (lepší zvuk, súbory WAV)

Na skutočný zvuk pripoj malý I2S DAC modul (PCM5102A, UDA1334A; alebo MAX98357A, čo je DAC
so zosilňovačom pre reproduktor) a prepni naň príkazom `sound out i2s` (uloží sa do
`/SOUND.TXT`, takže platí aj po reštarte; `sound out jack` prepne späť).

| DAC modul | Raspberry Pi |
|-----------|--------------|
| BCK (BCLK) | GPIO18 (pin 12) |
| LCK (LRCK, WS) | GPIO19 (pin 35) |
| DIN (DATA) | GPIO21 (pin 40) |
| VIN / GND | 3,3 V alebo 5 V (podľa modulu) / GND |
| SCK, FMT (PCM5102A) | GND (modul si hodiny vyrobí sám; formát I2S) |

- Tóny (`beep`, `play`, `SOUND`, `tone()`) sú potom sínusové a so skutočnou hlasitosťou.
- Súbory WAV (rovnaké príkazy ako vyššie) hrajú v plnej 16-bitovej kvalite.
- Vzorky idú do DAC cez DMA, takže zvuk hrá, aj keď RoboOS robí niečo iné. `sound` (pri I2S, alebo po WAV) ukáže,
  koľko kúskov sa prehralo a či DMA niekedy vyprázdnilo („restarts“ - vtedy mal zvuk krátku
  medzeru).

## USB klávesnica

Klávesnicu môžeš zapojiť pred zapnutím aj počas behu (hot-plug: porty hubov sa
kontrolujú každých 0,7 s). Pri štarte sa vypíše:

    [ OK ] USB keyboard: <názov>

- **Americké rozloženie (US)**. Funguje Shift, Caps Lock, Num Lock, Ctrl+C a Ctrl+L; LED Caps/Num Lock svietia podľa stavu.
- Numerická klávesnica: číslice a `/ * - + .` pri zapnutom Num Lock (predvolené po štarte);
  pri vypnutom Num Lock 8/2 = história hore/dole, 4/6 = kurzor vľavo/vpravo, 7/1 = Home/End a `.` = Delete.
- Šípky hore/dole = história, vľavo/vpravo/Home/End = kurzor, Backspace, Delete, Tab, autorepeat pri držaní klávesy.
- Podporované sú klávesnice s HID *boot protocol* (prakticky všetky bežné,
  aj klávesnice so zabudovaným hubom ako Apple a herné klávesnice).

Ak sa klávesnica nenájde, spusti na UART konzole:

    usb debug on
    usb reset

Výpis ukáže, kde enumerácia zlyhala.

## UART konzola (voliteľné, USB-TTL prevodník 3,3 V!)

| Prevodník | Pin RPi |
|-----------|---------|
| GND       | pin 6 (GND) |
| RX        | pin 8 (GPIO14 / TXD) |
| TX        | pin 10 (GPIO15 / RXD) |

VCC **nepripájaj**, RPi má vlastné napájanie. Prevodník musí byť 3,3 V,
5 V logika piny poškodí.

Nastavenie terminálu: **115200 baud, 8N1, bez riadenia toku**:
- Windows: PuTTY (Serial, COMx) alebo `plink -serial COM5 -sercfg 115200,8,n,1,N`
- Linux/macOS: `screen /dev/ttyUSB0 115200` alebo `picocom -b 115200 /dev/ttyUSB0`

Po zapnutí sa na HDMI aj v termináli zobrazí logo a prompt `robo@rpi3:~$` (meno závisí
od dosky). Ak sa pripojíš až po štarte, stlač Enter.

## Príkazy

Systém komunikuje po anglicky.

| Príkaz | Popis |
|--------|-------|
| `help` | zoznam príkazov |
| `info` | model, RAM, sériové číslo, MAC, takty, teplota, EL |
| `temp` | teplota CPU |
| `uptime` | čas od zapnutia |
| `echo <text>` | vypíše text |
| `led on\|off\|blink [n]` | zelená ACT LED |
| `gpio` | prehľad GPIO0–27 (funkcia + úroveň) |
| `gpio <pin> in\|out\|hi\|lo\|toggle\|read` | ovládanie pinu |
| `mem <adresa> [dĺžka]` | hexdump pamäte (napr. `mem 0x80000 64`) |
| `usb` | strom USB zariadení (adresa, VID:PID, rýchlosť, názov, stav portov hubov) |
| `usb reset` | znovu inicializuje USB a vyhľadá zariadenia |
| `usb test` | 8 s test klávesnice (štatistika prenosov, surové reporty) |
| `usb debug on\|off` | podrobný výpis USB a reportov klávesnice |
| `net` | stav siete (meno, čip, MAC, linka, počítadlá) |
| `net speed 100\|1000` | Pi 3B+: obmedzí linku na 100 Mbit / povolí 1000 |
| `net who` | zoznam RoboOS dosiek v sieti |
| `net send <meno\|all> <text>` | pošle správu |
| `net name [nové]` | ukáže / zmení meno tejto dosky |
| `net test [s]`, `net loop on\|off` | test rámcov, loopback |
| `ip` | IP adresa, `ip dhcp`, `ip set <adresa> [maska] [brána]` |
| `ping <adresa\|meno> [n]` | ping na iný počítač |
| `http on\|off` | malá webová stránka s údajmi o doske (uloží sa) |
| `http token [<tajne>\|off]` | token pre JSON API (Bearer), pozri časť o JSON API |
| `ls [cesta]` | obsah priečinka na SD karte (bez cesty: aktuálneho) |
| `cd [priečinok]` | zmena priečinka (`..` = vyššie, `/` = navrch, bez argumentu = navrch) |
| `pwd` | ukáže aktuálny priečinok |
| `view [obrázok\|priečinok]` | ukáže obrázky na celú obrazovku (JPEG, PNG, BMP, GIF) |
| `video <súbor.mpg>` | prehrá video (MPEG-1 + MP2) so zvukom |
| `mkdir <priečinok>` | vytvorí priečinok |
| `rm [-f] <súbor\|priečinok>` | zmaže súbor alebo prázdny priečinok (`-f` pre bootovacie súbory) |
| `cat <súbor>` | vypíše textový súbor (`"úvodzovky"` pre mená s medzerami) |
| `edit <súbor>` | celoobrazovkový textový editor |
| `run <program>` | spustí program `.bin` alebo `.bas` |
| `basic [súbor]` | interpreter BASICu |
| `beep [Hz] [ms]` | tón na zvukovom jacku |
| `play <noty>` | zahrá melódiu, napr. `play T120 L4 C D E F G2` |
| `sound [volume n\|out jack\|gpio\|i2s\|off]` | výstup a hlasitosť zvuku |
| `playwav <súbor>` | prehrá súbor WAV (jack, GPIO18/19 alebo I2S DAC) |
| `sd [init]` | info o SD karte / znovu inicializovať po výmene karty |
| `screen [ŠxV\|auto]` | rozlíšenie obrazovky (uloží sa do `/SCREEN.TXT`) |
| `clear` | vymaže obrazovku (aj Ctrl+L) |
| `reboot` | reštart cez watchdog |
| `halt` | zastaví systém |

Editor riadku (UART aj klávesnica, v shelli aj v BASICu): **šípky vľavo/vpravo**, **Home/End**
(alebo Ctrl+A / Ctrl+E) hýbu kurzorom, takže sa dá vkladať a mazať kdekoľvek v riadku.
**Tab** doplní príkaz a za ním meno súboru alebo priečinka (mená s medzerou dostanú
"úvodzovky"; pri viacerých možnostiach ich vypíše), **šípky hore/dole** listujú
históriu, **Ctrl+C** zruší riadok. Neplatný prístup do pamäte (napr. `mem 0xdeadbeef00`)
vyvolá výnimku; tá sa vypíše a systém sa vráti do shellu.

## Ako to funguje

Raspberry Pi 3 má štyri jadrá procesora. RoboOS používa všetky:

- **jadro 0**: shell, BASIC a programy, SD karta, obrazovka a zvuk;
- **jadro 1**: celé USB (klávesnica, hot-plug, ethernetový čip) a sieť (TCP/IP, správy
  RoboOS) - stále, nech jadro 0 robí čokoľvek;
- **jadrá 2 a 3**: pomocníci - jadro 0 im dá kus práce (zatiaľ prepočet snímok videa)
  a keď nemajú čo robiť, spia (`wfe`).

`info` ukáže, ako sa jadrá využívajú. Ak sa jadro 1 nespustí, jadro 0 robí všetko samo ako
v starších verziách (hlási to riadok pri štarte); bez pomocníkov prepočíta video jadro 0.

| Súbor | Čo robí |
|-------|---------|
| `src/boot.S` | prechod EL2→EL1, zásobníky, vektory výnimiek, BSS; vstup pre jadrá 1-3 |
| `src/smp.c` | prebudí jadro 1 (USB + sieť) a pomocníkov 2-3 (`smp_run` / `smp_wait`), zámky zdieľané jadrami |
| `src/mmu.c` | identické mapovanie 2 MB blokmi, zapnutie D/I cache |
| `src/mbox.c` | mailbox ku GPU firmvéru (info o doske, takty, teplota, framebuffer) |
| `src/uart.c` | PL011 UART0 na GPIO14/15 |
| `src/fb.c` | HDMI framebuffer (1024×768 alebo `screen`), dve stránky na page flip, textová konzola s fontom 8×16 |
| `src/console.c` | výstup na UART + HDMI, `kprintf`, farby, vstup |
| `src/usb.c` | USB host ovládač DWC2: inicializácia, root port, DMA prenosy, split transakcie, enumerácia, huby, hot-plug |
| `src/eth.c` | Ethernet: LAN9514 (3B) a LAN7515 (3B+) cez USB bulk endpointy, surové rámce |
| `src/net.c` | správy medzi RoboOS doskami (mená, who, odoslanie s potvrdením, schránka) |
| `src/ip.c` | TCP/IP: port lwIP, sieťové rozhranie, DHCP, `ip`, `ping`, `http`, sockety pre programy |
| `src/webapi.c` | JSON API za `http`: Bearer token, GPIO, stav, reštart |
| `lwip/` | TCP/IP stack lwIP 2.2.1 (bez úprav, licencia BSD) |
| `src/kbd.c` | HID klávesnica: boot protocol, preklad scan kódov, autorepeat |
| `src/sd.c` | ovládač SD karty (Arasan SDHCI, GPIO48-53 ALT3), čítanie aj zápis |
| `src/fat.c` | FAT16/FAT32: dlhé mená, čítanie, zápis (mená 8.3), mkdir, mazanie, obe kópie FAT |
| `src/app.c` | nahrávanie programov, API v štýle Arduina, Ctrl+C, autostart z `/RUN` |
| `src/basic.c` | interpreter BASICu |
| `src/ctx.S` | setjmp/longjmp na ukončenie programov |
| `src/board.c` | rozpoznanie dosky: názov, ACT LED, zapojenie USB |
| `src/lineedit.c` | editor riadku: kurzorové klávesy, história, Tab (shell + BASIC) |
| `src/edit.c` | celoobrazovkový textový editor |
| `src/shell.c` | príkazy, dopĺňanie cez Tab |
| `src/path.c` | aktuálny priečinok, relatívne cesty, `.` a `..` |
| `src/sound.c` | tóny z PWM (zvukový jack alebo GPIO18/19), noty |
| `src/i2s.c` | zvuk cez DMA: I2S DAC (periféria PCM) alebo PWM, sínusové tóny, súbory WAV |
| `src/image.c` | obrázky: dekódovanie (stb_image v `src/stbimg.c`), zmenšovanie, `view`, obrázok pri štarte |
| `src/video.c` | prehrávač videa: pl_mpeg (`third_party/pl_mpeg.h`) číta z karty, prepočet farieb na jadrách 2-3, zvukový prúd |
| `src/heap.c` | pamäť na veľkú dočasnú prácu (dekódovanie obrázkov) |
| `src/font.c` | font vygenerovaný z DejaVu Sans Mono (`tools/mkfont.py`) |

### Ako funguje USB

Na RPi 3 nie je klávesnica pripojená priamo, cesta vyzerá takto:

    radič DWC2 (v SoC) -> hub LAN9514 (HS 480 Mbit) -> klávesnica (LS 1,5 / FS 12 Mbit)

1. **Inicializácia radiča**: zapnutie cez mailbox, reset, host režim, DMA.
2. **Root port**: reset portu, zistenie rýchlosti.
3. **Enumerácia**: deskriptor, `SET_ADDRESS`, konfigurácia; rekurzívne cez huby.
4. **Hub**: napájanie portov, reset, rýchlosť zariadenia na porte.
5. **Split transakcie**: pomalé zariadenie za HS hubom sa oslovuje cez
   *start-split* + *complete-split* a Transaction Translator v hube.
6. **Klávesnica**: HID report deskriptor, `SET_PROTOCOL(boot)`, `SET_IDLE`, potom čítanie
   každých 10 ms. Kým klávesnica nepošle prvú stlačenú klávesu, striedajú sa interrupt IN
   a `GET_REPORT`; spôsob, ktorý funguje, sa zamkne (`usb` ukáže režim).
7. **Periodické split transakcie**: start-split v mikrorámci N, complete-split
   v N+2…N+4 (USB 2.0, kap. 11.18), inak TT dáta zahodí.
8. **Postupné zapínanie portov hubu**: hub v Apple klávesnici pri zapnutí
   všetkých portov naraz spadol (prúdový náraz) a odpojil sa.

Ovládač beží bez prerušení (polling, na jadre 1) a používa jeden DMA kanál (a druhý pre
sieťové adaptéry, ktoré nechávajú prenos čakať). DMA buffre ležia
v 2 MB bloku mapovanom bez cache.

Dosky bez hubu na doske (Zero 2 W, 3A+) majú klávesnicu priamo na root porte;
vtedy netreba split transakcie a hot-plug sleduje aj root port.

## Kompilácia

Linux / WSL s clang + lld:
```
make            # vytvorí kernel8.img
make apps       # preloží ukážky zo SDK do sd/APPS
make run        # spustí v QEMU (qemu-system-aarch64 -M raspi3b)
```
Alebo s ARM GNU toolchainom (aj vo Windows):
```
make TOOLCHAIN=gcc CROSS=aarch64-none-elf-
```

Testovanie v QEMU s HDMI oknom, USB klávesnicou a obrazom SD karty:
```
make sd.img
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -device usb-kbd \
    -drive file=sd.img,if=sd,format=raw
```
So sieťou (QEMU emuluje USB sieťový adaptér, DHCP dá adresu 10.0.2.15):
```
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -device usb-kbd \
    -drive file=sd.img,if=sd,format=raw \
    -device usb-net,netdev=n0 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:80
```
(po `http on` je stránka dosky na PC na http://127.0.0.1:8080/).


## Nápady do budúcna
- ďalšie rozloženia klávesnice (aj font s diakritikou)
- prerušenia + časovač → multitasking
- I2C / SPI / PWM (servá, stmievanie LED) v API aj v BASICu
- zvuk: MP3, miešanie tónov s WAV
- editor: hľadanie, skok na riadok, uložiť ako
- vzdialený shell (ako telnet), IPv6
- jadrá 2 a 3 pre programy (spustiť funkciu na inom jadre)
- video: playlist / celý priečinok, časová lišta

## Poďakovanie

- Firmvér (`bootcode.bin`, `start.elf`, `fixup.dat`) je od Raspberry Pi Ltd,
  licencia v `sd/LICENCE.broadcom`.
- Font konzoly je vykreslený z DejaVu Sans Mono (licencia Bitstream Vera).
- Inicializácia SD karty vychádza zo známych bare-metal návodov
  (bztsrc/raspi3-tutorial); časovanie periodických split transakcií podľa Circle/USPi.
- TCP/IP: lwIP od Adama Dunkelsa a vývojárov lwIP (licencia BSD, `lwip/COPYING`).
- Obrázky: stb_image od Seana Barretta (public domain / MIT, `third_party/stb_image.h`).
- Video: pl_mpeg od Dominica Szablewského (MIT, `third_party/pl_mpeg.h`).
- Registre LAN7515 (Pi 3B+) podľa linuxového ovládača `lan78xx`.
