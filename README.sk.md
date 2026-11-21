# RoboOS

[English](README.md) | **Slovensky**

![Štart RoboOS](docs/screenshot.png)

Mini operačný systém pre Raspberry Pi, ktorý beží priamo na hardvéri, bez Linuxu.
Ovláda sa **USB klávesnicou** alebo cez sériovú linku (UART). Výstup ide naraz na
HDMI aj UART a oba vstupy fungujú súčasne.

Aktuálna verzia: **0.73**

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
   a priečinky `APPS` (ukážkové programy), `RUN` (autostart) a `FIRMWARE` (firmvér WiFi čipu).
3. Vlož kartu do Raspberry Pi a zapni ho.

## Programy a autostart

SD karta je zároveň disk RoboOS (FAT32 alebo FAT16, čítanie aj zápis).
RoboOS má vlastný ovládač SD (firmvér len načíta `kernel8.img` a potom ustúpi).
Ak sa karta nenájde, `sd init` ukáže, ktorý krok zlyhal, a registre radiča.
Od verzie 0.28 beží karta na radiči SDHOST (ako v Linuxe), aby druhý radič (Arasan) ostal
neskôr voľný pre WiFi; ak SDHOST nefunguje, RoboOS použije Arasan ako predtým a pri štarte
ukáže žltý riadok `SDHOST:` s dôvodom. `sd` ukáže, ktorý sa používa, `sd init sdhost` /
`sd init emmc` skúsi jeden z nich.

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
video: `playVideo(path)`, `videoError`, súbory: `readFile`, `writeFile`
(viď `sdk/robos.h`). Program môže mať `setup()` + `loop()` alebo `main()`.
Ukážky: `hello`, `blink`, `button`, `knight`, `chat`, `tcpserv` (TCP server), `udpecho` (UDP),
`music` (pesnička a siréna), `alarm` (pozri nižšie).

### Ukážka: alarm (`alarm`)

`run alarm` spustí malý zabezpečovací systém (`sdk/examples/alarm.c`); skopíruj ho do `/RUN`, aby
štartoval sám. Nastavenia sú v `/ALARM.TXT` (pri prvom štarte sa vytvorí s predvolenými):

```
code 1234                    # PIN na zapnutie a vypnutie
exit 20                      # sekundy na odchod po zapnutí
entry 15                     # sekundy na vypnutie po otvorení zóny "delay"
siren 180                    # sekundy sirény (0 = kým sa nevypne)
siren_pin 24                 # výstup: siréna / relé (HIGH = zapnutá)
armed_pin 23                 # výstup: LED - svieti = zapnutý, bliká = odchod / príchod, rýchlo = poplach
sound on                     # pípanie, tón sirény a hlas na audio jacku
volume 80
voice /ALARM                 # priečinok s hlasovými WAV ("none" = len tóny)
port 8080                    # TCP port pre JSON (0 = vypnuté)
zone 17 delay nc Front door  # zone <gpio> <instant|delay|24h> <nc|no> <meno>
zone 27 instant nc Window
zone 22 instant no PIR hall
zone 5 24h nc Tamper
```

- **Vstupy** majú zapnutý vnútorný pull-up: kontakt zapoj medzi GPIO a GND. `nc` = rozpínací
  (dverový kontakt: otvorené = poplach), `no` = spínací (relé PIR, panické tlačidlo: zopnuté =
  poplach). `instant` = poplach hneď, keď je zapnutý, `delay` = spustí príchodový čas (cesta dnu,
  počas odchodového času sa nevšíma), `24h` = poplach vždy, aj vypnutý (sabotáž, panika).
  Kým je otvorená zóna instant / 24h, zapnúť sa nedá.
- **Sériový port / klávesnica**: `arm 1234`, `disarm 1234`, `status` (kód sa ukazuje ako `*`).
  Každá udalosť sa vypíše ako `[ALARM hh:mm:ss] ...` na sériový port aj na obrazovku.
- **JSON** na TCP porte: jeden riadok cez `nc <doska> 8080`, alebo HTTP
  (`curl -d "{\"cmd\":\"disarm\",\"code\":\"1234\"}" http://<doska>:8080/`; GET = stav):
  `{"cmd":"arm","code":"1234"}`, `{"cmd":"disarm","code":"1234"}`, `{"cmd":"status"}`.
  Odpoveď obsahuje stav, sirénu, zostávajúce sekundy, príčinu a všetky zóny.
- **Zvuk**: pípnutie každú sekundu odchodového / príchodového času, posledných 10 sekúnd
  hlasom, siréna ako stúpajúci a klesajúci tón a hlásenia udalostí. Na karte sú slovenské
  hlasové súbory v `/ALARM` (`ARMING`, `ARMED`, `ENTRY`, `ALARM`, `DISARMED`, `WRONG`, `NOARM`,
  `1`-`10` `.WAV`) vyrobené cez `tools/mkvoice.sh sk` s espeak-ng (`tools/mkvoice.sh en` po
  anglicky) - nahraď ich vlastnými nahrávkami. Hlasový súbor sa dohrá, až potom alarm pokračuje.
- Päť zlých kódov zablokuje kód na 60 s. Je to ukážka: Ctrl+C na doske ju ukončí.

Programy vedia od API verzie 8 čítať a zapisovať súbory: `readFile(path, buf, max)`,
`writeFile(path, data, len)`.

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
na doske; RoboOS má ovládač pre oba (oba fungujú: DHCP, NTP). 3B sa pripojí rýchlosťou 100 Mbit, 3B+ 1000 Mbit
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
- `ip` počíta aj rámce: `in` (ARP, IPv4, iné, s VLAN značkou) a `out` (chyby) - ak
  `in` ostáva blízko 0, k doske nič nechodí (kábel, switch, router). Rámcom s VLAN značkou
  (802.1Q, aj „len priorita“, ktorú pridávajú niektoré routery) sa značka odstráni.
- `http on` spustí malú webovú stránku s menom dosky, verziou, dobou behu a teplotou
  CPU: otvor `http://<adresa>/` v prehliadači. `http off` ju vypne. Voľba sa uloží do
  `/HTTP.TXT`, takže stránka (aj API nižšie) sa po reštarte spustí sama.
- Len IPv4.

### WiFi (vo výstavbe)

WiFi čip (CYW43438 na Pi 3B, CYW43436 na Zero 2 W, CYW43455 na Pi 3B+) visí na SDIO. Zatiaľ ho
RoboOS spustí, hľadá siete a pripojí sa (WPA2-PSK alebo otvorená sieť):

```
wifi scan                     nahrá firmvér do čipu a vypíše siete okolo
wifi join MojaSiet heslo      pripojí sa (uloží sa do /WIFI.TXT: po reštarte sa pripojí sama)
wifi join "Moja Siet" heslo   meno s medzerami
wifi                          čip, firmvér, MAC adresa, sieť, signál
wifi band 2|5|auto            len 2,4 GHz / len 5 GHz / obe (len Pi 3B+ - 3B a Zero 2 W majú len 2,4 GHz; uloží sa; pri sieti na oboch pásmach
                              vyberá firmvér, 'wifi' ukáže kanál, na ktorý sa pripojil)
wifi leave                    odpojí sa (a prestane sa pripájať znova);  wifi forget  zmaže aj /WIFI.TXT
```

WPA2 handshake robí firmvér čipu sám. Jedno IP rozhranie nesie všetko (DHCP, web, API, HTTPS,
NTP, programy): **Ethernet, keď je zapojený kábel, inak WiFi** - vytiahnutie kábla prepne na
WiFi (`[NET] now on WiFi`, nová adresa cez DHCP). `ip` ukáže, ktoré sa používa. Zero 2 W
Ethernet nemá, takže WiFi je jej sieť.

Čip potrebuje svoj firmvér na karte v `/FIRMWARE` - je v `sd/FIRMWARE`, takže stačí skopírovať
`sd/` (`tools/getwifi.sh` stiahne čerstvú kópiu): `brcmfmac43430-sdio.bin` + `.txt` + `.clm_blob` (Pi 3B; novší
firmvér bez `.clm_blob` nezapne rádio),
`brcmfmac43436-sdio.bin` + `.txt` + `.clm_blob` (Zero 2 W), `brcmfmac43455-sdio.bin` + `.txt` +
`.clm_blob` (Pi 3B+). Tieto súbory nie sú súčasťou RoboOS: sú to nezmenené kópie z balíka
firmware-nonfree od Raspberry Pi pod licenciami Cypress / Synaptics, ktoré ich dovoľujú šíriť na
použitie s ich čipmi - pozri `sd/FIRMWARE/README.txt` a súbory `LICENCE.*` vedľa nich. WiFi potrebuje
SD kartu na SDHOST (pozri časť o SD karte). Na Pi 3B funguje scan aj pripojenie; na Pi 3B+ funguje scan
(2,4 aj 5 GHz). Firmvér Pi 3B+ nemá zabudovaný WPA supplicant, preto od 0.36 robí WPA2-PSK handshake
sám RoboOS (`src/wpa.c`: PBKDF2, 4-way a skupinový handshake, AES key unwrap; overené na PC proti
testovacím vektorom IEEE 802.11 a prístupovému bodu v Pythone, `tools/test_wpa.py`) - `wifi join` potom
napíše "RoboOS does the WPA2 handshake" (len informácia). Ak sa pripojenie nepodarí (pri štarte, po strate signálu, po reštarte routera), doska to skúša znova sama:
po 20 s, potom raz za minútu (`[NET] WiFi: joining <sieť> again`), kým nedáš `wifi leave`. Na Pi 3B+ to **funguje** (pripojenie, DHCP aj ping cez
WiFi s vytiahnutým káblom). Siete len s WPA3 (SAE) nie sú podporované (zmiešané WPA2/WPA3 áno). **Funguje aj Zero 2 W** (štart, USB klávesnica cez OTG, WiFi).

### Bluetooth (BLE: senzory, mobil)

WiFi čip je zároveň Bluetooth čip. Zapne sa, až keď ho zavoláš:

```
bt on            zapnúť: počúva zariadenia okolo a mobil ho nájde
bt scan          čo je okolo (bt scan 20 = počúvať 20 s)
bt sensors       Bluetooth teplomery ako JSON
bt pair          spárovať Bluetooth klávesnicu (klasický BT; bt pair aa:bb:cc:dd:ee:ff = konkrétnu)
bt forget        zabudnúť spárovanú klávesnicu
bt               stav (pripojený mobil, zariadenia, teplomery)
bt off           vypnúť
bt auto on|off   zapnúť pri štarte (/BT.TXT)
```

**Senzory.** Teplomery Xiaomi LYWSD03MMC s voľným firmvérom ATC / pvvx (service data 0x181A) sa
čítajú stále, kým je Bluetooth zapnutý: `bt sensors`, `GET /api/ble` a MQTT `roboos/<meno>/ble/<mac>` =
`{"temp":21.53,"hum":45.12,"batt":87,"rssi":-63,"name":"ATC_112233"}` (najviac raz za 30 s), Home
Assistant ich nájde sám (teplota, vlhkosť, batéria). `bt scan` ukáže aj mená a výrobcov všetkého
ostatného (mobily, TV, slúchadlá). Od 0.43 hľadá aj klasické Bluetooth zariadenia (označené `BR`)
a označí klávesnice a myši: `[keyboard, BLE]` alebo `[keyboard, classic BT]` - tak zistíš, akú máš
klávesnicu (lacné bývajú klasický Bluetooth 3.0, ten potrebuje iný ovládač ako BLE).

**Mobil.** Doska sa ukáže pod svojím menom (`rpi3-...`) so službou Nordic UART. Pripoj sa terminálovou
aplikáciou - Android: *Serial Bluetooth Terminal* (Devices -> Bluetooth LE), iPhone / Android:
*nRF Toolbox* (UART) alebo *Bluefruit Connect* (UART) - a píš:

```
help                          príkazy
status                        meno, verzia, čas behu, adresa, čas
sensors                       CPU, BME280, ADS1115 a Bluetooth teplomery
gpio 17                       prečíta pin;  gpio 17 1 / gpio 17 0 / gpio 17 toggle = nastaví
login robo MojeHeslo1         treba, keď sú používatelia ('users add'); "view" len číta
```

Párovanie zatiaľ nie je: spojenie nie je šifrované, používaj ho nablízku (heslo napísané tam ide
vzduchom čitateľne). Kým je Bluetooth zapnutý, UART konzola beží na mini UARTe - rovnaké piny 8/10,
rovnakých 115200; `bt off` ju vráti. Scan funguje na Pi 3B (19 zariadení na prvý pokus) aj na Zero 2 W (12 zariadení); časť s mobilom
je vyskúšaná v QEMU (`tools/qemu_bt_test.py` hrá radič aj mobil). 
**Klávesnica (0.44, klasický Bluetooth).** Prepni klávesnicu do režimu párovania (zvyčajne podržať Connect
alebo Fn+C, kým nebliká LED) a napíš `bt pair`. RoboOS ju nájde, pripojí sa a ukáže číslo:
`on the keyboard type 123456 and press Enter` - napíš ho **na Bluetooth klávesnici**. Potom píše do shellu
ako USB klávesnica (US rozloženie, autorepeat). Kľúč sa uloží do `/BTKEYS.TXT`: po reštarte (s `bt auto on`)
alebo keď klávesnica zaspala, stlač kláves a pripojí sa sama. `bt` ukáže stav, `bt forget` kľúč zmaže.
BLE klávesnice zatiaľ nie. Čip na 3B+ má zástupnú Bluetooth adresu (43:45:c0:00:1f:ac, rovnakú na každej
doske); od 0.44 mu RoboOS dá vlastnú (b8:27:eb + sériové číslo, ako Raspberry Pi OS). **Funguje na Pi 3B+** s lacnou
klasickou BT klávesnicou (párovanie, písanie, pripojenie po reštarte); 0.45 nechá klávesnicu, nech sa pri
pripájaní overí sama (0.44 to krížila: chyba 0x05, jeden pokus navyše). 0.45 tiež nahráva firmvér Bluetoothu rýchlosťou
921600 baudov (8x rýchlejšie: `bt auto on` už nepredĺži boot o ~6 s) a pripojenie WiFi pri štarte robí I/O jadro,
takže WPA handshake routera dostane odpoveď hneď (predtým sa 3B+ často pripojila až na pokus o 20 s neskôr).
0.46: Bluetooth na pozadí počúva len 10 % času (30 ms z 300 ms, `bt scan` 50 %) - WiFi a Bluetooth zdieľajú
jedno rádio a 50 % hľadanie na 3B+ kazilo WPA handshake až do 3. pokusu; WiFi pri ďalšom pokuse vypíše, prečo
predošlý zlyhal. 0.49: ďalší pokus nikdy nepreruší ten, ktorý sa ešte pripája alebo robí WPA handshake (0.46 skúšala
znova po 10 s, a preto sa pripojila až na 4. pokus): po chybe o 2 s, inak o 20 s; vypíše aj, ako dlho pokus trval
(`connected to ... (N s)`). 0.50: na 3B+ (WPA handshake robí RoboOS) sa ignoruje WPA hlásenie
firmvéru - 0.49 ho brala ako chybu a reštartovala pripájanie uprostred handshake (10 pokusov). 0.51: `wifi` vypíše posledných 10 pokusov s časom
každého kroku (sieť nájdená, spojenie, WPA msg1/msg3, kľúče, strata + dôvod) - jedna fotka ukáže, kde sa pripájanie zastaví. 0.52: router
odpovedal na rýchle pokusy odpojením s dôvodom 2 („predošlé prihlásenie už neplatí“) 20-krát: teraz sa pred každým
ďalším pokusom Pi najprv korektne odpojí (disassociate, 0,3 s) a po odmietnutí skúša znova o 15 s, nie o 2 s. Na Pi 3B+ sa WiFi teraz pripojí na prvý pokus do sekundy.
0.53: s `bt auto on` a nastavenou WiFi sa Bluetooth zapne až po pripojení WiFi (najneskôr o 15 s) - zdieľajú
jedno rádio a štart Bluetoothu predĺžil pripojenie WiFi pri štarte na 20 s. QEMU test: `tools/qemu_btkbd_test.py`.
0.54: `history` vypíše posledných 30 príkazov (predtým 10), `!N` / `!!` spustí príkaz znova, `history clear` ju zmaže; história
sa ukladá do `/HISTORY.TXT` na karte, takže prežije reštart. WiFi: odmietnutý PRVÝ pokus po štarte sa opakuje o 3 s (nie o 20 s), rovnako prvý pokus,
kde WPA handshake zastal (msg2 odoslaná, msg3 do 5 s neprišla) - na fotke vlastníka router pri štarte na našu msg2 neodpovedal
a po 18 s Pi odpojil, druhý pokus sa potom pripojil za 3 s.
0.55: I2C displeje - OLED SSD1306 / SH1106 a LCD 16x2 / 20x4 s modulom PCF8574, nájdu sa pri štarte, stavová obrazovka
(meno, IP, čas), `display`, BASIC `DISPLAY`, C API 11 (pozri „I2C displej“).
0.56: `ai` - otázky pre Claude s vlastným API kľúčom (pozri „AI“); riadok shellu až 255 znakov; viac koreňových
certifikátov pre HTTPS (DigiCert G5, SSL.com, Certainly, Microsoft, IdenTrust).
0.57: OLED displeje, ktoré na I2C nepotvrdzujú (ACK) - 2,42" dosky SSD1309 s otvoreným D2 (na ESP išli, jeho softvérové I2C
ACK ignoruje): `display type ssd1306` ich použije aj tak, zápis softvérovým I2C na GPIO2/3.
Nájdu sa aj OLED radiče, ktoré odpovedajú na zápis, ale nie na čítanie (SSD1309): `i2c scan` a hľadanie displeja
skúšajú 0x3C/0x3D aj zápisom.
0.58: písma displeja (small 21 x 8 / normal / big), `display console on` (kópia terminálu), text cez sieť
(`/api/display`, MQTT `set/display`). 2,42" OLED na 3B+ funguje (vlastník: ukazuje IP aj čas).
0.59: `display font tiny` = 32 x 10 znakov (X11 4x6, voľné použitie); OLED sa kreslí do obrázka v pamäti a posielajú
sa len zmenené 8-bodové pásy. Riadky 1-10 v BASICu, API aj MQTT.
0.60: `display time` = veľké digitálne hodiny (na 128x64 aj s dátumom), `display info` = stavová obrazovka;
`display console off` sa vráti na stavovú obrazovku (v 0.59 nechal displej prázdny). Režim sa uloží.
0.61: hodiny tenkými číslicami (DejaVu Sans, `tools/mkclock.py`) namiesto hrubých 3x zväčšených, sekundy menšie
hneď vedľa HH:MM, dátum písmom 8x16 pod nimi.
0.62: `font tiny` je teraz čitateľnejšie X11 5x8 (25 x 8); stiesnené 4x6 (32 x 10) je `font micro`.
0.63: nezašifrovaný API kľúč zapísaný do `/AI.TXT` na PC (`key sk-ant-...`) sa prijme a pri prvom `ai` zašifruje.
0.64: aj samotný kľúč v `/AI.TXT`, alebo `API_KEY="sk-ant-..."` - hocijaký riadok s `sk-ant-`.
0.65: `ai` vypisuje každý krok (pripájanie, TLS, odosielanie, čakanie, čítanie, s ms) - vlastníkovi to zastalo
po "asking ..." a nebolo vidno kde.
0.66: viac na hľadanie zamrznutia: riadok odpovede ukáže HTTP stav, dĺžku a chunked; počas čakania každých 5 s
"still waiting: N bytes in so far, core 1: <čo robí> (loops)".
0.67: OPRAVA zamrznutia `ai`: po odpovedi po kúskoch (chunked; API vrátilo HTTP 429) sa BearSSL pri slušnom
ukončení TLS (`br_sslio_close`) zacyklil navždy - zopakované v QEMU; HTTPS klient teraz len zatvorí TCP spojenie.
Pri 429 napíše, že ide o limit volaní / výdavkov alebo kredit.
0.68: OPRAVA: `i2c scan` „našiel“ 0x3c / 0x3d aj bez ničoho pripojeného (skúška zápisom pre OLED): neskoré
chýbajúce ACK sa zachytí a zápisu sa verí, len ak rezervovaná adresa 0x7c NEodpovie. `i2c scan` aj upozorní,
ak sú SDA / SCL v pokoji nízko (skrat, prehodené vodiče, nenapájaný modul).
0.69: na prázdnej zbernici vlastníka stále fantóm 0x3c -> skúška zápisom musí odpovedať 3-krát po sebe, `i2c scan`
označí také nálezy "[answers writes only]"; `i2c probe <adresa>` ukáže surový stav radiča + čas čítania a zápisu;
`i2c speed <kHz>` (10-400, do reštartu) - LCD moduly (PCF8574) zvládajú najviac 100 kHz.
0.70: uvoľnenie zaseknutej I2C zbernice - LCD modul vlastníka držal SDA dole ("SDA LOW" v `i2c scan`, nič sa
nenašlo), kým ho neuvoľnil neúspešný prenos; teraz pri štarte, pred `i2c scan` a po vypršaní času RoboOS pošle až
9 hodinových impulzov + STOP, keď je SDA zaseknutá dole (`i2c scan` to napíše).
0.71: FM rádio - `radio` pre modul RDA5807M (ladenie, hľadanie, zoznam staníc, hlasitosť, RDS názov + text,
`display radio`, /api/radio, MQTT set/radio); MPU-6050 na 0x68 sa už nepovažuje za hodiny DS3231 (kontrola WHO_AM_I).
0.72: Waveshare 1,3" OLED HAT (SH1106 na SPI0 + 3 tlačidlá + joystick) ako panel rádia: `display hat`, `display keys`;
nový ovládač SPI0 `src/spi.c`.
0.73: senzor tlaku a teploty BMP180 (BMP085) na 0x77 v `sensors`, MQTT, API a BASIC `SENSOR()`.

### Dátum a čas (NTP)

Keď sieť beží, doska sa spýta NTP servera na čas (hneď a potom každú hodinu) a vypíše
`[NET] time 2026-11-18 21:05:07 (NTP pool.ntp.org)`. Nastavenia sú v `/TIME.TXT`:

```
time                     dátum, čas, pásmo a kedy sa synchronizoval
time zone +1 eu          posun od UTC (ide aj +5:30); eu = letný čas EÚ, bez neho žiadny
time server pool.ntp.org NTP server ('off' = žiadny)
time sync                spýtaj sa hneď
time set 2026-11-18 21:05   nastav ručne (miestny čas), napr. bez siete
```

Čas je v `GET /api/status` (`"time":"2026-11-18T21:05:07+01:00"`, `"unix"`), na webovej stránke,
v BASICu (`PRINT DATE$; " "; TIME$`, `HOUR`, `MINUTE`, `SECOND`, `UNIXTIME`), v C (`unixTime()`,
`timeText(buf)`, API verzia 9) a v hláseniach ukážky alarm. Pi nemá hodiny s batériou: po výpadku
prúdu sa čas vráti so sieťou (alebo cez `time set`).

### HTTPS (TLS)

`http tls on` zapne HTTPS na porte 443 (uloží sa do `/HTTP.TXT`, po reštarte sa zapne samo).
Potom JSON API odpovedá **len cez HTTPS** - na porte 80 povie, nech sa použije `https://` - takže
token nikdy nejde sieťou otvorene. `http tls off` ho vypne, `http tls new` vyrobí nový certifikát.

- Prvé `http tls on` vyrobí ECDSA P-256 kľúč a **samopodpísaný certifikát** pre meno a adresu
  dosky (platný 20 rokov): `/TLS/KEY.PEM` a `/TLS/CERT.PEM`. `http` ukáže jeho SHA-256 odtlačok.
  Prehliadač raz varuje („nie je zabezpečené“ - nevie, kto ho vydal); curl potrebuje `-k`, alebo
  lepšie: skopíruj `CERT.PEM` na PC a dôveruj presne tomuto certifikátu:

  ```
  curl --cacert CERT.PEM -H "Authorization: Bearer tajne123" https://192.168.1.50/api/status
  ```
- Vlastný certifikát (napr. z vlastnej CA, alebo `openssl req -x509 -newkey ec -pkeyopt
  ec_paramgen_curve:P-256 -nodes -keyout KEY.PEM -out CERT.PEM -days 3650 -subj /CN=robo`):
  daj `CERT.PEM` (PEM alebo DER, aj s reťazcom) a `KEY.PEM` (EC alebo RSA) do `/TLS`.
- TLS 1.2 s ECDHE a AES-GCM / ChaCha20 cez [BearSSL](https://bearssl.org) 0.6 (MIT); spojenie
  trvá v QEMU asi 40 ms. Naraz najviac 4 HTTPS spojenia; relácie sa obnovujú.

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
| `POST /api/sound` | prehrá WAV z priečinka `/SOUNDS`: `{"file":"dingdong.wav","out":"jack"}` - `out` je `jack`, `gpio` (GPIO18/19) alebo `i2s` (DAC modul, aj `"dac"` / `"device"`), bez neho aktuálny výstup (`sound out`); voliteľne `"volume":0-100`. `{"tone":880,"ms":500}` zahrá tón, `{"stop":true}` zastaví. Odpovie hneď kódom 202 |
| `GET /api/sound` | čo hrá, výsledok posledného zvuku, výstup, hlasitosť a súbory v `/SOUNDS` |
| `POST /api/reboot` | odpovie `{"ok":true,"rebooting":true}` a o 0,5 s reštartuje |

```
curl -H "Authorization: Bearer tajne123" http://192.168.1.50/api/status
curl -H "Authorization: Bearer tajne123" -d '{"pin":17,"value":1}' http://192.168.1.50/api/gpio
curl -H "Authorization: Bearer tajne123" -X POST http://192.168.1.50/api/reboot
```

Zvuky: WAV súbory (PCM 8/16 bitov, 8-48 kHz) daj na kartu do `/SOUNDS` (sú tam
`DINGDONG.WAV`, `BEEP.WAV`, `SIREN.WAV`). Prehrá ich jadro 0, keď čaká v shelli, BASICu alebo
programe (SD karta patrí jadru 0), nie počas `playwav` ani videa; výstup sa potom prepne späť
a voľba v `/SOUND.TXT` ostane.

```
curl -H "Authorization: Bearer tajne123" -d "{\"file\":\"dingdong.wav\",\"out\":\"i2s\"}" http://192.168.1.50/api/sound
```

Chyby prídu ako `{"ok":false,"error":"..."}` s kódom 400 / 401 / 403 / 404 / 405. API môže
volať aj webová stránka z inej adresy (CORS je povolený). Doska vypíše
`[API] reboot asked by <adresa>` a `[API] wrong or missing token from <adresa>`.
**HTTP nie je šifrované**: kto vidí do tvojej siete, prečíta aj token, takže API používaj
len vo vlastnej sieti, nie z internetu. Piny 14/15 sú UART.

### Aktualizácia cez sieť (kernel a súbory, 0.47)

S `http on` a tokenom (`http token <tajne>`) alebo prihlásením admina sa nový kernel dostane do Pi
bez vyberania karty:

```
curl --data-binary @kernel8.img -H "Authorization: Bearer <tajne>" http://<doska>/api/update
curl --data-binary @ALARM.BIN -H "Authorization: Bearer <tajne>" "http://<doska>/api/file?path=/APPS/ALARM.BIN"
```

(`https://` a `curl -k`, keď je zapnuté HTTPS; `-u meno:heslo` pre prihlásenie používateľa.) Kernel sa
najprv skontroluje: musí to byť kernel RoboOS (nesie značku `ROBOOS-KERNEL:<verzia>`), takže zlý súbor
nikdy nenahradí bežiaci kernel. Starý kernel sa skopíruje do `KERNEL8.BAK`, nový sa zapíše do nových
klastrov, položka v adresári sa prepne naň, prečíta sa späť a porovná - potom príde odpoveď
`{"ok":true,"old":"0.46","new":"0.47",...}` a Pi sa reštartuje (`?reboot=0` = nereštartovať). Najviac 16 MB.
`update` ukáže kernel na karte a zálohu, `update restore` vráti zálohu. Ak by nový kernel vôbec nenaštartoval,
vyber kartu a premenuj `KERNEL8.BAK` na `kernel8.img`. Vyskúšané v QEMU (HTTP + HTTPS, zlý token, súbor,
ktorý nie je kernel, restore, reštart) - na hardvéri ešte nie.

**Pi si ho stiahne samo (0.48).** `update check` stiahne kernel a povie, aká je to verzia; `update get` ho
rovnako bezpečne nainštaluje a reštartuje sa. Predvolene ide z tohto repozitára
(`https://raw.githubusercontent.com/didi2sk/RoboOS/main/sd/kernel8.img`); inú adresu uloží do `/UPDATE.TXT`
`update url https://...` (PC v domácej sieti: `http://192.168.1.10:8000/kernel8.img`, napr. `python -m http.server`).
Certifikáty HTTPS sa overujú voči vstavaným koreňovým autoritám (Let's Encrypt, DigiCert, Sectigo, Google,
GlobalSign, Amazon; `tools/mkta.py`) a `/TLS/CA.PEM` (vlastná autorita) - Pi na to potrebuje čas (NTP alebo
`time set`). `update get -k` (alebo slovo `insecure` v /UPDATE.TXT) overovanie vynechá. Rovnaká verzia sa znova
neinštaluje, staršia len cez `update get force`. Presmerovania a „chunked“ odpovede zvláda. Funguje na Pi 3B+: kernel poslaný z PC aj `update check` z GitHubu s overeným certifikátom.

### Používatelia: prihlásenie na webovú stránku a API (voliteľné)

Kým nie je žiadny používateľ, nič sa nemení: stránka je otvorená a API berie token.
Prvý `users add` zapne prihlasovanie pre stránku aj celé API:

```
users add robo MojeHeslo1           admin: smie všetko (prvý používateľ musí byť admin)
users add host Citatel123 view      používateľ "view": smie len pozerať (stránka, GET /api/...)
users                               zoznam
users passwd host NoveHeslo9        nové heslo
users del host                      zmazať (keď zmizne posledný, stránka je zase otvorená)
```

Prehliadač ukáže svoje prihlasovacie okno; curl: `curl -u robo:MojeHeslo1 http://<adresa>/api/status`.
Používateľ "view" dostane na POST (piny, zvuk, reštart) 403; Bearer token stále funguje a platí ako
admin. Päť zlých hesiel (alebo tokenov) z jednej adresy ju zablokuje na minútu (429), doska vypíše
`[WEB] wrong login 'x' from <adresa>`. `/USERS.TXT` obsahuje len osolený hash hesla
(PBKDF2-HMAC-SHA256, 1000 kôl), nikdy heslo samotné.
**Zapni HTTPS (`http tls on`)**: bez neho prehliadač posiela heslo po sieti čitateľne; so zapnutým
HTTPS stránky z `http://` skočia na `https://` a nič sa nepýta heslo nešifrovane.
Prihlásenie môže pýtať aj shell (klávesnica / UART na doske): `users console on` - po reštarte alebo
`logout` ukáže `RoboOS login:` a pustí len admina (heslo sa zobrazuje ako `*`, 3 zlé pokusy = 10 s
čakanie). Programy v `/RUN` sa spustia samy ako doteraz; Ctrl+C z nich vedie na prihlásenie.
`users console off` to vypne. **Zabudnuté heslo?** Daj SD kartu do PC a zmaž `/USERS.TXT` - shell aj
webová stránka sú potom znova otvorené (žiadne skryté hlavné heslo nie je).

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

## IoT: senzory (I2C) a MQTT

### Senzory na I2C

Zbernica I2C je na pinoch 3 (GPIO2 = SDA) a 5 (GPIO3 = SCL), 3,3 V na pine 1 a GND na pine 6.
Pri štarte RoboOS tieto moduly nájde sám (`sensors scan` hľadá znova):

| Modul | Adresa | Hodnoty |
|---|---|---|
| BME280 (BMP280) | 0x76 / 0x77 | teplota, vlhkosť (len BME280), tlak |
| BMP180 (BMP085) | 0x77 | teplota, tlak (0.73) |
| ADS1115 | 0x48-0x4B | 4 analógové vstupy A0-A3, 0-4,096 V v mV (neprekročiť 3,3 V!) |
| DS3231 (DS1307) | 0x68 | hodiny s batériou: čas je známy po štarte aj bez siete |

```
sensors                  hodnoty všetkého nájdeného (+ teplota CPU)
i2c scan                 všetky zariadenia na zbernici, s odhadom čo to je
i2c read 0x76 0xd0       čítanie registra (i2c read <adr> <reg> [počet])
i2c write 0x76 0xf4 0x27 zápis registra
```

Hodiny sa nastavia vždy, keď odpovie NTP (alebo `time set`), a pri štarte sa čas zoberie z nich -
`time` potom píše "from the battery clock (DS3231)".

### I2C displej (OLED / LCD, 0.55)

Bežné lacné I2C displeje sa pripájajú na tie isté štyri piny (3,3 V, GND, SDA, SCL) a nájdu sa pri štarte:

| Displej | Adresa | Text |
|---|---|---|
| OLED 0,96" / 1,3" 128x64, SSD1306 alebo SH1106 | 0x3C / 0x3D | 16 x 4 (128x32: 16 x 2) |
| znakový LCD 16x2 / 20x4 (HD44780) s modulom PCF8574 („I2C backpack“) | 0x27 / 0x3F | 16 x 2, 20 x 4 |

Pri štarte ukáže „RoboOS“ + verziu, po 2 s **stavovú obrazovku**: meno dosky, IP adresu, čas,
WiFi / Ethernet (na 2 riadkoch: IP + čas) - hodí sa pre dosku bez monitora.

```
display                  čo sa našlo (typ, adresa, veľkosť)
display scan             hľadaj znova (po pripojení)
display text 2 Ahoj      text na 2. riadok (vypne stavovú obrazovku do ďalšieho štartu)
display clear            prázdny displej
display status on|off    stavová obrazovka (uloží sa)
display size 20x4        veľkosť LCD sa nedá zistiť: predpokladá sa 16x2 (aj 128x32 pre malý OLED)
display type ssd1306     aj pre OLED, ktorý vôbec neodpovedá (bez ACK): zapisuje sa naslepo
display type sh1106      ak je obraz OLED posunutý / pravý okraj je šum (auto, lcd, ssd1306, sh1106, none)
display addr 0x26        iná adresa LCD modulu (prepojky A0-A2: 0x20-0x27, PCF8574A 0x38-0x3F)
display font small       písmo OLED: micro 32 x 10, tiny 25 x 8, small 21 x 8, normal 16 x 4, big 8 x 2 (veľké číslice) - uloží sa
display time             veľké digitálne hodiny: tenké HH:MM, vedľa menšie sekundy, pod tým deň + dátum (128x64), LCD = čas + dátum
display info             späť na stavovú obrazovku (meno, IP, čas, sieť)
display console on       kópia terminálu na displeji (najlepšie s malým písmom); off = späť na info
display contrast 128     jas OLED 0-255;  display light on|off = podsvietenie LCD;  display on|off
```

Text cez sieť: JSON API `POST /api/display` s `{"text":"riadok 1\nriadok 2"}` (celý displej) alebo
`{"row":3,"text":"..."}`, `{"clear":true}`; `GET /api/display` ukáže, čo na ňom je. MQTT: `roboos/<meno>/set/display`
(obsah = obrazovka, nový riadok = ďalší riadok) a `roboos/<meno>/set/display/<riadok>`. Príklad:
`curl -X POST http://<doska>/api/display -H "Authorization: Bearer <token>" -d '{"text":"Ahoj\nz PC"}'`.

Nastavenia sa ukladajú do `/DISPLAY.TXT`. BASIC: `DISPLAY 1, "T="; T` a `DISPLAY CLS` (príklad
`/APPS/display.bas`), C (API 11): `displayText(riadok, text)`, `displaySize()`. **Pozor na 5 V LCD:**
modul má vlastné pull-upy na SDA/SCL k svojmu napájaniu - pri 5 V dajú na piny Pi ~3,8 V (viac ako
3,3 V). Buď tie dva rezistory odstráň (zvyčajne 4,7k pri konektore), alebo použi I2C prevodník úrovní;
pri napájaní z 3,3 V býva text príliš slabý. OLED moduly sú na 3,3 V - v poriadku.
Trimrom kontrastu na LCD module treba otočiť, kým sa text neukáže. **Na hardvéri ešte neotestované** (len PC test `tools/test_display.c`,
ktorý hrá všetky tri čipy).

### AI: otázky pre Claude (0.56)

S vlastným API kľúčom Anthropic (vytvoríš ho na console.anthropic.com -> API keys; platí sa podľa
použitia, nastav si tam mesačný limit) sa doska vie pýtať Claude cez HTTPS (certifikát sa overuje):

```
ai key sk-ant-...          uloží kľúč (potom 'clear'); 'ai key off' ho zmaže
ai ako dlho vydrzi 2000 mAh baterka pri 150 mA
ai @wifi preco to trva tak dlho        spustí 'wifi' a pošle jeho výpis s otázkou
ai @"sensors" je tu teplo
ai model claude-sonnet-5-5             iný model; 'ai model' = späť na predvolený
ai                                     model, či je kľúč, návod
```

Predvolený je najlacnejší model `claude-haiku-5-5` (krátka otázka stojí zlomok centu). Odpoveď sa
ukáže v ASCII (bez diakritiky - font ju nemá). Claude dostane krátky popis RoboOS a zoznam príkazov
shellu, takže na otázky „ako spravím ...“ odpovedá skutočnými príkazmi. Kľúč je v `/AI.TXT`
**zašifrovaný XOR-om** - len aby nebol čitateľný na prvý pohľad; kto má kartu, vie ho získať, preto
drž limit výdavkov nízko. Do histórie príkazov sa nikdy nedostane. **Proti skutočnému API ešte
neotestované** (QEMU test s falošným serverom).

### FM rádio (RDA5807M, 0.71)

Malý modul RDA5807M (RRD-102, ~11 x 11 mm) je FM prijímač na I2C zbernici (adresy 0x10/0x11), 3,3 V.
Zapojenie: VCC pin 1, GND pin 6, SDA pin 3, SCL pin 5, na ANT drôt asi 75 cm; zvuk ide priamo z modulu
na L / R (slúchadlá 32 ohm priamo, reproduktor cez malý zosilňovač).

```
radio 104.4          naladí (aj 104,4 alebo 1044), zapne rádio
radio up / down      hľadá ďalšiu stanicu
radio scan           vypíše všetky stanice pásma (87-108 MHz) so silou signálu
radio vol 8          hlasitosť 0-15 (0 = stlmené)
radio mono on|off    vynútené mono (slabé stanice menej šumia); radio bass on|off
radio                frekvencia, stereo, signál, RDS názov stanice a text
radio off / on
display radio        rádio na OLED: frekvencia, názov stanice, text
```

Stanica, hlasitosť a zapnutie sa ukladajú do `/RADIO.TXT` a obnovia sa pri štarte. Cez sieť:
`POST /api/radio` s `{"cmd":"104.4"}` (alebo `up`, `down`, `off`, `vol 8`), `GET /api/radio` (stav +
RDS), MQTT `roboos/<meno>/set/radio`. **Na hardvéri zatiaľ neotestované** (PC test `tools/test_radio.c` so simulovaným čipom).

**Panel rádia: Waveshare 1,3" OLED HAT (0.72).** HAT sa nasadí priamo na 40-pinový konektor: OLED 128x64 SH1106
na SPI0 (CE0, DC GPIO24, RST GPIO25) + KEY1-3 (GPIO21/20/16) a 5-smerový joystick (hore 6, dole 19,
vľavo 5, vpravo 26, stlačenie 13). RDA5807M ostáva na I2C (piny 3/5 sú na HATe voľné).

```
display hat          použije HAT (uloží "bus spi" do /DISPLAY.TXT); 'display type auto' = späť na I2C
display radio        obrazovka rádia
display keys         ktoré tlačidlá sú práve stlačené (test)
```

Tlačidlá: KEY1 = ďalšia obrazovka (info, hodiny, rádio, konzola); na obrazovke rádia KEY2 / KEY3 = hľadanie
dole / hore, joystick vľavo / vpravo = -/+ 0,1 MHz, hore / dole = hlasitosť, stlačenie = rádio zap / vyp.
Pozor: GPIO24/25/16 sú aj výstupy IoT shieldu - nepoužívať naraz. **Na hardvéri funguje** (0.72).

### MQTT

MQTT je bežný spôsob, ako IoT zariadenia hovoria so serverom (Home Assistant, Node-RED,
Mosquitto...). RoboOS sa pripojí k brokeru a posiela mu hodnoty; broker mu posiela príkazy.

```
mqtt broker 192.168.1.10      broker (meno alebo adresa, port 1883; uloží sa do /MQTT.TXT)
mqtt user robo tajne          ak broker chce prihlásenie
mqtt                          stav
mqtt interval 30              hodnoty každých 30 s (0 = nikdy samo)
mqtt pub ahoj svet            pošle roboos/<meno>/ahoj = svet
mqtt off                      vypnúť
```

Témy (`<meno>` je meno dosky z `/NAME.TXT`, napr. `roboos/rpi3-3456/...`):

| Téma | |
|---|---|
| `roboos/<meno>/status` | `online` / `offline` (retained; `offline` je "posledná vôľa", keď doska zmizne) |
| `roboos/<meno>/state` | JSON: `{"cpu":48.31,"temp":21.53,"hum":40.12,"press":1013.25,"a0":1234,...,"uptime":..,"ip":"..","time":".."}` |
| `roboos/<meno>/set/gpio/17` | pošli `1` / `0` / `on` / `off` / `toggle`: pin sa stane výstupom |
| `roboos/<meno>/gpio/17` | doska odpovie novou úrovňou (retained) |
| `roboos/<meno>/set/publish` | pošle stav hneď |
| `roboos/<meno>/cmd/...` | voľné pre tvoje programy (čítajú ich BASIC / C) |

Home Assistant nájde senzory sám (MQTT discovery, `mqtt discovery off` to vypne). Z PC:

```
mosquitto_sub -h 192.168.1.10 -t 'roboos/#' -v
mosquitto_pub -h 192.168.1.10 -t roboos/rpi3-3456/set/gpio/17 -m toggle
```

`/MQTT.TXT` (dá sa napísať aj ručne):

```
broker 192.168.1.10
port 1883
user robo
pass tajne
topic roboos/zahrada
interval 60
discovery on
```

S prihlásením (Mosquitto `password_file`, používateľ MQTT doplnku Home Assistant): `mqtt user <meno> <heslo>`;
`mqtt` potom ukáže "user <meno>", zlé prihlásenie ukáže "last problem: bad user name or password" /
"not authorized" a skúša sa znova len raz za minútu. Heslo je v `/MQTT.TXT` ako obyčajný text a po
sieti ide nešifrovane, ako všetko na porte 1883 (QoS 0) - používaj to vo vlastnej sieti.

V programoch: BASIC `SENSOR(n)` (0 CPU, 1 teplota, 2 vlhkosť v 0,01 C / %, 3 tlak v Pa,
4-7 ADC v mV, -999999 = senzor nie je), `MQTTPUB "téma", položky` (ako PRINT), `MQTTOK`;
príklad `/APPS/mqtt.bas`. C (API 10): `sensorRead(n)`, `mqttPublish(téma, text, retain)`,
`mqttReceive(téma, tmax, správa, pmax)` (správy na `cmd/...`), `mqttConnected()`;
príklad `iotnode` (posiela JSON so senzormi, LED na GPIO23 cez `cmd/led` on / off / blink).

### IoT shield (návrh dosky)

`hardware/iot-shield/` obsahuje návrh dosky v KiCade, ktorá sa nasadí na 40-pinový konektor:
napájanie 12-24 V, 4 izolované vstupy, 4 MOSFET výstupy, ADS1115, BME280, hodiny DS3231,
1-Wire. Pozri jej README - **natiahnutá, Gerbery sú pribalené, nevyrobená ani neotestovaná**.

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
- Šípky hore/dole = história (posledných 30, uložená v /HISTORY.TXT; `history`, `!N`), vľavo/vpravo/Home/End = kurzor, Backspace, Delete, Tab, autorepeat pri držaní klávesy.
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
| `http tls on\|off\|new` | HTTPS na porte 443 (API potom len tam), nový certifikát |
| `wifi [scan\|join <sieť> [heslo]\|band 2\|5\|auto\|leave\|forget]` | WiFi: siete, pripojenie (uloží sa), stav |
| `bt [on\|scan [s]\|sensors\|off\|auto on\|off]` | Bluetooth LE: zariadenia, teplomery, mobil (len na požiadanie) |
| `sensors [scan]`, `i2c scan\|read\|write` | senzory na I2C (BME280, ADS1115, DS3231) |
| `ai <otázka>\|@príkaz ..\|key\|model` | otázka pre Claude (vlastný API kľúč Anthropic) |
| `radio [MHz\|up\|down\|scan\|vol n\|off]` | FM rádio (modul RDA5807M na I2C) |
| `display [scan\|text n ..\|clear\|status on\|off\|size\|type\|addr\|hat\|keys]` | I2C displej OLED / LCD, Waveshare SPI OLED HAT |
| `mqtt [broker\|user\|pub\|interval\|off]` | MQTT klient (IoT) |
| `users [add\|passwd\|del\|console on\|off]`, `logout` | prihlásenie na web a API (voliteľné) |
| `time [sync\|zone\|server\|set]` | dátum a čas z NTP, časové pásmo, ručné nastavenie |
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
| `sound test` | nič nepočuť? tri testy za sebou (tón z PWM, sínus zapisovaný procesorom, sínus cez DMA) s registrami na obrazovke - povedz, ktorý bolo počuť |
| `playwav <súbor>` | prehrá súbor WAV (jack, GPIO18/19 alebo I2S DAC) |
| `sd [init [sdhost\|emmc]]` | info o SD karte (aj radič) / znovu inicializovať po výmene karty |
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
| `src/sd.c` | SD karta: výber radiča; ovládač Arasan SDHCI (GPIO48-53 ALT3), čítanie aj zápis |
| `src/netdev.c` | Ethernet alebo WiFi pod IP vrstvou (Ethernet, keď je zapojený kábel) |
| `src/wifi.c` | WiFi čip cez SDIO (Arasan, GPIO34-39): firmvér, príkazy, scan (port 9front ether4330.c, MIT) |
| `src/sdhost.c` | SD karta na radiči SDHOST (GPIO48-53 ALT0), predvolený od 0.28 |
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
- TLS: BearSSL od Thomasa Pornina (MIT, `third_party/bearssl`).
- WiFi: podľa ovládača `ether4330.c` z Plan 9 / 9front od Richarda Millera (MIT).
- Video: pl_mpeg od Dominica Szablewského (MIT, `third_party/pl_mpeg.h`).
- Registre LAN7515 (Pi 3B+) podľa linuxového ovládača `lan78xx`.
