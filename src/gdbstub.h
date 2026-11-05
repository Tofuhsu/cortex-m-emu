/*
 * Minimal GDB Remote Serial Protocol server for one connection over TCP.
 *
 * Supports ?, g, G, p, P, m, M, c, s, Z0/z0 (software breakpoints kept in a
 * table; flash is not modified), k, D, qSupported and the m-profile target
 * description (qXfer:features:read). Memory access is limited to flash and
 * SRAM so that a debugger read never changes peripheral state.
 */
#ifndef GDBSTUB_H
#define GDBSTUB_H

#include <stdint.h>

#include "bus.h"
#include "cpu.h"

/*
 * Listen on 127.0.0.1:port (0 picks a free port), print the port to stderr,
 * accept one client and serve it until it kills or detaches, or the
 * connection closes. Returns 0 on a clean end, nonzero on a socket error.
 */
int gdbstub_serve(cpu_t *cpu, bus_t *bus, uint16_t port);

#endif
