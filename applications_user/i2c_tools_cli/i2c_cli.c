#include "i2c_cli.h"
#include "i2c_compat.h"

#include <furi.h>
#include <furi_hal.h>
#include <cli/cli.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/args.h>
#include <toolbox/pipe.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C_CLI_BUS     &furi_hal_i2c_handle_external
#define I2C_CLI_TIMEOUT 50
#define CLI_READ_MAX    256
#define CLI_WRITE_MAX   32

// How long i2c_cli_unregister() waits for a command that is still running (ms).
#define I2C_CLI_DRAIN_TIMEOUT_MS 5000

// Commands currently executing code of this FAP. The CLI shell runs them on its own thread and
// cli_registry_delete_command() does not wait for them, so the app must not return (and the
// loader unmap the FAP) while one is still inside i2c_cli_callback().
static volatile int32_t i2c_cli_active = 0;

// Port: write straight to the shell pipe. The upstream code used printf(), which only reaches
// the terminal if libc stdout is bound to the per-thread furi stdout callback; this port has no
// such binding (no --wrap of the stdio calls), so printf output never arrived. pipe_send() is
// what the shell itself drains, and it behaves the same on a stock Flipper.
static void __attribute__((format(printf, 2, 3))) cli_print(PipeSide* pipe, const char* fmt, ...) {
    char buf[112];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if(n <= 0) return;
    if(n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    pipe_send(pipe, buf, (size_t)n);
}

static void i2c_cli_print_usage(PipeSide* pipe) {
    cli_print(pipe, "Usage:\r\n");
    cli_print(pipe, "  i2c scan                                  - scan all 7-bit addresses\r\n");
    cli_print(pipe, "  i2c probe <addr>                          - check if device responds\r\n");
    cli_print(
        pipe, "  i2c read  <addr> <reg> <count> [hex|ascii] - read <count> bytes from <reg>\r\n");
    cli_print(
        pipe, "  i2c write <addr> <reg> <byte> [<byte>...] - write bytes starting at <reg>\r\n");
    cli_print(pipe, "All numbers are hex. Accepted forms: 1A, 1a, 0x1A, 0X1a, 0x1a.\r\n");
    cli_print(
        pipe,
        "Wiring: SCL=GPIO%d SDA=GPIO%d (Qwiic) GND=GND, 3V3 levels only.\r\n",
        (int)I2C_BUS_SCL_PIN,
        (int)I2C_BUS_SDA_PIN);
    cli_print(
        pipe,
        "Writes to the power ICs 0x%02X (BQ25896) and 0x%02X (BQ27220) are refused.\r\n",
        I2C_ADDR_BQ25896,
        I2C_ADDR_BQ27220);
}

// Parse a hex unsigned integer from the next whitespace-delimited token in args.
// Accepts an optional 0x or 0X prefix and any mix of upper/lower hex digits.
// Returns false if no token, empty token, or invalid characters.
static bool parse_uint(FuriString* args, uint32_t* out) {
    FuriString* word = furi_string_alloc();
    bool ok = args_read_string_and_trim(args, word);
    if(ok) {
        const char* s = furi_string_get_cstr(word);
        if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        if(*s == '\0') {
            ok = false;
        } else {
            char* end = NULL;
            unsigned long v = strtoul(s, &end, 16);
            ok = (end != NULL) && (*end == '\0');
            if(ok) *out = (uint32_t)v;
        }
    }
    furi_string_free(word);
    return ok;
}

static bool parse_byte(FuriString* args, uint8_t* out) {
    uint32_t v = 0;
    if(!parse_uint(args, &v)) return false;
    if(v > 0xFF) return false;
    *out = (uint8_t)v;
    return true;
}

static void cmd_scan(PipeSide* pipe) {
    cli_print(pipe, "Scanning I2C bus...\r\n");
    uint8_t found = 0;
    furi_hal_i2c_acquire(I2C_CLI_BUS);
    for(uint8_t addr = 1; addr <= 0x7F; addr++) {
        if(furi_hal_i2c_is_device_ready(I2C_CLI_BUS, addr << 1, I2C_CLI_TIMEOUT)) {
            cli_print(pipe, "  0x%02X\r\n", addr);
            found++;
        }
    }
    furi_hal_i2c_release(I2C_CLI_BUS);
    cli_print(pipe, "Done. %u device(s) found.\r\n", (unsigned)found);
}

static void cmd_probe(PipeSide* pipe, FuriString* args) {
    uint8_t addr = 0;
    if(!parse_byte(args, &addr) || addr > 0x7F) {
        cli_print(pipe, "usage: i2c probe <addr>\r\n");
        return;
    }
    furi_hal_i2c_acquire(I2C_CLI_BUS);
    bool ready = furi_hal_i2c_is_device_ready(I2C_CLI_BUS, addr << 1, I2C_CLI_TIMEOUT);
    furi_hal_i2c_release(I2C_CLI_BUS);
    cli_print(pipe, "0x%02X: %s\r\n", addr, ready ? "present" : "absent");
}

static void cmd_read(PipeSide* pipe, FuriString* args) {
    uint8_t addr = 0;
    uint8_t reg = 0;
    uint32_t count = 0;
    if(!parse_byte(args, &addr) || addr > 0x7F) goto bad;
    if(!parse_byte(args, &reg)) goto bad;
    if(!parse_uint(args, &count)) goto bad;
    if(count == 0 || count > CLI_READ_MAX) {
        cli_print(pipe, "count must be in 1..%u\r\n", (unsigned)CLI_READ_MAX);
        return;
    }

    // Optional format: "hex" (default) or "ascii"
    bool ascii = false;
    FuriString* fmt = furi_string_alloc();
    if(args_read_string_and_trim(args, fmt)) {
        const char* f = furi_string_get_cstr(fmt);
        if(strcmp(f, "ascii") == 0 || strcmp(f, "ASCII") == 0) {
            ascii = true;
        } else if(strcmp(f, "hex") != 0 && strcmp(f, "HEX") != 0) {
            cli_print(pipe, "unknown format '%.16s' (expected 'hex' or 'ascii')\r\n", f);
            furi_string_free(fmt);
            return;
        }
    }
    furi_string_free(fmt);

    // Heap, not stack: the command thread only has the CLI default stack (4 KB).
    uint8_t* buf = malloc(CLI_READ_MAX);
    furi_hal_i2c_acquire(I2C_CLI_BUS);
    bool ok = furi_hal_i2c_trx(I2C_CLI_BUS, addr << 1, &reg, 1, buf, count, I2C_CLI_TIMEOUT);
    furi_hal_i2c_release(I2C_CLI_BUS);
    if(!ok) {
        free(buf);
        cli_print(pipe, "read failed (no ACK?)\r\n");
        return;
    }
    char line[64];
    if(ascii) {
        for(uint32_t i = 0; i < count; i += sizeof(line) - 3) {
            size_t n = 0;
            for(; n < sizeof(line) - 3 && (i + n) < count; n++) {
                uint8_t b = buf[i + n];
                line[n] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
            }
            pipe_send(pipe, line, n);
        }
        cli_print(pipe, "\r\n");
    } else {
        for(uint32_t i = 0; i < count; i += 16) {
            size_t n = 0;
            for(uint32_t j = i; j < i + 16 && j < count; j++) {
                n += snprintf(line + n, sizeof(line) - n, j + 1 == i + 16 || j + 1 == count ? "%02X" : "%02X ", buf[j]);
            }
            n += snprintf(line + n, sizeof(line) - n, "\r\n");
            pipe_send(pipe, line, n);
        }
    }
    free(buf);
    return;
bad:
    cli_print(pipe, "usage: i2c read <addr> <reg> <count> [hex|ascii]\r\n");
}

static void cmd_write(PipeSide* pipe, FuriString* args) {
    uint8_t addr = 0;
    uint8_t reg = 0;
    if(!parse_byte(args, &addr) || addr > 0x7F) goto bad;
    if(!parse_byte(args, &reg)) goto bad;
    uint8_t buf[1 + CLI_WRITE_MAX];
    buf[0] = reg;
    uint8_t n = 1;
    while(n < (1 + CLI_WRITE_MAX)) {
        if(!parse_byte(args, &buf[n])) break;
        n++;
    }
    if(n == 1) goto bad;
    furi_hal_i2c_acquire(I2C_CLI_BUS);
    bool ok = furi_hal_i2c_tx(I2C_CLI_BUS, addr << 1, buf, n, I2C_CLI_TIMEOUT);
    furi_hal_i2c_release(I2C_CLI_BUS);
    if(!ok && i2c_compat_write_blocked()) {
        cli_print(
            pipe, "refused: 0x%02X is a power IC on this board, writes are blocked\r\n", addr);
        return;
    }
    cli_print(
        pipe,
        "write %u byte(s) to 0x%02X[0x%02X]: %s\r\n",
        (unsigned)(n - 1),
        addr,
        reg,
        ok ? "ok" : "failed");
    return;
bad:
    cli_print(pipe, "usage: i2c write <addr> <reg> <byte> [<byte>...]\r\n");
}

static void i2c_cli_callback(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    __atomic_fetch_add(&i2c_cli_active, 1, __ATOMIC_SEQ_CST);

    FuriString* sub = furi_string_alloc();
    if(!args_read_string_and_trim(args, sub)) {
        i2c_cli_print_usage(pipe);
    } else {
        const char* s = furi_string_get_cstr(sub);
        if(strcmp(s, "scan") == 0) {
            cmd_scan(pipe);
        } else if(strcmp(s, "probe") == 0) {
            cmd_probe(pipe, args);
        } else if(strcmp(s, "read") == 0) {
            cmd_read(pipe, args);
        } else if(strcmp(s, "write") == 0) {
            cmd_write(pipe, args);
        } else if(strcmp(s, "help") == 0 || strcmp(s, "?") == 0) {
            i2c_cli_print_usage(pipe);
        } else {
            cli_print(pipe, "Unknown subcommand '%.16s'.\r\n", s);
            i2c_cli_print_usage(pipe);
        }
    }
    furi_string_free(sub);

    __atomic_fetch_sub(&i2c_cli_active, 1, __ATOMIC_SEQ_CST);
}

void i2c_cli_register(void) {
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(
        registry, "i2c", CliCommandFlagParallelSafe, i2c_cli_callback, NULL);
    furi_record_close(RECORD_CLI);
}

void i2c_cli_unregister(void) {
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_delete_command(registry, "i2c");
    furi_record_close(RECORD_CLI);

    // No new invocation can start now; let one that is already running finish, then give its
    // thread a moment to return into the shell (the return path is still this FAP's code).
    for(uint32_t waited = 0; __atomic_load_n(&i2c_cli_active, __ATOMIC_SEQ_CST) > 0 &&
                             waited < I2C_CLI_DRAIN_TIMEOUT_MS;
        waited += 10) {
        furi_delay_ms(10);
    }
    furi_delay_ms(50);
}
