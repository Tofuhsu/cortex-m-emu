#include "loader.h"

#include <stdio.h>
#include <stdlib.h>

int load_bin(bus_t *bus, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    static uint8_t buf[FLASH_SIZE];
    size_t n = fread(buf, 1, sizeof buf, f);
    int too_big = !feof(f) && fgetc(f) != EOF;
    fclose(f);
    if (n == 0 || too_big) {
        return -1;
    }
    return bus_load_flash(bus, 0, buf, (uint32_t)n);
}
