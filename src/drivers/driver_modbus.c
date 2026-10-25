/**
 * @file driver_modbus.c
 * @brief Schneider Modicon M340/M580 driver: non-blocking Modbus TCP client + process simulator
 * @standards Modbus Application Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b
 *
 * Everything runs inside the 1 kHz fieldbus loop and never blocks:
 *   - connect() is started non-blocking and polled with a zero-timeout select()
 *   - exactly one request is in flight; responses are assembled from partial
 *     recv()s in a static buffer and matched by transaction id
 *   - operator writes go through an 8-entry queue and are sent between reads
 *   - a missing response after HMI_MODBUS_TIMEOUT_MS drops the link, marks
 *     every PLC tag COMM_FAULT, raises the PLC-link alarm and reconnects with
 *     exponential back-off (1 s .. 8 s)
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../include/drivers/driver_modbus.h"
#include "../../include/drivers/modbus_proto.h"
#include "../../include/renderer/bytecode.h"
#include "../../include/hal/timer.h"

/* ------------------------------------------------------------------------- */
/* Tag metadata and UI manifest                                               */
/* ------------------------------------------------------------------------- */
#define s_meta g_schneider_meta
#define s_manifest g_schneider_manifest
#define MANIFEST_COUNT SCHNEIDER_MANIFEST_COUNT

static const uint8_t s_plc_tags[] = {
    SCHNEIDER_TAG_TANK_LEVEL, SCHNEIDER_TAG_PUMP_RPM, SCHNEIDER_TAG_VALVE_POS, SCHNEIDER_TAG_REACTOR_PRESS,
    SCHNEIDER_TAG_COOLANT_TEMP, SCHNEIDER_TAG_PUMP_RUNNING, SCHNEIDER_TAG_PUMP_B_RUN, SCHNEIDER_TAG_ESTOP_ACTIVE
};

static hmi_driver_stats_t s_stats;

/* ------------------------------------------------------------------------- */
/* Process simulator (default mode)                                           */
/* ------------------------------------------------------------------------- */
static struct {
    int32_t  level_x100;    /* tank level, raw units x 100 (raw 720 = 72.0 %) */
    int32_t  rpm;
    int32_t  valve;         /* % */
    int32_t  temp_x10;
    int32_t  press;
    bool     pump_a, pump_b, estop;
    uint32_t last_ms;
    uint32_t rng;
} s_sim;

static void sim_reset(void)
{
    memset(&s_sim, 0, sizeof(s_sim));
    s_sim.level_x100 = 72000;
    s_sim.rpm = 1450;
    s_sim.valve = 65;
    s_sim.temp_x10 = 560;
    s_sim.press = 130;
    s_sim.pump_a = true;
    s_sim.rng = 0x12345678u;
}

static int32_t sim_noise(int32_t amplitude)
{
    s_sim.rng = s_sim.rng * 1664525u + 1013904223u;
    if (amplitude <= 0) return 0;
    return (int32_t)((s_sim.rng >> 16) % (uint32_t)(2 * amplitude + 1)) - amplitude;
}

static void sim_step(void)   /* one 100 ms step */
{
    if (s_sim.estop) { s_sim.pump_a = false; s_sim.pump_b = false; }

    int32_t target_rpm = s_sim.pump_a ? 1450 : 0;
    if (s_sim.rpm < target_rpm) s_sim.rpm += 60;
    if (s_sim.rpm > target_rpm) s_sim.rpm -= s_sim.estop ? 150 : 80;
    if (s_sim.rpm < 0) s_sim.rpm = 0;
    if (s_sim.pump_a && s_sim.rpm >= 1440) s_sim.rpm = 1450 + sim_noise(6);

    /* Tank: inflow from pump A, outflow through V-101 (raw x100 per step) */
    int32_t inflow = (60 * s_sim.rpm) / 1450;
    int32_t outflow = (s_sim.valve * 9) / 10;
    if (s_sim.level_x100 < 2000) outflow = (outflow * s_sim.level_x100) / 2000;   /* gravity drain slows when low */
    s_sim.level_x100 += inflow - outflow + sim_noise(3);
    if (s_sim.level_x100 < 0) s_sim.level_x100 = 0;
    if (s_sim.level_x100 > 100000) s_sim.level_x100 = 100000;

    /* Line pressure follows pump speed and valve throttling */
    int32_t target_p = 20 + (s_sim.rpm * 95) / 1450 + ((100 - s_sim.valve) * 40) / 100;
    s_sim.press += (target_p - s_sim.press) / 4 + sim_noise(1);

    /* Coolant temperature: first-order lag */
    int32_t target_t = 300 + (s_sim.rpm * 290) / 1450 - (s_sim.pump_b ? 140 : 0);
    s_sim.temp_x10 += (target_t - s_sim.temp_x10) / 40;
}

static void sim_publish(hmi_tag_table_t *tags)
{
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_TANK_LEVEL, (int16_t)(s_sim.level_x100 / 100), QUALITY_SIMULATED);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_PUMP_RPM, (int16_t)s_sim.rpm, QUALITY_SIMULATED);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_VALVE_POS, (int16_t)s_sim.valve, QUALITY_SIMULATED);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_REACTOR_PRESS, (int16_t)s_sim.press, QUALITY_SIMULATED);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_COOLANT_TEMP, (int16_t)(s_sim.temp_x10 / 10), QUALITY_SIMULATED);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_RUNNING, s_sim.pump_a, QUALITY_SIMULATED);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_B_RUN, s_sim.pump_b, QUALITY_SIMULATED);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_ESTOP_ACTIVE, s_sim.estop, QUALITY_SIMULATED);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_COMM_LOSS, false, QUALITY_GOOD);
}

static void sim_poll(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (s_sim.last_ms == 0) s_sim.last_ms = now_ms;
    uint32_t steps = 0;
    while ((uint32_t)(now_ms - s_sim.last_ms) >= 100u && steps < 50u) {
        s_sim.last_ms += 100u;
        sim_step();
        steps++;
    }
    if ((uint32_t)(now_ms - s_sim.last_ms) >= 100u) s_sim.last_ms = now_ms;   /* after a long stall */
    sim_publish(tags);
    s_stats.last_good_ms = now_ms;
}

static bool sim_write(bool coil, uint16_t addr, int16_t value)
{
    if (coil) {
        if (addr == MB_COIL_PUMP_A)      s_sim.pump_a = value && !s_sim.estop;
        else if (addr == MB_COIL_PUMP_B) s_sim.pump_b = value && !s_sim.estop;
        else if (addr == MB_COIL_ESTOP)  s_sim.estop = value != 0;
        else { s_stats.writes_failed++; return false; }
    } else {
        if (addr != MB_HR_VALVE_POS) { s_stats.writes_failed++; return false; }
        s_sim.valve = value < 0 ? 0 : value > 100 ? 100 : value;
    }
    s_stats.writes_ok++;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Modbus TCP client                                                          */
/* ------------------------------------------------------------------------- */
typedef enum { STEP_NONE, STEP_READ_REGS, STEP_READ_COILS, STEP_WRITE } tcp_step_t;

typedef struct { bool coil; uint16_t addr; int16_t value; } pending_write_t;

static struct {
    SOCKET   sock;
    bool     wsa_ok;
    struct sockaddr_in addr;
    uint8_t  unit;
    uint16_t tid;
    uint32_t next_try_ms;
    uint32_t backoff_ms;
    uint32_t connect_start_ms;
    uint32_t last_cycle_ms;
    bool     cycle_due_now;
    bool     ever_failed;

    tcp_step_t   step;           /* request in flight */
    mb_request_t req;
    uint32_t     sent_ms;
    uint64_t     sent_us;
    uint8_t      rx[MB_MAX_ADU * 2];
    size_t       rx_len;

    /* results of the current read cycle */
    uint16_t regs[MB_HR_COUNT];
    bool     regs_ok;

    pending_write_t wq[HMI_MODBUS_WRITE_QUEUE];
    uint8_t  wq_head, wq_count;
} s_tcp;

static mb_response_t s_rsp;      /* static: ~520 bytes kept off the stack */

static void tcp_mark_down(hmi_tag_table_t *tags)
{
    if (!tags) return;
    for (unsigned i = 0; i < sizeof(s_plc_tags); i++) hmi_tag_set_quality(tags, s_plc_tags[i], QUALITY_COMM_FAULT);
    if (s_tcp.ever_failed) hmi_tag_write_digital(tags, SCHNEIDER_TAG_COMM_LOSS, true, QUALITY_GOOD);
}

static void tcp_close(hmi_tag_table_t *tags, uint32_t now_ms, bool failure)
{
    if (s_tcp.sock != INVALID_SOCKET) closesocket(s_tcp.sock);
    s_tcp.sock = INVALID_SOCKET;
    s_tcp.step = STEP_NONE;
    s_tcp.rx_len = 0;
    s_stats.link_state = HMI_LINK_DOWN;
    if (failure) {
        s_tcp.ever_failed = true;
        s_tcp.next_try_ms = now_ms + s_tcp.backoff_ms;
        s_tcp.backoff_ms *= 2u;
        if (s_tcp.backoff_ms > HMI_MODBUS_BACKOFF_MAX_MS) s_tcp.backoff_ms = HMI_MODBUS_BACKOFF_MAX_MS;
    }
    /* queued writes are dropped: they were requested against a state we can no longer see */
    s_stats.writes_failed += s_tcp.wq_count;
    s_tcp.wq_count = 0;
    tcp_mark_down(tags);
}

static void tcp_start_connect(uint32_t now_ms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { s_tcp.next_try_ms = now_ms + s_tcp.backoff_ms; return; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    int rc = connect(s, (const struct sockaddr *)&s_tcp.addr, sizeof(s_tcp.addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        s_tcp.ever_failed = true;
        s_tcp.next_try_ms = now_ms + s_tcp.backoff_ms;
        return;
    }
    s_tcp.sock = s;
    s_tcp.connect_start_ms = now_ms;
    s_stats.link_state = HMI_LINK_CONNECTING;
}

/* 1 = connected, 0 = still pending, -1 = failed */
static int tcp_check_connect(void)
{
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s_tcp.sock, &w); FD_SET(s_tcp.sock, &e);
    struct timeval tv = { 0, 0 };
    int n = select(0, NULL, &w, &e, &tv);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return 0;
    if (FD_ISSET(s_tcp.sock, &e)) return -1;
    return FD_ISSET(s_tcp.sock, &w) ? 1 : 0;
}

static bool tcp_send(tcp_step_t step, uint8_t fc, uint16_t addr, uint16_t qv, uint32_t now_ms)
{
    uint8_t adu[MB_MAX_ADU];
    s_tcp.req.tid = ++s_tcp.tid;
    s_tcp.req.unit = s_tcp.unit;
    s_tcp.req.fc = fc;
    s_tcp.req.addr = addr;
    s_tcp.req.qty_or_value = qv;
    size_t len = mb_encode_request(&s_tcp.req, adu, sizeof(adu));
    if (len == 0) return false;
    int n = send(s_tcp.sock, (const char *)adu, (int)len, 0);
    if (n != (int)len) return false;   /* a 12-byte send is atomic on a healthy socket */
    s_stats.requests_sent++;
    s_tcp.step = step;
    s_tcp.sent_ms = now_ms;
    s_tcp.sent_us = hmi_timer_get_us();
    /* rx is left alone: any late frame for an older request is dropped by its transaction id */
    return true;
}

static void tcp_apply_cycle(hmi_tag_table_t *tags, const mb_response_t *coils, uint32_t now_ms)
{
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_TANK_LEVEL,    (int16_t)s_tcp.regs[MB_HR_TANK_LEVEL], QUALITY_GOOD);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_PUMP_RPM,      (int16_t)s_tcp.regs[MB_HR_PUMP_RPM],   QUALITY_GOOD);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_VALVE_POS,     (int16_t)s_tcp.regs[MB_HR_VALVE_POS],  QUALITY_GOOD);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_REACTOR_PRESS, (int16_t)s_tcp.regs[MB_HR_PRESSURE],   QUALITY_GOOD);
    hmi_tag_write_analog(tags, SCHNEIDER_TAG_COOLANT_TEMP,  (int16_t)s_tcp.regs[MB_HR_TEMP],       QUALITY_GOOD);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_RUNNING, mb_coil_get(coils, MB_COIL_PUMP_A), QUALITY_GOOD);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_B_RUN,   mb_coil_get(coils, MB_COIL_PUMP_B), QUALITY_GOOD);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_ESTOP_ACTIVE, mb_coil_get(coils, MB_COIL_ESTOP),  QUALITY_GOOD);
    hmi_tag_write_digital(tags, SCHNEIDER_TAG_COMM_LOSS, false, QUALITY_GOOD);
    s_stats.last_good_ms = now_ms;
}

/* Handle one complete response frame. Returns false if the link must be dropped. */
static bool tcp_on_frame(hmi_tag_table_t *tags, const uint8_t *adu, size_t len, uint32_t now_ms)
{
    mb_status_t st = mb_decode_response(&s_tcp.req, adu, len, &s_rsp);
    if (st == MB_ERR_TID) { s_stats.protocol_errors++; return true; }   /* stale frame: keep waiting */

    uint32_t rtt = (uint32_t)(hmi_timer_get_us() - s_tcp.sent_us);
    s_stats.last_rtt_us = rtt;
    if (rtt > s_stats.max_rtt_us) s_stats.max_rtt_us = rtt;
    tcp_step_t step = s_tcp.step;
    s_tcp.step = STEP_NONE;

    if (st == MB_RESP_EXCEPTION) {
        s_stats.exceptions++;
        if (step == STEP_WRITE) s_stats.writes_failed++;
        else { s_tcp.regs_ok = false; tcp_mark_down(tags); }
        return true;
    }
    if (st != MB_RESP_OK) { s_stats.protocol_errors++; return false; }

    s_stats.responses_ok++;
    s_stats.link_state = HMI_LINK_UP;
    s_tcp.backoff_ms = HMI_MODBUS_BACKOFF_MIN_MS;
    switch (step) {
        case STEP_READ_REGS:
            for (int i = 0; i < MB_HR_COUNT; i++) s_tcp.regs[i] = s_rsp.regs[i];
            s_tcp.regs_ok = true;
            return tcp_send(STEP_READ_COILS, MB_FC_READ_COILS, MB_COIL_PUMP_A, MB_COIL_COUNT, now_ms);
        case STEP_READ_COILS:
            if (s_tcp.regs_ok) tcp_apply_cycle(tags, &s_rsp, now_ms);
            s_tcp.regs_ok = false;
            break;
        case STEP_WRITE:
            s_stats.writes_ok++;
            s_tcp.cycle_due_now = true;      /* read back the effect promptly */
            break;
        default: break;
    }
    return true;
}

static void tcp_poll(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (s_tcp.sock == INVALID_SOCKET) {
        tcp_mark_down(tags);
        if ((int32_t)(now_ms - s_tcp.next_try_ms) >= 0) tcp_start_connect(now_ms);
        return;
    }

    if (s_stats.link_state == HMI_LINK_CONNECTING) {
        int c = tcp_check_connect();
        if (c < 0 || (uint32_t)(now_ms - s_tcp.connect_start_ms) > HMI_MODBUS_CONNECT_TIMEOUT_MS) {
            tcp_close(tags, now_ms, true);
            return;
        }
        if (c == 0) { tcp_mark_down(tags); return; }
        s_stats.link_state = HMI_LINK_UP;
        s_stats.reconnects++;
        s_tcp.cycle_due_now = true;
    }

    /* Receive whatever has arrived */
    if (s_tcp.step != STEP_NONE) {
        for (;;) {
            if (s_tcp.rx_len >= sizeof(s_tcp.rx)) { s_stats.protocol_errors++; tcp_close(tags, now_ms, true); return; }
            int n = recv(s_tcp.sock, (char *)s_tcp.rx + s_tcp.rx_len, (int)(sizeof(s_tcp.rx) - s_tcp.rx_len), 0);
            if (n > 0) { s_tcp.rx_len += (size_t)n; continue; }
            if (n == 0) { tcp_close(tags, now_ms, true); return; }            /* peer closed */
            if (WSAGetLastError() != WSAEWOULDBLOCK) { tcp_close(tags, now_ms, true); return; }
            break;
        }
        while (s_tcp.step != STEP_NONE && s_tcp.rx_len > 0) {
            int32_t need = mb_adu_length(s_tcp.rx, s_tcp.rx_len);
            if (need < 0) { s_stats.protocol_errors++; tcp_close(tags, now_ms, true); return; }
            if (need == 0 || (size_t)need > s_tcp.rx_len) break;
            /* consume the frame BEFORE acting on it: handling may send the next request */
            static uint8_t frame[MB_MAX_ADU];
            size_t flen = (size_t)need;
            if (flen > sizeof(frame)) { s_stats.protocol_errors++; tcp_close(tags, now_ms, true); return; }
            memcpy(frame, s_tcp.rx, flen);
            memmove(s_tcp.rx, s_tcp.rx + flen, s_tcp.rx_len - flen);
            s_tcp.rx_len -= flen;
            if (!tcp_on_frame(tags, frame, flen, now_ms)) { tcp_close(tags, now_ms, true); return; }
        }
        if (s_tcp.step != STEP_NONE && (uint32_t)(now_ms - s_tcp.sent_ms) > HMI_MODBUS_TIMEOUT_MS) {
            s_stats.timeouts++;
            if (s_tcp.step == STEP_WRITE) s_stats.writes_failed++;
            tcp_close(tags, now_ms, true);
        }
        if (s_tcp.step != STEP_NONE || s_tcp.sock == INVALID_SOCKET) return;
    }

    /* Idle: writes first, then the cyclic read */
    if (s_tcp.wq_count) {
        pending_write_t *w = &s_tcp.wq[s_tcp.wq_head];
        s_tcp.wq_head = (uint8_t)((s_tcp.wq_head + 1u) % HMI_MODBUS_WRITE_QUEUE);
        s_tcp.wq_count--;
        bool ok = w->coil ? tcp_send(STEP_WRITE, MB_FC_WRITE_SINGLE_COIL, w->addr, w->value ? 1u : 0u, now_ms)
                          : tcp_send(STEP_WRITE, MB_FC_WRITE_SINGLE_REG, w->addr, (uint16_t)w->value, now_ms);
        if (!ok) { s_stats.writes_failed++; tcp_close(tags, now_ms, true); }
        return;
    }
    if (s_tcp.cycle_due_now || (uint32_t)(now_ms - s_tcp.last_cycle_ms) >= HMI_MODBUS_POLL_MS) {
        s_tcp.cycle_due_now = false;
        s_tcp.last_cycle_ms = now_ms;
        if (!tcp_send(STEP_READ_REGS, MB_FC_READ_HOLDING, MB_HR_TANK_LEVEL, MB_HR_COUNT, now_ms)) {
            tcp_close(tags, now_ms, true);
        }
    }
}

static bool tcp_queue_write(bool coil, uint16_t addr, int16_t value)
{
    if (s_stats.link_state != HMI_LINK_UP || s_tcp.wq_count >= HMI_MODBUS_WRITE_QUEUE) {
        s_stats.writes_failed++;
        return false;
    }
    uint8_t idx = (uint8_t)((s_tcp.wq_head + s_tcp.wq_count) % HMI_MODBUS_WRITE_QUEUE);
    s_tcp.wq[idx].coil = coil;
    s_tcp.wq[idx].addr = addr;
    s_tcp.wq[idx].value = value;
    s_tcp.wq_count++;
    return true;
}

/* "a.b.c.d[:port][/unit]" or "localhost[...]" */
static bool parse_endpoint(const char *ep)
{
    char host[48];
    unsigned long port = HMI_MODBUS_DEFAULT_PORT, unit = 1;
    size_t n = 0;
    while (ep[n] && ep[n] != ':' && ep[n] != '/' && n + 1 < sizeof(host)) { host[n] = ep[n]; n++; }
    host[n] = '\0';
    const char *p = ep + n;
    if (*p == ':') { char *end; port = strtoul(p + 1, &end, 10); p = end; }
    if (*p == '/') { char *end; unit = strtoul(p + 1, &end, 10); p = end; }
    if (*p != '\0' || port == 0 || port > 65535 || unit > 255) return false;
    if (strcmp(host, "localhost") == 0) snprintf(host, sizeof(host), "127.0.0.1");

    memset(&s_tcp.addr, 0, sizeof(s_tcp.addr));
    s_tcp.addr.sin_family = AF_INET;
    s_tcp.addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &s_tcp.addr.sin_addr) != 1) return false;
    s_tcp.unit = (uint8_t)unit;
    snprintf(s_stats.endpoint, sizeof(s_stats.endpoint), "%.32s:%lu", host, port);
    s_stats.unit_id = (uint8_t)unit;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Driver interface                                                           */
/* ------------------------------------------------------------------------- */
static bool modbus_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_tcp, 0, sizeof(s_tcp));
    s_tcp.sock = INVALID_SOCKET;
    s_tcp.backoff_ms = HMI_MODBUS_BACKOFF_MIN_MS;
    sim_reset();
    s_stats.mode = HMI_DRV_MODE_SIMULATOR;
    s_stats.link_state = HMI_LINK_UP;
    s_stats.poll_period_ms = 100;
    snprintf(s_stats.endpoint, sizeof(s_stats.endpoint), "Built-in simulator");
    return true;
}

static bool modbus_connect(const char *endpoint)
{
    if (!endpoint || !endpoint[0]) {
        s_stats.mode = HMI_DRV_MODE_SIMULATOR;
        s_stats.link_state = HMI_LINK_UP;
        return true;
    }
    s_stats.mode = HMI_DRV_MODE_MODBUS_TCP;
    s_stats.link_state = HMI_LINK_DOWN;
    s_stats.poll_period_ms = HMI_MODBUS_POLL_MS;
    if (!s_tcp.wsa_ok) {
        WSADATA wd;
        s_tcp.wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    }
    if (!parse_endpoint(endpoint)) {
        snprintf(s_stats.endpoint, sizeof(s_stats.endpoint), "INVALID: %.38s", endpoint);
        s_tcp.ever_failed = true;
        if (s_tcp.wsa_ok) { WSACleanup(); s_tcp.wsa_ok = false; }   /* poll then only reports the link as down */
        return false;
    }
    s_tcp.next_try_ms = hmi_timer_get_ms();
    return s_tcp.wsa_ok;
}

static void modbus_disconnect(void)
{
    if (s_tcp.sock != INVALID_SOCKET) closesocket(s_tcp.sock);
    s_tcp.sock = INVALID_SOCKET;
    if (s_tcp.wsa_ok) { WSACleanup(); s_tcp.wsa_ok = false; }
    if (s_stats.mode == HMI_DRV_MODE_MODBUS_TCP) s_stats.link_state = HMI_LINK_DOWN;
}

static bool modbus_poll_cyclic(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!tags) return false;
    if (s_stats.mode == HMI_DRV_MODE_SIMULATOR) { sim_poll(tags, now_ms); return true; }
    if (!s_tcp.wsa_ok) { tcp_mark_down(tags); return false; }
    tcp_poll(tags, now_ms);
    return s_stats.link_state == HMI_LINK_UP;
}

static bool modbus_write_coil(uint16_t coil_addr, bool state)
{
    if (s_stats.mode == HMI_DRV_MODE_SIMULATOR) return sim_write(true, coil_addr, state ? 1 : 0);
    return tcp_queue_write(true, coil_addr, state ? 1 : 0);
}

static bool modbus_write_register(uint16_t reg_addr, int16_t value)
{
    if (s_stats.mode == HMI_DRV_MODE_SIMULATOR) return sim_write(false, reg_addr, value);
    return tcp_queue_write(false, reg_addr, value);
}

static void modbus_get_stats(hmi_driver_stats_t *out)
{
    if (out) *out = s_stats;
}

static const hmi_plc_driver_t s_driver = {
    .driver_name    = "Schneider Modicon M340/M580",
    .default_port   = HMI_MODBUS_DEFAULT_PORT,
    .init           = modbus_init,
    .connect        = modbus_connect,
    .disconnect     = modbus_disconnect,
    .poll_cyclic    = modbus_poll_cyclic,
    .write_coil     = modbus_write_coil,
    .write_register = modbus_write_register,
    .get_stats      = modbus_get_stats,
    .ui_bytecode    = s_manifest,
    .widget_count   = MANIFEST_COUNT,
    .tag_metadata   = s_meta,
    .tag_count      = SCHNEIDER_TAG_COUNT
};

const hmi_plc_driver_t* hmi_driver_modbus_get(void)
{
    return &s_driver;
}
