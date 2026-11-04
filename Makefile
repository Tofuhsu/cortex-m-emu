CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g -Isrc
LIB_SRC := src/bus.c src/cpu.c src/loader.c src/periph/rcc.c src/periph/gpio.c
BUILD   := build

# Test programs are assembled with the ARM toolchain and linked at the start of flash.
AS      := arm-none-eabi-as
LD      := arm-none-eabi-ld
OBJCOPY := arm-none-eabi-objcopy
PROGS   := $(patsubst tests/progs/%.s,$(BUILD)/progs/%.bin,$(wildcard tests/progs/*.s))

.PHONY: all test progs clean

all: $(BUILD)/cortex-m-emu

$(BUILD)/cortex-m-emu: src/main.c $(LIB_SRC) src/*.h src/periph/*.h | $(BUILD)
	$(CC) $(CFLAGS) src/main.c $(LIB_SRC) -o $@

$(BUILD)/test_runner: tests/test_main.c $(LIB_SRC) src/*.h src/periph/*.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_main.c $(LIB_SRC) -o $@

progs: $(PROGS)

$(BUILD)/progs/%.bin: tests/progs/%.s tests/progs/vectors.inc | $(BUILD)/progs
	$(AS) -I tests/progs -o $(BUILD)/progs/$*.o $<
	$(LD) -Ttext=0x08000000 -o $(BUILD)/progs/$*.elf $(BUILD)/progs/$*.o
	$(OBJCOPY) -O binary $(BUILD)/progs/$*.elf $@

test: $(BUILD)/test_runner $(BUILD)/cortex-m-emu progs
	./$(BUILD)/test_runner

$(BUILD) $(BUILD)/progs:
	mkdir -p $@

clean:
	rm -rf $(BUILD)
