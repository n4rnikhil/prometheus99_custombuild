/**
 * @file modbus_server.c
 * @brief The bench of simulated devices: non-blocking Modbus TCP servers (Winsock / BSD sockets)
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define SEND_FLAGS 0
static void sock_close(sock_t s) { closesocket(s); }
static void sock_nonblock(sock_t s) { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }
static bool would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define SEND_FLAGS MSG_NOSIGNAL
static void sock_close(sock_t s) { close(s); }
static void sock_nonblock(sock_t s) { int f = fcntl(s, F_GETFL, 0); if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK); }
static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "modbus_server.h"

typedef struct {
    sock_t  s;
    uint8_t rx[SIM_MAX_ADU * 2];
    size_t  rx_len;
} client_t;

typedef struct {
    sock_t   listener;
    client_t cl[PLC_MAX_CLIENTS];
} station_io_t;

static plc_station_t s_st[PLC_MAX_STATIONS];
static station_io_t  s_io[PLC_MAX_STATIONS];
static uint32_t      s_port_freed_ms[PLC_MAX_STATIONS];
static bool          s_port_ever_freed[PLC_MAX_STATIONS];
static bench_log_fn  s_log;
static bool          s_network;
static bool          s_ok;
static uint16_t      s_base = PLC_DEFAULT_PORT;
static char          s_lan_ip[16];
static uint32_t      s_last_step_ms, s_now_ms;
static bool          s_stepping;

#if defined(__GNUC__)
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
static void logf_(const char *fmt, ...)
{
    char text[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (s_log) s_log(text);
}

static bool is_plc(const plc_station_t *p) { return sim_type_info(p->sim.type)->is_plc; }

/* ------------------------------------------------------------------------- */
static void find_lan_ip(void)
{
    char host[256];
    struct addrinfo hints, *res = NULL;
    if (gethostname(host, sizeof(host)) != 0) return;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)(const void *)r->ai_addr;
        uint32_t ip = ntohl(sa->sin_addr.s_addr);
        if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;   /* loopback, link-local */
        snprintf(s_lan_ip, sizeof(s_lan_ip), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        break;
    }
    freeaddrinfo(res);
}

bool bench_init(bench_log_fn log, uint16_t base_port)
{
    s_log = log;
    if (base_port) s_base = base_port;
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
#endif
    for (int i = 0; i < PLC_MAX_STATIONS; i++) {
        s_io[i].listener = BAD_SOCK;
        for (int k = 0; k < PLC_MAX_CLIENTS; k++) s_io[i].cl[k].s = BAD_SOCK;
    }
    find_lan_ip();
    s_ok = true;
    return true;
}

void bench_shutdown(void)
{
    for (int i = 0; i < PLC_MAX_STATIONS; i++) if (s_st[i].used) bench_unplug(i);
#ifdef _WIN32
    if (s_ok) WSACleanup();
#endif
    s_ok = false;
}

const char *bench_lan_ip(void) { return s_lan_ip; }
bool bench_network(void) { return s_network; }
uint16_t bench_base_port(void) { return s_base; }

plc_station_t *bench_station(int idx)
{
    return (idx >= 0 && idx < PLC_MAX_STATIONS && s_st[idx].used) ? &s_st[idx] : NULL;
}

/* ------------------------------------------------------------------------- */
/* Connections                                                                */
/* ------------------------------------------------------------------------- */
static void count_clients(int idx)
{
    int n = 0;
    for (int k = 0; k < PLC_MAX_CLIENTS; k++) if (s_io[idx].cl[k].s != BAD_SOCK) n++;
    s_st[idx].clients = n;
}

static void close_client(int idx, int k, bool log_it)
{
    client_t *c = &s_io[idx].cl[k];
    if (c->s == BAD_SOCK) return;
    sock_close(c->s);
    c->s = BAD_SOCK;
    c->rx_len = 0;
    count_clients(idx);
    plc_station_t *p = &s_st[idx];
    if (log_it) {
        if (is_plc(p)) logf_("%s: client %s disconnected", p->name, p->client_ip[k]);
        else if (p->clients == 0) logf_("%s: %s checked it and left it alone (not a PLC)", p->name, p->client_ip[k]);
    }
    p->client_ip[k][0] = '\0';
}

void bench_unplug(int idx)
{
    plc_station_t *p = bench_station(idx);
    if (!p) return;
    for (int k = 0; k < PLC_MAX_CLIENTS; k++) close_client(idx, k, false);
    if (s_io[idx].listener != BAD_SOCK) sock_close(s_io[idx].listener);
    s_io[idx].listener = BAD_SOCK;
    p->plugged = false;
}

bool bench_plug(int idx)
{
    plc_station_t *p = bench_station(idx);
    if (!p || p->plugged) return p != NULL;
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) return false;
    int one = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(p->port);
    a.sin_addr.s_addr = htonl(s_network ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        sock_close(s);
        logf_("%s: port %u is in use by another program%s", p->name, (unsigned)p->port,
              p->port < 1024 ? " (or needs administrator / root rights)" : "");
        return false;
    }
    sock_nonblock(s);
    s_io[idx].listener = s;
    p->plugged = true;
    p->hmi_checked = false;
    return true;
}

void bench_set_network(bool on)
{
    if (s_network == on) return;
    s_network = on;
    for (int i = 0; i < PLC_MAX_STATIONS; i++) if (s_st[i].used && s_st[i].plugged) { bench_unplug(i); bench_plug(i); }
    if (on) logf_("Network mode: devices are reachable at %s:%u-%u", s_lan_ip[0] ? s_lan_ip : "this computer's address",
                  (unsigned)s_base, (unsigned)(s_base + PLC_MAX_STATIONS - 1));
    else    logf_("Local mode: devices are reachable from this computer only");
}

/* ------------------------------------------------------------------------- */
/* Stations                                                                   */
/* ------------------------------------------------------------------------- */
static int pick_port_index(uint32_t ms)
{
    bool taken[PLC_MAX_STATIONS] = { false };
    for (int i = 0; i < PLC_MAX_STATIONS; i++) if (s_st[i].used) taken[s_st[i].port - s_base] = true;
    for (int k = 0; k < PLC_MAX_STATIONS; k++)                     /* prefer ports an HMI has not seen recently */
        if (!taken[k] && (!s_port_ever_freed[k] || ms - s_port_freed_ms[k] >= PLC_PORT_REUSE_MS)) return k;
    for (int k = 0; k < PLC_MAX_STATIONS; k++) if (!taken[k]) return k;
    return -1;
}

int bench_add(sim_type_t type)
{
    if (!sim_type_info(type)) return -1;
    int slot = -1;
    for (int i = 0; i < PLC_MAX_STATIONS; i++) if (!s_st[i].used) { slot = i; break; }
    int port_idx = pick_port_index(s_now_ms);
    if (slot < 0 || port_idx < 0) { logf_("The bench is full: %d devices at most", PLC_MAX_STATIONS); return -1; }
    plc_station_t *p = &s_st[slot];
    memset(p, 0, sizeof(*p));
    p->used = true;
    sim_init(&p->sim, type);
    p->port = (uint16_t)(s_base + port_idx);
    s_io[slot].listener = BAD_SOCK;
    for (int k = 0; k < PLC_MAX_CLIENTS; k++) { s_io[slot].cl[k].s = BAD_SOCK; s_io[slot].cl[k].rx_len = 0; }
    for (uint8_t n = 1; n < 20; n++) {                        /* lowest free number per device type */
        bool clash = false;
        for (int i = 0; i < PLC_MAX_STATIONS; i++)
            if (i != slot && s_st[i].used && s_st[i].sim.type == type && s_st[i].number == n) clash = true;
        if (!clash) { p->number = n; break; }
    }
    snprintf(p->name, sizeof(p->name), "%s %u", sim_type_info(type)->type_name, (unsigned)p->number);
    if (bench_plug(slot)) logf_("%s plugged in on port %u", p->name, (unsigned)p->port);
    return slot;
}

void bench_remove(int idx)
{
    plc_station_t *p = bench_station(idx);
    if (!p) return;
    bench_unplug(idx);
    s_port_freed_ms[p->port - s_base] = s_now_ms;
    s_port_ever_freed[p->port - s_base] = true;
    logf_("%s removed from the bench", p->name);
    p->used = false;
}

/* ------------------------------------------------------------------------- */
/* Polling                                                                    */
/* ------------------------------------------------------------------------- */
static void poll_station(int idx, uint32_t ms)
{
    plc_station_t *p = &s_st[idx];
    station_io_t *io = &s_io[idx];
    if (!p->plugged) return;
    for (;;) {                                               /* new connections */
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        sock_t c = accept(io->listener, (struct sockaddr *)&from, &flen);
        if (c == BAD_SOCK) break;
        int k = -1;
        for (int i = 0; i < PLC_MAX_CLIENTS; i++) if (io->cl[i].s == BAD_SOCK) { k = i; break; }
        if (k < 0) { sock_close(c); continue; }
        sock_nonblock(c);
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        snprintf(p->client_ip[k], sizeof(p->client_ip[k]), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        io->cl[k].s = c;
        io->cl[k].rx_len = 0;
        count_clients(idx);
        if (is_plc(p) || !p->hmi_checked) logf_("%s: client %s connected", p->name, p->client_ip[k]);
        p->hmi_checked = true;
    }
    for (int k = 0; k < PLC_MAX_CLIENTS; k++) {
        client_t *c = &io->cl[k];
        if (c->s == BAD_SOCK) continue;
        for (;;) {
            if (c->rx_len >= sizeof(c->rx)) { close_client(idx, k, true); break; }
            int n = (int)recv(c->s, (char *)c->rx + c->rx_len, (int)(sizeof(c->rx) - c->rx_len), 0);
            if (n > 0) { c->rx_len += (size_t)n; continue; }
            if (n == 0 || !would_block()) close_client(idx, k, true);
            break;
        }
        while (c->s != BAD_SOCK && c->rx_len > 0) {
            int32_t fl = sim_frame_length(c->rx, c->rx_len);
            if (fl < 0) { close_client(idx, k, true); break; }
            if (fl == 0) break;
            uint8_t out[SIM_MAX_ADU];
            size_t rl = sim_handle(&p->sim, c->rx, (size_t)fl, out, sizeof(out));
            p->last_request_ms = ms;
            memmove(c->rx, c->rx + fl, c->rx_len - (size_t)fl);
            c->rx_len -= (size_t)fl;
            if (rl && (int)send(c->s, (const char *)out, (int)rl, SEND_FLAGS) != (int)rl) close_client(idx, k, true);
        }
    }
}

void bench_poll(uint32_t now_ms)
{
    s_now_ms = now_ms;
    if (!s_stepping) { s_stepping = true; s_last_step_ms = now_ms; }
    for (int i = 0; i < PLC_MAX_STATIONS; i++) if (s_st[i].used) poll_station(i, now_ms);
    if (now_ms - s_last_step_ms >= 100u) {                   /* one 100 ms scan of every device */
        s_last_step_ms += 100u;
        if (now_ms - s_last_step_ms >= 1000u) s_last_step_ms = now_ms;
        for (int i = 0; i < PLC_MAX_STATIONS; i++) if (s_st[i].used) sim_step(&s_st[i].sim);
    }
    char ev[SIM_EVENT_LEN];
    for (int i = 0; i < PLC_MAX_STATIONS; i++)
        while (s_st[i].used && sim_pop_event(&s_st[i].sim, ev, sizeof(ev))) logf_("%s: %s", s_st[i].name, ev);
}

/* ------------------------------------------------------------------------- */
static const char * const s_keys[SIM_TYPE_COUNT] = {
    [SIM_TANK] = "tank", [SIM_ATV340] = "atv340", [SIM_ATV630] = "atv630", [SIM_CONVEYOR] = "conveyor",
    [SIM_BOILER] = "boiler", [SIM_PUMPS] = "pumps", [SIM_COMPRESSOR] = "compressor", [SIM_AHU] = "ahu",
    [SIM_VFD] = "abb", [SIM_METER] = "meter", [SIM_ANON] = "unknown",
};

const char *bench_type_key(sim_type_t t) { return (unsigned)t < SIM_TYPE_COUNT ? s_keys[t] : "?"; }

sim_type_t bench_type_by_name(const char *name)
{
    for (int t = 0; t < SIM_TYPE_COUNT; t++) {
        const char *k = s_keys[t];
        size_t i = 0;
        while (k[i] && name[i] && (name[i] | 0x20) == k[i]) i++;
        if (!k[i] && !name[i]) return (sim_type_t)t;
    }
    return SIM_TYPE_COUNT;
}
