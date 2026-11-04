#ifndef LOADER_H
#define LOADER_H

#include "bus.h"

/* Load a raw binary (objcopy -O binary) into flash at 0x08000000. Returns 0 on success. */
int load_bin(bus_t *bus, const char *path);

#endif
