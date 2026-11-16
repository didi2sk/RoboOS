# RoboOS - notes for Claude Code

Bare-metal hobby OS for the Raspberry Pi (64-bit BCM2837 boards), written from scratch in C +
AArch64 assembly. No Linux, no libc. Owner: Robo (talks in Slovak; UI, code comments and the
English README are in English, `README.sk.md` is the Slovak README - keep both in sync).

## Build & test

```
make                 # kernel8.img (clang --target=aarch64-none-elf + ld.lld)
make apps            # SDK examples -> sd/APPS/*.bin
make sd.img          # 64 MB FAT32 test image for QEMU (needs dosfstools, mtools, fdisk)
make run             # QEMU raspi3b, UART on stdio, USB keyboard
```

Automated QEMU testing (no display): `tools/qemu_test.py kernel8.img "cmd1" "cmd2" ...`
- sends each arg + CR over UART, prints UART output, saves HDMI screenshot to /tmp/rpi_screen.ppm
- an arg starting with `!` goes to the QEMU monitor; `;` separates several, e.g.
  `"!sendkey e; sendkey c; sendkey h; sendkey o; sendkey ret"` types on the emulated USB keyboard
- `$'\x03'` sends Ctrl+C
- env: `QEMU_EXTRA="-device usb-kbd"`, `QEMU_SD=sd.img`, `QEMU_MACHINE=raspi3ap`, `BOOT_WAIT=seconds`
- verify FAT writes: `dd if=sd.img of=p.img bs=512 skip=2048; fsck.fat -n p.img`
  ("Free cluster summary uninitialized" is expected - we invalidate FSInfo on purpose)

QEMU limitations: no split transactions / HS hubs (QEMU hubs are FS), no Zero 2 W machine,
`device_add` of a USB device at runtime freezes QEMU 8.2 (not our bug) - hot-plug can't be tested.

## Hardware status (what the owner has verified on real boards)

- RPi 3B: boot, HDMI, UART, MMU, USB keyboards **work** (incl. Apple A1243 keyboard with built-in
  HS hub, and an RGB gaming keyboard). Hot-plug works.
- RPi 3B: SD init + FAT file writes **work** (owner tested after the CMD55 fix, v0.7).
- Not yet verified on hardware: Ethernet (LAN9514 driver, v0.8 - QEMU has no such device, so it is
  completely untested), program loader, BASIC, line editor / full-screen editor,
  board detection, Zero 2 W (ACT LED GPIO29 active-low is an assumption), Pi 2 v1.2.
- Owner also has a Zero 2 W and a Pi 2 (version v1.1 vs v1.2 not confirmed yet; v1.1 is 32-bit
  and would need an AArch32 port). No Pi 4 / Pi 5.
- Debugging on hardware happens via photos of the HDMI screen - keep diagnostics printable on
  screen (`usb debug on`, `usb test`, `usb` tree with hub port status).

## Architecture

| File | Role |
|------|------|
| `src/boot.S` | park cores 1-3, EL3/EL2 -> EL1, stack below 0x80000, vectors, BSS; exceptions use a separate 16 KB stack |
| `src/mmu.c` | identity map, 2 MB blocks: RAM cached, GPU RAM + 2 MB DMA block non-cached, peripherals device |
| `src/board.c` | revision -> board name, prompt host, ACT LED pin, `board_usb_direct()` |
| `src/usb.c` | DWC2 host driver: polling, one channel (0), Buffer DMA, one packet per channel run |
| `src/eth.c` | LAN9514 (0424:ec00) Ethernet: vendor control requests for registers, bulk IN/OUT for frames (`usb_bulk`), polled; `net`, `net test` |
| `src/kbd.c` | HID boot keyboard, US layout, keypad, LEDs, autorepeat, runtime method lock |
| `src/sd.c` / `src/fat.c` | Arasan SDHCI (GPIO48-53 ALT3), PIO single-block read/write; FAT16/32 with LFN read, 8.3 write |
| `src/app.c` | loads `.bin` at 0x01000000, API table (`sdk/robos.h`), Ctrl+C via `ctx_save/ctx_restore` (`src/ctx.S`), autostart `/RUN` |
| `src/basic.c` | line-numbered integer BASIC, text `.BAS` files, SAVE/LOAD, GPIO statements |
| `src/lineedit.c` | shared line editor (shell, BASIC prompt, INPUT): cursor keys, insert/delete, history, Tab hook; redraws with `\b` + chars only |
| `src/edit.c` | full-screen editor (`edit <file>`): flat buffer at 0x02000000 (1 MB) + save scratch above it; draws via `con_goto`/`con_clreol`, only changed rows (UART is slow) |
| `src/shell.c` | commands, Tab completion |
| `sdk/` | program SDK: `start.S` header (`ROBOAPP1` magic at +8, image end at +16), `crt0.c`, `app.ld`, examples |

Memory: kernel at 0x80000, BSS ends ~0x120000, DMA block at 0x200000 (2 MB, uncached),
programs at 0x01000000-0x02000000, editor text at 0x02000000 (1 MB) + save scratch up to 0x02300000.
Keys: `con_getkey()` decodes escape sequences into `K_UP`... (kbd.c sends the same sequences as a terminal).

## Hard-won lessons (do not regress)

- **-mstrict-align / -mgeneral-regs-only**: code runs with MMU off early on (device memory faults on unaligned access).
- **Mailbox & DMA buffers**: clean/invalidate D-cache around mailbox calls; USB DMA buffer lives in the uncached block; DMA bus address = phys | 0xC0000000.
- **DWC2 in DMA mode auto-retries NAK** on control/bulk, so HCINT can show NAK and XFERCOMPL together - check XFERCOMPL first.
- **Periodic split transactions** need microframe scheduling: SSPLIT in uframe N (avoid 6), CSPLIT in N+2.. (2-3 tries on NYET). Without it interrupt IN through the LAN9514 TT never delivered keys.
- **Keyboard read method** is decided at runtime: alternate interrupt IN and GET_REPORT until the first non-zero report, then lock (`k_lock`).
- **Hub ports must be powered one at a time** (with pgood delay); powering all ports of the Apple keyboard hub at once made it drop off the bus.
- Gaming keyboards attach late (>500 ms after port power) -> hot-plug polling of hub ports every 0.7 s, plus the root port on boards without an onboard hub.
- HID init like a PC: GET_DESCRIPTOR(report), SET_PROTOCOL(boot), SET_IDLE(0), SET_REPORT(LEDs) - some keyboards stay dark/silent otherwise.
- **SD on WiFi boards**: firmware leaves GPIO34-39 on ALT3 (Arasan -> WiFi chip); set them to input before
  routing GPIO48-53 to ALT3 (Circle does the same; CMD8 answers on the 3B with this in place).
- **SD CMD55 must wait for its R1 answer** even with RCA 0; otherwise ACMD41 collides with it on a real card
  (INT=0x30000 at "ACMD41 power-up" on the 3B; QEMU answers instantly and hides it). Reset CMD/DAT lines after a failed command.
- In BASIC, compute an array index (which may allocate) before reading `v->arr` (evaluation-order bug fixed once).
- The font covers ASCII 32-126 only - keep all on-screen strings ASCII.
- Keep exceptions inside programs recoverable: exception stack is separate so `app_crashed()` can longjmp back.

## Conventions

- Version string in `src/main.c` (`VERSION`), mention it in both READMEs.
- After changes: build with no warnings, run the QEMU tests relevant to the change, update README.md + README.sk.md.
- Put the fresh `kernel8.img` into `sd/` (the CI also builds it).

## Open ideas / next steps

- verify Ethernet/BASIC/loader on real hardware; Zero 2 W and Pi 2 bring-up
- network: owner wants RoboOS-to-RoboOS messages over raw frames first (EtherType 0x88b5), UDP/IP later
- license (MIT suggested, not chosen yet)
- Slovak keyboard layout + font with diacritics
- I2C / SPI / PWM in API and BASIC, interrupts + timer, multitasking, editor search / go to line / save as
- optional AArch32 port for Pi 2 v1.1 / Zero
