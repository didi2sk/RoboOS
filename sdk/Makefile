# RoboOS program SDK
#   make                 builds every examples/*.c (and *.c here in apps/) into a .bin
#   make TOOLCHAIN=gcc CROSS=aarch64-none-elf-
# Copy the resulting .bin files to the SD card: into /RUN to start them at boot,
# anywhere else to start them with "run <name>".

TOOLCHAIN ?= clang
CROSS     ?= aarch64-none-elf-

CFLAGS = -Wall -O2 -ffreestanding -fno-stack-protector -nostdlib -mcpu=cortex-a53 -I.

ifeq ($(TOOLCHAIN),clang)
  CC      = clang --target=aarch64-none-elf
  LD      = ld.lld
  OBJCOPY = llvm-objcopy
else
  CC      = $(CROSS)gcc
  LD      = $(CROSS)ld
  OBJCOPY = $(CROSS)objcopy
endif

SRCS = $(wildcard examples/*.c)
BINS = $(SRCS:examples/%.c=bin/%.bin)

all: $(BINS)

bin/%.bin: examples/%.c build/start.o build/crt0.o robos.h app.ld | bin
	$(CC) $(CFLAGS) -c $< -o build/$*.o
	$(LD) -T app.ld build/start.o build/crt0.o build/$*.o -o build/$*.elf
	$(OBJCOPY) -O binary build/$*.elf $@
	@echo "  $@ ($$(wc -c < $@) bytes)"

build/start.o: start.S | build
	$(CC) $(CFLAGS) -c $< -o $@
build/crt0.o: crt0.c robos.h | build
	$(CC) $(CFLAGS) -c $< -o $@

bin build:
	mkdir -p $@

clean:
	rm -rf build bin

.PHONY: all clean
