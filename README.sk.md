# RoboOS

[English](README.md) | **Slovensky**

![Štart RoboOS](docs/screenshot.png)

Mini operačný systém pre Raspberry Pi, ktorý beží priamo na hardvéri, bez Linuxu.
Ovláda sa **USB klávesnicou** alebo cez sériovú linku (UART). Výstup ide naraz na
HDMI aj UART a oba vstupy fungujú súčasne.

Aktuálna verzia: **0.6**

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
- `run <meno>` spustí program z `/`, `/RUN` alebo `/APPS`; prípona je nepovinná.
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

- Príkazy: `RUN`, `LIST [a-b]`, `NEW`, `SAVE "meno"`, `LOAD "meno"`, `DIR [cesta]`, `HELP`, `BYE`.
  Samotné číslo riadku riadok zmaže.
- Príkazy v programe: `PRINT`/`?`, `INPUT`, `LET`, `IF..THEN..ELSE`, `GOTO`, `GOSUB`/`RETURN`,
  `FOR..TO..STEP`/`NEXT`, `DIM`, `END`, `REM`/`'`, `CLS`, `RANDOMIZE`; viac príkazov oddeľ `:`.
- GPIO: `PINMODE pin, OUT|IN|PULLUP|PULLDOWN`, `GPIO pin, HIGH|LOW`, `GPIO(pin)`, `LED ON|OFF`, `WAIT ms`.
- Funkcie: `ABS()`, `RND(n)`, `MIN(a,b)`, `MAX(a,b)`, `MILLIS`, `MICROS`, `INKEY`, `TEMP`, `CHR$()` (v PRINT).
- Iba celé čísla (64-bit). Kľúčové slová musia byť oddelené medzerami. Súbory `.bas` sú
  obyčajný text, dajú sa písať aj na PC.
- Mená súborov pri ukladaní: 8.3 (max. 8 znakov + prípona).

Ukážky na karte: `APPS/blink.bas`, `APPS/button.bas`, `APPS/knight.bas`.

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
`clearScreen`, `textColor`, `actLed`, `cpuTemperature`, `stopRequested`, `exit`
(viď `sdk/robos.h`). Program môže mať `setup()` + `loop()` alebo `main()`.
Ukážky: `hello`, `blink`, `button`, `knight`.

Programy sa nahrávajú na adresu 0x01000000 (max. 16 MB). Ak program spadne, vypíše sa
výnimka a systém sa vráti do shellu.

**GPIO má 3,3 V** (nie 5 V ako Arduino Uno), max. asi 16 mA na pin.
LED zapájaj cez odpor 220–330 Ω, relé a motory cez tranzistor alebo modul.

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
| `ls [cesta]` | obsah priečinka na SD karte |
| `cat <súbor>` | vypíše textový súbor (`"úvodzovky"` pre mená s medzerami) |
| `run <program>` | spustí program `.bin` alebo `.bas` |
| `basic [súbor]` | interpreter BASICu |
| `sd [init]` | info o SD karte / znovu inicializovať po výmene karty |
| `clear` | vymaže obrazovku (aj Ctrl+L) |
| `reboot` | reštart cez watchdog |
| `halt` | zastaví systém |

Editor riadku (UART aj klávesnica, v shelli aj v BASICu): **šípky vľavo/vpravo**, **Home/End**
(alebo Ctrl+A / Ctrl+E) hýbu kurzorom, takže sa dá vkladať a mazať kdekoľvek v riadku.
**Tab** doplní príkaz, **šípky hore/dole** listujú
históriu, **Ctrl+C** zruší riadok. Neplatný prístup do pamäte (napr. `mem 0xdeadbeef00`)
vyvolá výnimku; tá sa vypíše a systém sa vráti do shellu.

## Ako to funguje

| Súbor | Čo robí |
|-------|---------|
| `src/boot.S` | uspí jadrá 1–3, prechod EL2→EL1, zásobník, vektory výnimiek, BSS |
| `src/mmu.c` | identické mapovanie 2 MB blokmi, zapnutie D/I cache |
| `src/mbox.c` | mailbox ku GPU firmvéru (info o doske, takty, teplota, framebuffer) |
| `src/uart.c` | PL011 UART0 na GPIO14/15 |
| `src/fb.c` | HDMI framebuffer 1024×768, textová konzola s fontom 8×16 |
| `src/console.c` | výstup na UART + HDMI, `kprintf`, farby, vstup |
| `src/usb.c` | USB host ovládač DWC2: inicializácia, root port, DMA prenosy, split transakcie, enumerácia, huby, hot-plug |
| `src/kbd.c` | HID klávesnica: boot protocol, preklad scan kódov, autorepeat |
| `src/sd.c` | ovládač SD karty (Arasan SDHCI, GPIO48-53 ALT3), čítanie aj zápis |
| `src/fat.c` | FAT16/FAT32: dlhé mená, čítanie, zápis (mená 8.3), obe kópie FAT |
| `src/app.c` | nahrávanie programov, API v štýle Arduina, Ctrl+C, autostart z `/RUN` |
| `src/basic.c` | interpreter BASICu |
| `src/ctx.S` | setjmp/longjmp na ukončenie programov |
| `src/board.c` | rozpoznanie dosky: názov, ACT LED, zapojenie USB |
| `src/lineedit.c` | editor riadku: kurzorové klávesy, história, Tab (shell + BASIC) |
| `src/shell.c` | príkazy |
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

Ovládač beží bez prerušení (polling) a používa jeden DMA kanál. DMA buffre ležia
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

## Nápady do budúcna
- ďalšie rozloženia klávesnice (aj font s diakritikou)
- prerušenia + časovač → multitasking
- I2C / SPI / PWM v API aj v BASICu
- celoobrazovkový textový editor
- ďalšie jadrá (spin table na 0xE0–0xF0)

## Poďakovanie

- Firmvér (`bootcode.bin`, `start.elf`, `fixup.dat`) je od Raspberry Pi Ltd,
  licencia v `sd/LICENCE.broadcom`.
- Font konzoly je vykreslený z DejaVu Sans Mono (licencia Bitstream Vera).
- Inicializácia SD karty vychádza zo známych bare-metal návodov
  (bztsrc/raspi3-tutorial); časovanie periodických split transakcií podľa Circle/USPi.
