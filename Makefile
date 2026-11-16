# RoboOS - build
#   make              (clang + lld, default)
#   make TOOLCHAIN=gcc CROSS=aarch64-none-elf-     (ARM GNU toolchain)
#   make apps         (SDK example programs -> sd/APPS)
#   make run          (QEMU raspi3b, UART in the terminal)

TOOLCHAIN ?= clang
CROSS     ?= aarch64-none-elf-

SRCS = $(wildcard src/*.c)
ASMS = $(wildcard src/*.S)
OBJS = $(SRCS:src/%.c=build/%.o) $(ASMS:src/%.S=build/%.o)

COMMON = -Wall -Wextra -O2 -ffreestanding -fno-stack-protector -nostdlib \
         -mgeneral-regs-only -mstrict-align -mcpu=cortex-a53

ifeq ($(TOOLCHAIN),clang)
  CC      = clang --target=aarch64-none-elf
  LD      = ld.lld
  OBJCOPY = llvm-objcopy
else
  CC      = $(CROSS)gcc
  LD      = $(CROSS)ld
  OBJCOPY = $(CROSS)objcopy
endif

all: kernel8.img

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(COMMON) -c $< -o $@

build/%.o: src/%.S | build
	$(CC) $(COMMON) -c $< -o $@

build:
	mkdir -p build

kernel8.elf: $(OBJS) src/linker.ld
	$(LD) -nostdlib -T src/linker.ld $(OBJS) -o $@

kernel8.img: kernel8.elf
	$(OBJCOPY) -O binary $< $@
	@echo "done: $@ ($$(wc -c < $@) bytes)"

# build the SDK example programs and put them into sd/APPS
apps:
	$(MAKE) -C sdk TOOLCHAIN=$(TOOLCHAIN) CROSS=$(CROSS)
	mkdir -p sd/APPS sd/RUN
	cp sdk/bin/*.bin sd/APPS/

# 64 MB SD image for QEMU (needs mtools, dosfstools, fdisk)
sd.img: apps
	tools/mksdimg.sh sd.img "" "hello blink button knight chat"

run: kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -device usb-kbd

clean:
	rm -rf build kernel8.elf kernel8.img sd.img

.PHONY: all apps run clean
