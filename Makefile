CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g -Isrc
LIB_SRC := src/bus.c src/cpu.c src/loader.c src/vcd.c src/periph/rcc.c src/periph/gpio.c src/periph/systick.c src/periph/usart.c src/periph/nvic.c src/periph/i2c.c src/periph/bme280.c
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

$(BUILD)/test_extended: tests/test_extended.c $(LIB_SRC) src/*.h src/periph/*.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_extended.c $(LIB_SRC) -o $@

test: $(BUILD)/test_extended $(BUILD)/test_runner $(BUILD)/cortex-m-emu progs
	./$(BUILD)/test_runner
	./$(BUILD)/test_extended --programs
	python3 tests/test_cli.py

$(BUILD) $(BUILD)/progs:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

# Host-only checks do not rebuild ARM assembly and do not imply target acceptance.
.PHONY: test-host
test-host: $(BUILD)/test_extended
	./$(BUILD)/test_extended
