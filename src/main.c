/*
 * cortex-m-emu: run a raw STM32 firmware image (.bin, linked at 0x08000000).
 *
 *   cortex-m-emu [-n max_steps] [-t] firmware.bin
 *     -n  stop after this many instructions (default 1000000)
 *     -t  print each instruction's address and first halfword before it executes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "loader.h"

static void dump_registers(const cpu_t *cpu)
{
    for (int i = 0; i < 16; i++) {
        printf("r%-2d = 0x%08x%s", i, cpu->r[i], (i % 4 == 3) ? "\n" : "   ");
    }
    printf("N=%d Z=%d C=%d V=%d   steps=%llu\n", cpu->n, cpu->z, cpu->c, cpu->v,
           (unsigned long long)cpu->steps);
}

int main(int argc, char **argv)
{
    unsigned long long max_steps = 1000000;
    int trace = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-t") == 0) {
            trace = 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s [-n max_steps] [-t] firmware.bin\n", argv[0]);
        return 2;
    }

    bus_t *bus = bus_new();
    if (!bus || load_bin(bus, path) != 0) {
        fprintf(stderr, "cannot load %s\n", path);
        return 1;
    }

    cpu_t cpu;
    cpu_reset(&cpu, bus);
    while (!cpu.halted && cpu.steps < max_steps) {
        if (trace) {
            printf("%08x: %04x\n", cpu.r[PC], bus_read16(bus, cpu.r[PC]));
        }
        cpu_step(&cpu, bus);
    }

    printf("stopped: %s\n", cpu.halted ? cpu.halt_reason : "step limit reached");
    dump_registers(&cpu);
    if (bus_unmapped_count(bus)) {
        printf("note: %u accesses to peripheral addresses with no model (last 0x%08x)\n",
               bus_unmapped_count(bus), bus_last_unmapped_addr(bus));
    }
    bus_free(bus);
    return 0;
}
