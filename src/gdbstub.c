/*
 * GDB Remote Serial Protocol server. Packet format: $<data>#<two hex digit
 * checksum>, acknowledged with + (good) or - (resend). See the GDB manual,
 * appendix "Remote Protocol".
 *
 * Registers follow the org.gnu.gdb.arm.m-profile feature: r0-r12, sp, lr, pc
 * (GDB numbers 0-15) and xpsr (number 25). The `g` packet carries them in that
 * order, little endian.
 */
#define _POSIX_C_SOURCE 200809L
#include "gdbstub.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_PACKET 4096
#define MAX_BREAKPOINTS 64
#define XPSR_REGNUM 25
/* Check the socket for Ctrl-C (0x03) after this many instructions. */
#define POLL_INTERVAL 4096u

static const char target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\"/><reg name=\"r1\" bitsize=\"32\"/>"
    "<reg name=\"r2\" bitsize=\"32\"/><reg name=\"r3\" bitsize=\"32\"/>"
    "<reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/>"
    "<reg name=\"r8\" bitsize=\"32\"/><reg name=\"r9\" bitsize=\"32\"/>"
    "<reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\" regnum=\"25\"/>"
    "</feature></target>";

typedef struct {
    int fd;
    cpu_t *cpu;
    bus_t *bus;
    uint32_t breakpoints[MAX_BREAKPOINTS];
    unsigned breakpoint_count;
    bool interrupted;
} stub_t;

static const char hex_digits[] = "0123456789abcdef";

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse hex digits up to a non-hex character. Returns false if there are none. */
static bool parse_hex(const char **text, uint32_t *value)
{
    const char *p = *text;
    uint32_t result = 0;
    int digit;
    if (hex_value(*p) < 0) return false;
    while ((digit = hex_value(*p)) >= 0) {
        if (result > 0x0FFFFFFFu) return false;
        result = (result << 4) | (uint32_t)digit;
        p++;
    }
    *text = p;
    *value = result;
    return true;
}

static void put_le32(char *out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        uint8_t byte = (uint8_t)(value >> (8 * i));
        out[2 * i] = hex_digits[byte >> 4];
        out[2 * i + 1] = hex_digits[byte & 15];
    }
}

static bool get_le32(const char *in, uint32_t *value)
{
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        int hi = hex_value(in[2 * i]), lo = hex_value(in[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        result |= (uint32_t)(hi << 4 | lo) << (8 * i);
    }
    *value = result;
    return true;
}

static bool send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static int read_byte(int fd)
{
    unsigned char c;
    for (;;) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

/* Send one packet and wait for '+'; resend on '-'. */
static bool send_packet(stub_t *s, const char *data)
{
    size_t len = strlen(data);
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) sum += (unsigned char)data[i];
    char tail[4] = {'#', hex_digits[(sum >> 4) & 15], hex_digits[sum & 15], 0};
    for (int attempt = 0; attempt < 8; attempt++) {
        if (!send_all(s->fd, "$", 1) || !send_all(s->fd, data, len) || !send_all(s->fd, tail, 3))
            return false;
        int ack;
        do ack = read_byte(s->fd); while (ack == 0x03 && (s->interrupted = true));
        if (ack == '+') return true;
        if (ack != '-') return false;
    }
    return false;
}

/* Receive one packet into buf (NUL terminated). Returns false on EOF. */
static bool recv_packet(stub_t *s, char *buf, size_t size)
{
    for (;;) {
        int c;
        do {
            c = read_byte(s->fd);
            if (c < 0) return false;
        } while (c != '$');
        size_t len = 0;
        unsigned sum = 0;
        bool overflow = false;
        while ((c = read_byte(s->fd)) >= 0 && c != '#') {
            if (len + 1 < size) buf[len++] = (char)c;
            else overflow = true;
            sum += (unsigned)c;
        }
        if (c < 0) return false;
        int hi = read_byte(s->fd), lo = read_byte(s->fd);
        if (hi < 0 || lo < 0) return false;
        buf[len] = 0;
        int want_hi = hex_value((char)hi), want_lo = hex_value((char)lo);
        if (!overflow && want_hi >= 0 && want_lo >= 0 &&
            (unsigned)(want_hi << 4 | want_lo) == (sum & 0xFFu)) {
            return send_all(s->fd, "+", 1);
        }
        if (!send_all(s->fd, "-", 1)) return false;
    }
}

static uint32_t read_register(const cpu_t *cpu, uint32_t n, bool *ok)
{
    *ok = true;
    if (n < 16) return cpu->r[n];
    if (n == XPSR_REGNUM) return cpu_get_xpsr(cpu);
    *ok = false;
    return 0;
}

/* xPSR writes change only N, Z, C and V; IPSR and the T bit are kept. */
static bool write_register(cpu_t *cpu, uint32_t n, uint32_t value)
{
    if (n < 15) {
        cpu->r[n] = value;
    } else if (n == 15) {
        cpu->r[PC] = value & ~1u;
    } else if (n == XPSR_REGNUM) {
        cpu->n = (value >> 31) & 1u;
        cpu->z = (value >> 30) & 1u;
        cpu->c = (value >> 29) & 1u;
        cpu->v = (value >> 28) & 1u;
    } else {
        return false;
    }
    return true;
}

static bool is_breakpoint(const stub_t *s, uint32_t pc)
{
    for (unsigned i = 0; i < s->breakpoint_count; i++)
        if (s->breakpoints[i] == pc) return true;
    return false;
}

static bool interrupt_requested(stub_t *s)
{
    struct pollfd pfd = {s->fd, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        unsigned char c;
        if (recv(s->fd, &c, 1, MSG_PEEK) == 1 && c == 0x03) {
            (void)recv(s->fd, &c, 1, 0);
            return true;
        }
    }
    return false;
}

/* Stop reply after s/c: 05 SIGTRAP for a step, breakpoint or BKPT,
 * 02 SIGINT for Ctrl-C, 04 SIGILL for any other halt. */
static const char *stop_reply(stub_t *s, bool interrupted)
{
    if (interrupted) return "S02";
    if (s->cpu->halted && strncmp(s->cpu->halt_reason, "bkpt #", 6) != 0) {
        fprintf(stderr, "gdb: CPU halted: %s\n", s->cpu->halt_reason);
        return "S04";
    }
    return "S05";
}

static const char *resume(stub_t *s, bool single_step)
{
    if (s->cpu->halted) return stop_reply(s, false);
    if (single_step) {
        cpu_step(s->cpu, s->bus);
        return stop_reply(s, false);
    }
    /* The instruction at the current PC runs even if it has a breakpoint,
     * so `continue` from a breakpoint makes progress. */
    unsigned since_poll = 0;
    bool first = true;
    while (!s->cpu->halted) {
        if (!first && is_breakpoint(s, s->cpu->r[PC])) break;
        first = false;
        cpu_step(s->cpu, s->bus);
        if (++since_poll == POLL_INTERVAL) {
            since_poll = 0;
            if (s->interrupted || interrupt_requested(s)) {
                s->interrupted = false;
                return stop_reply(s, true);
            }
        }
    }
    return stop_reply(s, false);
}

static void handle_read_registers(stub_t *s, char *out)
{
    bool ok;
    for (uint32_t n = 0; n < 16; n++) put_le32(out + 8 * n, read_register(s->cpu, n, &ok));
    put_le32(out + 8 * 16, cpu_get_xpsr(s->cpu));
    out[8 * 17] = 0;
}

static const char *handle_write_registers(stub_t *s, const char *args)
{
    if (strlen(args) < 8 * 17) return "E01";
    uint32_t values[17];
    for (int n = 0; n < 17; n++)
        if (!get_le32(args + 8 * n, &values[n])) return "E01";
    for (uint32_t n = 0; n < 16; n++) write_register(s->cpu, n, values[n]);
    write_register(s->cpu, XPSR_REGNUM, values[16]);
    return "OK";
}

static void handle_memory_read(stub_t *s, const char *args, char *out)
{
    uint32_t addr, len;
    if (!parse_hex(&args, &addr) || *args++ != ',' || !parse_hex(&args, &len) || *args ||
        len == 0 || len > (MAX_PACKET - 1) / 2) {
        strcpy(out, "E01");
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t byte;
        if (!bus_debug_byte(s->bus, addr + i, &byte, false)) {
            if (i == 0) strcpy(out, "E14");
            else out[2 * i] = 0;
            return;
        }
        out[2 * i] = hex_digits[byte >> 4];
        out[2 * i + 1] = hex_digits[byte & 15];
    }
    out[2 * len] = 0;
}

static const char *handle_memory_write(stub_t *s, const char *args)
{
    uint32_t addr, len;
    if (!parse_hex(&args, &addr) || *args++ != ',' || !parse_hex(&args, &len) || *args++ != ':' ||
        strlen(args) != 2u * len) return "E01";
    for (uint32_t i = 0; i < len; i++) {
        int hi = hex_value(args[2 * i]), lo = hex_value(args[2 * i + 1]);
        if (hi < 0 || lo < 0) return "E01";
        uint8_t byte = (uint8_t)(hi << 4 | lo);
        if (!bus_debug_byte(s->bus, addr + i, &byte, true)) return "E14";
    }
    return "OK";
}

/* Z0,addr,kind / z0,addr,kind. Only software breakpoints (type 0). */
static const char *handle_breakpoint(stub_t *s, const char *args, bool insert)
{
    uint32_t type, addr, kind;
    if (!parse_hex(&args, &type) || *args++ != ',' || !parse_hex(&args, &addr) ||
        *args++ != ',' || !parse_hex(&args, &kind)) return "E01";
    if (type != 0) return "";
    addr &= ~1u;
    for (unsigned i = 0; i < s->breakpoint_count; i++) {
        if (s->breakpoints[i] == addr) {
            if (!insert) s->breakpoints[i] = s->breakpoints[--s->breakpoint_count];
            return "OK";
        }
    }
    if (!insert) return "OK";
    if (s->breakpoint_count == MAX_BREAKPOINTS) return "E0e";
    s->breakpoints[s->breakpoint_count++] = addr;
    return "OK";
}

/* qXfer:features:read:target.xml:offset,length */
static void handle_xfer(const char *args, char *out)
{
    const char *prefix = "target.xml:";
    uint32_t offset, length;
    if (strncmp(args, prefix, strlen(prefix)) != 0) {
        strcpy(out, "E00");
        return;
    }
    args += strlen(prefix);
    if (!parse_hex(&args, &offset) || *args++ != ',' || !parse_hex(&args, &length)) {
        strcpy(out, "E01");
        return;
    }
    size_t total = sizeof target_xml - 1;
    if (offset >= total) {
        strcpy(out, "l");
        return;
    }
    size_t n = total - offset;
    if (n > length) n = length;
    if (n > MAX_PACKET - 2) n = MAX_PACKET - 2;
    out[0] = offset + n < total ? 'm' : 'l';
    memcpy(out + 1, target_xml + offset, n);
    out[1 + n] = 0;
}

/* Returns false when the session should end. */
static bool handle_packet(stub_t *s, char *packet, char *out)
{
    const char *args = packet + 1;
    out[0] = 0;
    switch (packet[0]) {
    case '?': strcpy(out, "S05"); break;
    case 'g': handle_read_registers(s, out); break;
    case 'G': strcpy(out, handle_write_registers(s, args)); break;
    case 'p': {
        uint32_t n;
        bool ok = false;
        uint32_t value = parse_hex(&args, &n) && !*args ? read_register(s->cpu, n, &ok) : 0;
        if (ok) { put_le32(out, value); out[8] = 0; }
        else strcpy(out, "E01");
        break;
    }
    case 'P': {
        uint32_t n, value;
        bool ok = parse_hex(&args, &n) && *args++ == '=' && strlen(args) == 8 &&
                  get_le32(args, &value) && write_register(s->cpu, n, value);
        strcpy(out, ok ? "OK" : "E01");
        break;
    }
    case 'm': handle_memory_read(s, args, out); break;
    case 'M': strcpy(out, handle_memory_write(s, args)); break;
    case 'c':
    case 's':
        /* An address argument resumes there; GDB normally sends none. */
        if (*args) {
            uint32_t addr;
            if (!parse_hex(&args, &addr)) { strcpy(out, "E01"); break; }
            s->cpu->r[PC] = addr & ~1u;
        }
        strcpy(out, resume(s, packet[0] == 's'));
        break;
    case 'Z': strcpy(out, handle_breakpoint(s, args, true)); break;
    case 'z': strcpy(out, handle_breakpoint(s, args, false)); break;
    case 'H': strcpy(out, "OK"); break;
    case 'T': strcpy(out, "OK"); break;
    case 'k': return false;
    case 'D': send_packet(s, "OK"); return false;
    case 'q':
        if (strncmp(packet, "qSupported", 10) == 0)
            snprintf(out, MAX_PACKET, "PacketSize=%x;qXfer:features:read+", MAX_PACKET);
        else if (strncmp(packet, "qXfer:features:read:", 20) == 0) handle_xfer(packet + 20, out);
        else if (strcmp(packet, "qAttached") == 0) strcpy(out, "1");
        else if (strcmp(packet, "qC") == 0) strcpy(out, "QC1");
        else if (strcmp(packet, "qfThreadInfo") == 0) strcpy(out, "m1");
        else if (strcmp(packet, "qsThreadInfo") == 0) strcpy(out, "l");
        break;
    default:
        break; /* Empty reply: not supported. */
    }
    return send_packet(s, out);
}

int gdbstub_serve(cpu_t *cpu, bus_t *bus, uint16_t port)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("gdb: socket"); return 1; }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    addr.sin_port = htons(port);
    socklen_t addr_len = sizeof addr;
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        perror("gdb: bind");
        close(listener);
        return 1;
    }
    fprintf(stderr, "gdb: listening on 127.0.0.1:%u\n", (unsigned)ntohs(addr.sin_port));
    int fd = accept(listener, NULL, NULL);
    close(listener);
    if (fd < 0) { perror("gdb: accept"); return 1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    stub_t s = {.fd = fd, .cpu = cpu, .bus = bus};
    static char packet[MAX_PACKET], out[MAX_PACKET + 16];
    while (recv_packet(&s, packet, sizeof packet)) {
        if (!handle_packet(&s, packet, out)) break;
    }
    close(fd);
    return 0;
}
