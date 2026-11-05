#include "vcd.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

struct vcd {
    FILE *file;
    uint64_t last_step;
    uint32_t uart_count, i2c_count;
    bool failed;
};

static bool timestamp(vcd_t *v, uint64_t step)
{
    if (!v || v->failed) return false;
    if (step < v->last_step) { v->failed = true; return false; }
    if (step != v->last_step) fprintf(v->file, "#%" PRIu64 "\n", step);
    v->last_step = step;
    return true;
}

static void vector(vcd_t *v, unsigned width, uint32_t value, const char *id)
{
    fputc('b', v->file);
    for (unsigned bit = width; bit > 0; --bit) fputc((value & (UINT32_C(1) << (bit - 1))) ? '1' : '0', v->file);
    fprintf(v->file, " %s\n", id);
}

vcd_t *vcd_open(const char *path)
{
    vcd_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->file = fopen(path, "w");
    if (!v->file) { free(v); return NULL; }
    /* No wall-clock date, random IDs or path names: byte-for-byte reproducible. */
    fputs("$version cortex-m-emu transaction trace v1 $end\n"
          "$comment Timestamps are executed-instruction steps. One step is NOT one ns. "
          "UART/I2C are transaction vectors, not electrical or baud waveforms. $end\n"
          "$timescale 1 ns $end\n$scope module cortex_m_emu $end\n"
          "$var wire 1 ! pa5 $end\n"
          "$var wire 8 u uart_tx_byte $end\n$var wire 32 U uart_event $end\n"
          "$var wire 3 k i2c_kind $end\n$var wire 8 d i2c_data $end\n"
          "$var wire 1 a i2c_ack $end\n$var wire 1 r i2c_read $end\n"
          "$var wire 1 p i2c_repeated_start $end\n$var wire 32 I i2c_event $end\n"
          "$upscope $end\n$enddefinitions $end\n#0\n$dumpvars\n"
          "0!\nb00000000 u\nb0 U\nb000 k\nb00000000 d\n0a\n0r\n0p\nb0 I\n$end\n", v->file);
    return v;
}

void vcd_pa5(vcd_t *v, uint64_t step, bool value)
{
    if (timestamp(v, step)) fprintf(v->file, "%u!\n", (unsigned)value);
}

void vcd_uart(vcd_t *v, uint64_t step, uint8_t byte)
{
    if (!timestamp(v, step)) return;
    vector(v, 8, byte, "u"); vector(v, 32, ++v->uart_count, "U");
}

void vcd_i2c(vcd_t *v, uint64_t step, i2c_event_t event)
{
    if (!timestamp(v, step)) return;
    vector(v, 3, (unsigned)event.kind, "k");
    /* Retain the last data byte over START/STOP. Final RX and STOP can share
     * one instruction timestamp; a control event must not erase that byte. */
    if (event.kind == I2C_EVENT_ADDRESS || event.kind == I2C_EVENT_TX || event.kind == I2C_EVENT_RX) {
        vector(v, 8, event.value, "d");
        fprintf(v->file, "%ua\n%ur\n", (unsigned)event.ack, (unsigned)event.read);
    }
    fprintf(v->file, "%up\n", (unsigned)event.repeated);
    vector(v, 32, ++v->i2c_count, "I");
}

bool vcd_close(vcd_t *v)
{
    if (!v) return true;
    bool good = !v->failed && !ferror(v->file);
    if (fclose(v->file) != 0) good = false;
    free(v);
    return good;
}
