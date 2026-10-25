/**
 * @file discovery.c
 * @brief Modbus TCP device discovery: parallel non-blocking probes + FC43 identification
 *
 * Runs in its own thread (THREAD_PRIORITY_BELOW_NORMAL), never in the 1 kHz loop.
 * All buffers are static: no heap.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/drivers/discovery.h"
#include "../../include/drivers/modbus_proto.h"

typedef struct {
    uint32_t ip_first, ip_last;     /* host byte order */
    uint16_t port_first, port_last;
    uint8_t  unit;
    bool     explicit_target;
} range_t;

typedef struct { char host[16]; uint16_t port; DWORD until; } ignored_t;
static ignored_t s_ignored[HMI_DISCOVERY_IGNORE_LIST];

static bool is_ignored(const char *host, uint16_t port)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < HMI_DISCOVERY_IGNORE_LIST; i++) {
        if (s_ignored[i].port == port && strcmp(s_ignored[i].host, host) == 0) {
            if ((int32_t)(s_ignored[i].until - now) > 0) return true;
            s_ignored[i].port = 0;
            return false;
        }
    }
    return false;
}

static void ignore(const char *host, uint16_t port)
{
    int slot = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < HMI_DISCOVERY_IGNORE_LIST; i++) {
        if (s_ignored[i].port == 0 || (int32_t)(s_ignored[i].until - now) <= 0) { slot = i; break; }
        if ((int32_t)(s_ignored[i].until - s_ignored[slot].until) < 0) slot = i;   /* else evict the oldest */
    }
    snprintf(s_ignored[slot].host, sizeof(s_ignored[slot].host), "%s", host);
    s_ignored[slot].port = port;
    s_ignored[slot].until = now + HMI_DISCOVERY_IGNORE_MS;
}

static range_t  s_ranges[HMI_DISCOVERY_MAX_RANGES];
static uint8_t  s_nranges;
static hmi_discovery_stats_t s_stats;

/* ------------------------------------------------------------------------- */
/* Target specs                                                               */
/* ------------------------------------------------------------------------- */
static bool parse_ipv4(const char *s, size_t n, uint32_t *out)
{
    char buf[16];
    if (n == 0 || n >= sizeof(buf)) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    if (strcmp(buf, "localhost") == 0) { *out = 0x7F000001u; return true; }
    struct in_addr a;
    if (inet_pton(AF_INET, buf, &a) != 1) return false;
    *out = ntohl(a.s_addr);
    return true;
}

bool hmi_discovery_add(const char *spec, bool explicit_target)
{
    if (!spec || s_nranges >= HMI_DISCOVERY_MAX_RANGES) return false;
    range_t r;
    memset(&r, 0, sizeof(r));
    r.unit = 1;
    r.explicit_target = explicit_target;
    r.port_first = r.port_last = HMI_MODBUS_DEFAULT_PORT;

    size_t n = strcspn(spec, "/:");
    uint32_t ip;
    if (!parse_ipv4(spec, n, &ip)) return false;
    r.ip_first = r.ip_last = ip;
    const char *p = spec + n;
    char *end;

    if (*p == '/' && p[1] != 'u') {                         /* subnet prefix */
        unsigned long bits = strtoul(p + 1, &end, 10);
        if (end == p + 1 || bits < 16 || bits > 32) return false;   /* at most a /16 */
        uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1u);
        r.ip_first = (ip & mask) + (bits < 31 ? 1u : 0u);   /* skip network address */
        r.ip_last = (ip | ~mask) - (bits < 31 ? 1u : 0u);   /* skip broadcast */
        p = end;
    }
    if (*p == ':') {                                        /* port or port range */
        unsigned long a = strtoul(p + 1, &end, 10), b = a;
        if (end == p + 1) return false;
        if (*end == '-') { const char *q = end + 1; b = strtoul(q, &end, 10); if (end == q) return false; }
        if (a == 0 || b < a || b > 65535 || b - a > 256) return false;
        r.port_first = (uint16_t)a;
        r.port_last = (uint16_t)b;
        p = end;
    }
    if (p[0] == '/' && p[1] == 'u') {                       /* unit id */
        unsigned long u = strtoul(p + 2, &end, 10);
        if (end == p + 2 || u > 255) return false;
        r.unit = (uint8_t)u;
        p = end;
    }
    if (*p != '\0') return false;

    /* A sweep probes at most HMI_DISCOVERY_MAX_TARGETS addresses, taking ranges in the order they
     * were added. Accepting a spec that does not fit would silently skip its tail and starve every
     * range added after it (e.g. an explicit --plc address), so refuse it here instead. */
    uint64_t total = (uint64_t)(r.ip_last - r.ip_first + 1u) * ((uint64_t)(r.port_last - r.port_first) + 1u);
    for (uint8_t i = 0; i < s_nranges; i++)
        total += (uint64_t)(s_ranges[i].ip_last - s_ranges[i].ip_first + 1u) *
                 ((uint64_t)(s_ranges[i].port_last - s_ranges[i].port_first) + 1u);
    if (total > HMI_DISCOVERY_MAX_TARGETS) return false;

    s_ranges[s_nranges++] = r;
    return true;
}

void hmi_discovery_clear(void)
{
    memset(s_ranges, 0, sizeof(s_ranges));
    s_nranges = 0;
}

void hmi_discovery_add_defaults(void)
{
    hmi_discovery_add("127.0.0.1:502", false);
    hmi_discovery_add("127.0.0.1:1502-1509", false);
}

uint32_t hmi_discovery_target_count(void)
{
    uint32_t n = 0;
    for (uint8_t i = 0; i < s_nranges; i++) {
        uint32_t hosts = s_ranges[i].ip_last - s_ranges[i].ip_first + 1u;
        uint32_t ports = (uint32_t)(s_ranges[i].port_last - s_ranges[i].port_first) + 1u;
        n += hosts * ports;
        if (n > HMI_DISCOVERY_MAX_TARGETS) return HMI_DISCOVERY_MAX_TARGETS;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Probing                                                                    */
/* ------------------------------------------------------------------------- */
#define BATCH 32

typedef enum { PR_IDLE, PR_CONNECTING, PR_WAIT_ID, PR_WAIT_FC03, PR_DONE_ID, PR_DONE_MODBUS, PR_DEAD } probe_state_t;

typedef struct {
    SOCKET   sock;
    uint8_t  state;
    char     host[16];
    uint16_t port;
    uint8_t  unit;
    bool     explicit_target;
    uint8_t  rx[MB_MAX_ADU];
    size_t   rx_len;
    mb_device_id_t id;
} probe_t;

static probe_t s_probe[BATCH];

static void probe_close(probe_t *p, uint8_t final_state)
{
    if (p->sock != INVALID_SOCKET) closesocket(p->sock);
    p->sock = INVALID_SOCKET;
    p->state = final_state;
}

static bool probe_send_fc03(probe_t *p, uint16_t tid)
{
    uint8_t adu[16];
    mb_request_t rq = { tid, p->unit, MB_FC_READ_HOLDING, 0, 1 };
    size_t n = mb_encode_request(&rq, adu, sizeof(adu));
    p->rx_len = 0;
    return n && send(p->sock, (const char *)adu, (int)n, 0) == (int)n;
}

/* Wait (up to timeout) for sockets in `want` state; handle replies. */
static void pump_replies(uint8_t want, uint32_t timeout_ms, volatile long *running)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        int pending = 0;
        for (int i = 0; i < BATCH; i++) if (s_probe[i].state == want) { FD_SET(s_probe[i].sock, &rs); pending++; }
        if (!pending || !*running) return;
        DWORD now = GetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        struct timeval tv = { 0, (long)((deadline - now) * 1000u) };
        if (select(0, &rs, NULL, NULL, &tv) <= 0) break;
        for (int i = 0; i < BATCH; i++) {
            probe_t *p = &s_probe[i];
            if (p->state != want || !FD_ISSET(p->sock, &rs)) continue;
            int n = recv(p->sock, (char *)p->rx + p->rx_len, (int)(sizeof(p->rx) - p->rx_len), 0);
            if (n <= 0) { probe_close(p, PR_DEAD); continue; }
            p->rx_len += (size_t)n;
            int32_t need = mb_adu_length(p->rx, p->rx_len);
            if (need < 0) { probe_close(p, PR_DEAD); continue; }
            if (need == 0 || (size_t)need > p->rx_len) continue;
            if (want == PR_WAIT_ID) {
                mb_status_t st = mb_decode_devid(p->rx, (size_t)need, (uint16_t)(0x4300 + i), p->unit, &p->id);
                if (st == MB_RESP_OK && (p->id.vendor[0] || p->id.product[0])) { probe_close(p, PR_DONE_ID); continue; }
                /* no identification: does it speak plain Modbus? */
                if (probe_send_fc03(p, (uint16_t)(0x0300 + i))) p->state = PR_WAIT_FC03;
                else probe_close(p, PR_DEAD);
            } else {
                mb_request_t rq = { (uint16_t)(0x0300 + i), p->unit, MB_FC_READ_HOLDING, 0, 1 };
                static mb_response_t rsp;
                mb_status_t st = mb_decode_response(&rq, p->rx, (size_t)need, &rsp);
                /* an exception is still a valid Modbus answer from a Modbus device */
                probe_close(p, (st == MB_RESP_OK || st == MB_RESP_EXCEPTION) ? PR_DONE_MODBUS : PR_DEAD);
            }
        }
    }
    for (int i = 0; i < BATCH; i++) if (s_probe[i].state == want) probe_close(&s_probe[i], PR_DEAD);
}

static void probe_batch(int count, volatile long *running, hmi_discovery_found_fn found)
{
    /* 1. connect */
    for (int i = 0; i < count; i++) {
        probe_t *p = &s_probe[i];
        p->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (p->sock == INVALID_SOCKET) { p->state = PR_DEAD; continue; }
        u_long nb = 1;
        ioctlsocket(p->sock, FIONBIO, &nb);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(p->port);
        inet_pton(AF_INET, p->host, &a.sin_addr);
        int rc = connect(p->sock, (const struct sockaddr *)&a, sizeof(a));
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { probe_close(p, PR_DEAD); continue; }
        p->state = PR_CONNECTING;
        s_stats.probes++;
    }
    DWORD deadline = GetTickCount() + HMI_DISCOVERY_CONNECT_MS;
    for (;;) {
        fd_set ws, es;
        FD_ZERO(&ws); FD_ZERO(&es);
        int pending = 0;
        for (int i = 0; i < count; i++) if (s_probe[i].state == PR_CONNECTING) { FD_SET(s_probe[i].sock, &ws); FD_SET(s_probe[i].sock, &es); pending++; }
        if (!pending || !*running) break;
        DWORD now = GetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        struct timeval tv = { 0, (long)((deadline - now) * 1000u) };
        if (select(0, NULL, &ws, &es, &tv) <= 0) break;
        for (int i = 0; i < count; i++) {
            probe_t *p = &s_probe[i];
            if (p->state != PR_CONNECTING) continue;
            if (FD_ISSET(p->sock, &es)) { probe_close(p, PR_DEAD); continue; }
            if (FD_ISSET(p->sock, &ws)) {
                uint8_t req[16];
                size_t n = mb_encode_devid_request((uint16_t)(0x4300 + i), p->unit, req, sizeof(req));
                p->rx_len = 0;
                if (n && send(p->sock, (const char *)req, (int)n, 0) == (int)n) p->state = PR_WAIT_ID;
                else probe_close(p, PR_DEAD);
            }
        }
    }
    for (int i = 0; i < count; i++) if (s_probe[i].state == PR_CONNECTING) probe_close(&s_probe[i], PR_DEAD);

    /* 2. identification, then the FC03 fallback */
    pump_replies(PR_WAIT_ID, HMI_DISCOVERY_REPLY_MS, running);
    pump_replies(PR_WAIT_FC03, HMI_DISCOVERY_REPLY_MS, running);

    /* 3. report */
    for (int i = 0; i < count; i++) {
        probe_t *p = &s_probe[i];
        if (p->state != PR_DONE_ID && p->state != PR_DONE_MODBUS) continue;
        /* Only PLCs: a scanned device must match a driver that may auto-attach */
        bool ident = (p->state == PR_DONE_ID);
        const hmi_device_profile_t *prof = ident ? hmi_profile_match(p->id.vendor, p->id.product)
                                                 : hmi_profile_match(NULL, NULL);
        if (!p->explicit_target && !prof->auto_attach) {
            ignore(p->host, p->port);
            s_stats.ignored++;
            continue;
        }
        hmi_device_candidate_t c;
        memset(&c, 0, sizeof(c));
        c.explicit_target = p->explicit_target;
        snprintf(c.host, sizeof(c.host), "%s", p->host);
        c.port = p->port;
        c.unit = p->unit;
        c.identified = (p->state == PR_DONE_ID);
        if (c.identified) c.id = p->id;
        s_stats.found++;
        found(&c);
    }
}

void hmi_discovery_run(volatile long *running, hmi_discovery_attached_fn attached, hmi_discovery_found_fn found)
{
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return;
    s_stats.targets = hmi_discovery_target_count();

    while (*running) {
        DWORD t0 = GetTickCount();
        int count = 0;
        uint32_t budget = HMI_DISCOVERY_MAX_TARGETS;
        for (uint8_t r = 0; r < s_nranges && *running && budget; r++) {
            const range_t *rg = &s_ranges[r];
            for (uint32_t ip = rg->ip_first; ip <= rg->ip_last && *running && budget; ip++) {
                for (uint32_t port = rg->port_first; port <= rg->port_last && budget; port++) {
                    budget--;
                    char host[16];
                    struct in_addr a;
                    a.s_addr = htonl(ip);
                    inet_ntop(AF_INET, &a, host, sizeof(host));
                    if (attached(host, (uint16_t)port, rg->unit)) continue;
                    if (!rg->explicit_target && is_ignored(host, (uint16_t)port)) continue;
                    probe_t *p = &s_probe[count];
                    memset(p, 0, sizeof(*p));
                    p->sock = INVALID_SOCKET;
                    snprintf(p->host, sizeof(p->host), "%s", host);
                    p->port = (uint16_t)port;
                    p->unit = rg->unit;
                    p->explicit_target = rg->explicit_target;
                    if (++count == BATCH) { probe_batch(count, running, found); count = 0; }
                }
                if (ip == 0xFFFFFFFFu) break;
            }
        }
        if (count && *running) probe_batch(count, running, found);
        s_stats.sweeps++;
        s_stats.last_sweep_ms = GetTickCount() - t0;

        for (uint32_t waited = 0; waited < HMI_DISCOVERY_PERIOD_MS && *running; waited += 50u) Sleep(50);
    }
    WSACleanup();
}

void hmi_discovery_get_stats(hmi_discovery_stats_t *out)
{
    if (out) *out = s_stats;   /* plain counters: a torn read only shows a stale number */
}
