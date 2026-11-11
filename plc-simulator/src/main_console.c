/**
 * @file main_console.c
 * @brief PLC Simulator without a window: a bench of simulated devices on the network, for a
 *        Raspberry Pi, a Linux box, a CI job or a Windows console
 *
 *   plc_simulator_cli --add atv340 --add tank --network
 *   plc_simulator_cli --port 502 --add ahu --network      (port 502 needs root on Linux)
 *
 * Options
 *   --add <type>       device to put on the bench (repeatable, default: tank)
 *                      types: tank atv340 atv630 conveyor boiler pumps compressor ahu abb meter unknown
 *   --port <n>         first TCP port (default 1502; devices use n, n+1, ...)
 *   --network          listen on every network interface (default: 127.0.0.1 only)
 *   --run-for <s>      stop after s seconds (default: until Ctrl+C)
 *   --status <s>       print every device's values every s seconds (default 0 = never)
 * Every Modbus write from a client is printed as it happens.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint32_t now_ms(void) { return GetTickCount(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modbus_server.h"

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void on_log(const char *text)
{
    printf("%s\n", text);
    fflush(stdout);
}

static void usage(void)
{
    printf("usage: plc_simulator_cli [--add <type>]... [--port <n>] [--network] [--run-for <s>] [--status <s>]\n"
           "types: tank atv340 atv630 conveyor boiler pumps compressor ahu abb meter unknown\n");
}

static void print_status(void)
{
    for (int i = 0; i < PLC_MAX_STATIONS; i++) {
        plc_station_t *p = bench_station(i);
        if (!p) continue;
        const sim_type_info_t *ti = sim_type_info(p->sim.type);
        printf("[%s  port %u  %s  clients %d]\n", p->name, (unsigned)p->port,
               p->sim.stopped ? "STOP" : "RUN", p->clients);
        for (uint8_t r = 0; r < ti->nrows; r++) {
            char v[40];
            sim_row_text(&p->sim, r, v, sizeof(v));
            printf("    %-26s %s\n", ti->rows[r].label, v);
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    uint16_t base = PLC_DEFAULT_PORT;
    bool network = false;
    long run_for = 0, status_s = 0;
    sim_type_t adds[PLC_MAX_STATIONS];
    int nadds = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--network") == 0) network = true;
        else if (strcmp(a, "--port") == 0 && next) {
            long v = strtol(next, NULL, 10);
            if (v <= 0 || v > 65535 - PLC_MAX_STATIONS) { usage(); return 64; }
            base = (uint16_t)v; i++;
        } else if (strcmp(a, "--add") == 0 && next) {
            sim_type_t t = bench_type_by_name(next);
            if (t == SIM_TYPE_COUNT) { printf("unknown device type: %s\n", next); usage(); return 64; }
            if (nadds < PLC_MAX_STATIONS) adds[nadds++] = t;
            i++;
        } else if (strcmp(a, "--run-for") == 0 && next) { run_for = strtol(next, NULL, 10); i++; }
        else if (strcmp(a, "--status") == 0 && next) { status_s = strtol(next, NULL, 10); i++; }
        else { usage(); return 64; }
    }
    if (nadds == 0) adds[nadds++] = SIM_TANK;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!bench_init(on_log, base)) { printf("sockets are not available\n"); return 1; }
    bench_set_network(network);
    for (int i = 0; i < nadds; i++) bench_add(adds[i]);
    printf("PLC Simulator running (%s). Ctrl+C to stop.\n",
           network ? "network: other computers can connect" : "local: this computer only, --network to share");
    fflush(stdout);

    uint32_t t0 = now_ms(), last_status = t0;
    while (!g_stop) {
        uint32_t t = now_ms();
        bench_poll(t);
        if (run_for > 0 && (uint32_t)(t - t0) >= (uint32_t)run_for * 1000u) break;
        if (status_s > 0 && (uint32_t)(t - last_status) >= (uint32_t)status_s * 1000u) { last_status = t; print_status(); }
        sleep_ms(5);
    }
    bench_shutdown();
    printf("PLC Simulator stopped.\n");
    return 0;
}
