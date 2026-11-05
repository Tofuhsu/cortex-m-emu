#include "ssd1306.h"

#include <stdlib.h>

typedef enum { EXPECT_CONTROL, COMMANDS, DATA } stream_t;

struct ssd1306 {
    uint8_t address;
    uint8_t ram[SSD1306_PAGES][SSD1306_WIDTH];
    bool on;
    uint8_t mode; /* 0 horizontal, 2 page; 1 (vertical) is stored as horizontal */
    uint8_t col_start, col_end, page_start, page_end, column, page;
    stream_t stream;
    bool single;        /* Co = 1: one byte, then a new control byte */
    uint8_t command;    /* command waiting for arguments */
    uint8_t args_left, args[2], args_seen;
    uint64_t unknown;
};

/* Number of argument bytes for each multi-byte command this model accepts. */
static int argument_count(uint8_t command)
{
    switch (command) {
    case 0x20: case 0x81: case 0x8D: case 0xA8: case 0xD3:
    case 0xD5: case 0xD9: case 0xDA: case 0xDB:
        return 1;
    case 0x21: case 0x22:
        return 2;
    default:
        return 0;
    }
}

static bool single_byte_command(uint8_t c)
{
    return c <= 0x1F || (c >= 0x40 && c <= 0x7F) || c == 0xA0 || c == 0xA1 ||
           (c >= 0xA4 && c <= 0xA7) || c == 0xAE || c == 0xAF ||
           (c >= 0xB0 && c <= 0xB7) || c == 0xC0 || c == 0xC8 || c == 0xE3;
}

static void finish_command(ssd1306_t *s)
{
    switch (s->command) {
    case 0x20: s->mode = (s->args[0] & 3u) == 2u ? 2u : 0u; break;
    case 0x21:
        s->col_start = s->args[0] & 0x7Fu; s->col_end = s->args[1] & 0x7Fu;
        s->column = s->col_start;
        break;
    case 0x22:
        s->page_start = s->args[0] & 7u; s->page_end = s->args[1] & 7u;
        s->page = s->page_start;
        break;
    default: break;
    }
}

static void command_byte(ssd1306_t *s, uint8_t c)
{
    if (s->args_left) {
        s->args[s->args_seen++ & 1u] = c;
        if (--s->args_left == 0) finish_command(s);
        return;
    }
    int n = argument_count(c);
    if (n) {
        s->command = c; s->args_left = (uint8_t)n; s->args_seen = 0;
        return;
    }
    if (!single_byte_command(c)) { s->unknown++; return; }
    if (c == 0xAE) s->on = false;
    else if (c == 0xAF) s->on = true;
    else if (c >= 0xB0 && c <= 0xB7) s->page = c & 7u;
    else if (c <= 0x0F) s->column = (uint8_t)((s->column & 0xF0u) | c);
    else if (c <= 0x1F) s->column = (uint8_t)(((c & 0x07u) << 4) | (s->column & 0x0Fu));
}

static void data_byte(ssd1306_t *s, uint8_t d)
{
    s->ram[s->page & 7u][s->column & 0x7Fu] = d;
    if (s->mode == 2u) { /* page mode: column wraps within the page */
        s->column = (uint8_t)((s->column + 1u) & 0x7Fu);
        return;
    }
    if (s->column >= s->col_end) {
        s->column = s->col_start;
        s->page = s->page >= s->page_end ? s->page_start : (uint8_t)(s->page + 1u);
    } else {
        s->column++;
    }
}

static void dev_start(void *ctx, bool read)
{
    ssd1306_t *s = ctx;
    (void)read;
    s->stream = EXPECT_CONTROL;
    s->single = false;
}

static bool dev_write(void *ctx, uint8_t byte)
{
    ssd1306_t *s = ctx;
    if (s->stream == EXPECT_CONTROL) {
        if (byte & 0x3Fu) s->unknown++; /* only Co and D/C# are defined */
        s->single = (byte & 0x80u) != 0;
        s->stream = (byte & 0x40u) ? DATA : COMMANDS;
        return true;
    }
    if (s->stream == DATA) data_byte(s, byte);
    else command_byte(s, byte);
    if (s->single) s->stream = EXPECT_CONTROL;
    return true;
}

static uint8_t dev_read(void *ctx, bool ack)
{
    (void)ctx; (void)ack;
    return 0;
}

static void dev_stop(void *ctx)
{
    ((ssd1306_t *)ctx)->stream = EXPECT_CONTROL;
}

ssd1306_t *ssd1306_new(uint8_t address)
{
    ssd1306_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->address = address;
    s->col_end = SSD1306_WIDTH - 1u;
    s->page_end = SSD1306_PAGES - 1u;
    s->mode = 2u; /* page addressing after reset */
    return s;
}

void ssd1306_free(ssd1306_t *oled) { free(oled); }

i2c_device_t ssd1306_as_device(ssd1306_t *s)
{
    return (i2c_device_t){s->address, s, dev_start, dev_write, dev_read, dev_stop};
}

bool ssd1306_display_on(const ssd1306_t *s) { return s->on; }

uint8_t ssd1306_gddram(const ssd1306_t *s, unsigned page, unsigned column)
{
    return page < SSD1306_PAGES && column < SSD1306_WIDTH ? s->ram[page][column] : 0;
}

uint64_t ssd1306_unknown_bytes(const ssd1306_t *s) { return s->unknown; }

bool ssd1306_write_pbm(const ssd1306_t *s, FILE *file)
{
    fprintf(file, "P1\n# SSD1306 GDDRAM, display %s\n%u %u\n", s->on ? "on" : "off",
            SSD1306_WIDTH, SSD1306_PAGES * 8u);
    for (unsigned y = 0; y < SSD1306_PAGES * 8u; ++y) {
        for (unsigned x = 0; x < SSD1306_WIDTH; ++x)
            fputc((s->ram[y / 8u][x] >> (y % 8u)) & 1u ? '1' : '0', file);
        fputc('\n', file);
    }
    return !ferror(file);
}
