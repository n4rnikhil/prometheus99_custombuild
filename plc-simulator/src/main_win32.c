/**
 * @file main_win32.c
 * @brief PLC Simulator for Windows: a bench of simulated PLCs, drives and meters on the network
 *
 * Every device on the bench is a real Modbus TCP server on its own port (1502..1509 by default,
 * the ports the HMI runtime looks at), so any Modbus TCP client - the Prometheus HMI runtime, a
 * Harmony panel, a SCADA - discovers, polls and controls it exactly like hardware. From here you
 * add and remove devices, plug and unplug them, switch PLCs between RUN and STOP, change process
 * values, force inputs, and inject faults (frozen link, error replies).
 *
 * Command line:  plc_simulator.exe [--network] [--port <first port>] [--add <type>]...
 *   types: tank atv340 atv630 conveyor boiler pumps compressor ahu abb meter unknown
 *
 * One thread: the bench (modbus_server.c) is polled from a 10 ms timer and the window is
 * painted with GDI into an off-screen bitmap.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <timeapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbus_server.h"

#define LAB_MAX_PLC      PLC_MAX_STATIONS
#define LAB_LOG_LINES    64
#define TIMER_POLL       1
#define POLL_MS          10

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */
typedef plc_station_t lab_plc_t;

static char      g_log[LAB_LOG_LINES][120];
static int       g_log_count;
static HWND      g_hwnd;
static int       g_scroll;
static int       g_content_h;
static float     g_scale = 1.0f;
static uint32_t  g_last_paint_ms;

static uint32_t now_ms(void) { return GetTickCount(); }
static int S(int v) { return (int)(v * g_scale + 0.5f); }

static void lab_log(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
static void lab_log(const char *fmt, ...)
{
    char text[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_log_count == LAB_LOG_LINES) {
        memmove(g_log[0], g_log[1], sizeof(g_log[0]) * (LAB_LOG_LINES - 1));
        g_log_count--;
    }
    snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%02u:%02u:%02u  %s", st.wHour, st.wMinute, st.wSecond, text);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

static void on_bench_log(const char *text) { lab_log("%s", text); }

/* Start the HMI runtime: next to this exe, or in the sibling hmi-runtime folder of the repository */
static void open_hmi(void)
{
    char exe[MAX_PATH], dir[MAX_PATH + 32], path[MAX_PATH + 64];
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    char *slash = (n > 0 && n < sizeof(exe)) ? strrchr(exe, '\\') : NULL;
    if (!slash) return;
    *slash = '\0';
    snprintf(dir, sizeof(dir), "%.259s", exe);                          /* next to this app ... */
    snprintf(path, sizeof(path), "%.291s\\industrial_hmi.exe", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {          /* ... or in the repository's hmi-runtime */
        snprintf(dir, sizeof(dir), "%.259s\\..\\hmi-runtime", exe);
        snprintf(path, sizeof(path), "%.291s\\industrial_hmi.exe", dir);
    }
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        lab_log("industrial_hmi.exe not found next to this app or in ..\\hmi-runtime: build the HMI runtime first");
        return;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    char cmd[MAX_PATH + 80];
    /* The bench: simulated devices only, so the factory PINs may be used (the HMI says so on screen). */
    snprintf(cmd, sizeof(cmd), "\"%s\" --allow-factory-pins", path);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        lab_log("HMI runtime started: it finds the plugged-in PLCs within about 3 s");
    } else {
        lab_log("Could not start industrial_hmi.exe (error %lu)", (unsigned long)GetLastError());
    }
}

/* ------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* ------------------------------------------------------------------------- */
#define C_BG      RGB(0x0B, 0x0F, 0x17)
#define C_CARD    RGB(0x16, 0x1F, 0x2E)
#define C_BORDER  RGB(0x26, 0x33, 0x4D)
#define C_HOVER   RGB(0x1E, 0x29, 0x3B)
#define C_TEXT    RGB(0xF8, 0xFA, 0xFC)
#define C_MUTED   RGB(0x94, 0xA3, 0xB8)
#define C_DIM     RGB(0x64, 0x74, 0x8B)
#define C_BLUE    RGB(0x0A, 0x84, 0xFF)
#define C_CYAN    RGB(0x38, 0xBD, 0xF8)
#define C_GREEN   RGB(0x10, 0xB9, 0x81)
#define C_AMBER   RGB(0xF5, 0x9E, 0x0B)
#define C_RED     RGB(0xEF, 0x44, 0x44)
#define C_BTN     RGB(0x22, 0x2C, 0x3D)

typedef enum {
    ACT_NONE, ACT_OPEN_HMI, ACT_LOCAL, ACT_NETWORK, ACT_ADD, ACT_PLUG, ACT_FREEZE, ACT_ERRORS,
    ACT_REMOVE, ACT_MINUS, ACT_PLUS, ACT_COIL, ACT_RUNSTOP
} action_t;

typedef struct { RECT r; uint8_t act, plc, row; } hit_t;
static hit_t g_hits[384];
static int   g_nhits;
static int   g_hover = -1;

static HFONT g_f_title, g_f_head, g_f_body, g_f_small, g_f_value;

static void make_fonts(void)
{
    HFONT *fs[] = { &g_f_title, &g_f_head, &g_f_body, &g_f_small, &g_f_value };
    for (int i = 0; i < 5; i++) if (*fs[i]) DeleteObject(*fs[i]);
    g_f_title = CreateFontW(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_head  = CreateFontW(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_body  = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_small = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_f_value = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void fill_round(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void text_at(HDC dc, HFONT f, COLORREF c, int x, int y, const char *s)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

static int text_w(HDC dc, HFONT f, const char *s)
{
    SIZE sz;
    SelectObject(dc, f);
    GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    return sz.cx;
}

static void text_in(HDC dc, HFONT f, COLORREF c, RECT r, const char *s, UINT align)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static void add_hit(RECT r, action_t act, int plc, int row)
{
    if (g_nhits >= (int)(sizeof(g_hits) / sizeof(g_hits[0]))) return;
    g_hits[g_nhits].r = r;
    g_hits[g_nhits].act = (uint8_t)act;
    g_hits[g_nhits].plc = (uint8_t)plc;
    g_hits[g_nhits].row = (uint8_t)row;
    g_nhits++;
}

/* A button; returns its rect. style: 0 plain, 1 primary, 2 active (toggled), 3 danger */
static RECT button(HDC dc, int x, int y, int w, int h, const char *label, int style, action_t act, int plc, int row)
{
    RECT r = { x, y, x + w, y + h };
    bool hover = (g_hover == g_nhits);   /* hit lists are rebuilt in the same order every paint */
    COLORREF fill = style == 1 ? C_BLUE : style == 2 ? RGB(0x3B, 0x2A, 0x10) : style == 3 ? RGB(0x3A, 0x16, 0x1B) : C_BTN;
    COLORREF border = style == 1 ? C_BLUE : style == 2 ? C_AMBER : style == 3 ? C_RED : C_BORDER;
    COLORREF tc = style == 1 ? RGB(255, 255, 255) : style == 2 ? C_AMBER : style == 3 ? C_RED : C_TEXT;
    if (hover && style == 0) fill = C_HOVER;
    if (hover && style == 1) fill = RGB(0x30, 0x9A, 0xFF);
    fill_round(dc, r, S(10), fill, border);
    text_in(dc, g_f_body, tc, r, label, DT_CENTER);
    add_hit(r, act, plc, row);
    return r;
}


/* Lays out one device card from c.top; draws it when draw is true. Returns its height. */
static int card_layout(HDC dc, int idx, RECT c, bool draw)
{
    lab_plc_t *p = bench_station(idx);
    const sim_type_info_t *ti = sim_type_info(p->sim.type);
    if (draw) fill_round(dc, c, S(18), C_CARD, C_BORDER);

    /* header: status dot, name, identity, status */
    int x = c.left + S(18), y = c.top + S(14);
    if (draw) {
        const char *status;
        COLORREF sc;
        int ncl = p->clients;
        char conn[64];
        const char *first_ip = "";
        for (int k = 0; k < PLC_MAX_CLIENTS; k++) if (p->client_ip[k][0]) { first_ip = p->client_ip[k]; break; }
        if (ncl > 1) snprintf(conn, sizeof(conn), "%d clients: %s ...", ncl, first_ip);
        else snprintf(conn, sizeof(conn), "Client %s", first_ip);
        if (!p->plugged)        { status = "Unplugged";            sc = C_DIM; }
        else if (p->sim.freeze) { status = "Frozen: no replies";   sc = C_RED; }
        else if (p->sim.errors) { status = "Answering with errors"; sc = C_AMBER; }
        else if (p->sim.stopped && ncl > 0) { status = "STOP  -  client connected"; sc = C_AMBER; }
        else if (p->sim.stopped) { status = "STOP"; sc = C_AMBER; }
        else if (ncl > 0)       { status = conn;                   sc = C_GREEN; }
        else if (!ti->is_plc && p->hmi_checked) { status = "Checked by the HMI, ignored"; sc = C_MUTED; }
        else                    { status = ti->app ? "RUN  -  waiting for a client" : "Waiting for a client"; sc = C_CYAN; }
        HBRUSH db = CreateSolidBrush(sc);
        HPEN np = CreatePen(PS_SOLID, 1, sc);
        HGDIOBJ ob = SelectObject(dc, db), op = SelectObject(dc, np);
        Ellipse(dc, x, y + S(6), x + S(10), y + S(16));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(db); DeleteObject(np);
        text_at(dc, g_f_head, C_TEXT, x + S(18), y, p->name);
        char sub[112];
        snprintf(sub, sizeof(sub), "%s  -  %s  -  port %u%s%s", ti->model, ti->product ? ti->product : "no identification",
                 (unsigned)p->port, ti->app ? "  -  program " : "", ti->app ? ti->app : "");
        if (!ti->is_plc && p->sim.type == SIM_METER)
            snprintf(sub, sizeof(sub), "%s  -  port %u  -  not a PLC: add it to the HMI with --plc <ip>:%u@em6400",
                     ti->model, (unsigned)p->port, (unsigned)p->port);
        else if (!ti->is_plc)
            snprintf(sub, sizeof(sub), "%s  -  port %u  -  not a PLC: the HMI should ignore it", ti->model, (unsigned)p->port);
        RECT subr = { x, y + S(24), c.right - S(18), y + S(42) };
        text_in(dc, g_f_small, C_MUTED, subr, sub, DT_LEFT);
        RECT str = { c.right - S(240), y, c.right - S(18), y + S(22) };
        text_in(dc, g_f_small, sc, str, status, DT_RIGHT);
    }

    /* registers: label, value, -/+ for what the lab may change */
    int ry = c.top + S(64);
    for (uint8_t r = 0; r < ti->nrows; r++) {
        const sim_row_t *row = &ti->rows[r];
        if (row->flags & SIM_COIL) continue;
        if (draw) {
            char val[40];
            sim_row_text(&p->sim, r, val, sizeof(val));
            text_at(dc, g_f_body, C_MUTED, c.left + S(18), ry + S(4), row->label);
            if (row->flags & SIM_HMI_W) {
                int lw = text_w(dc, g_f_body, row->label);
                text_at(dc, g_f_small, C_CYAN, c.left + S(26) + lw, ry + S(7), "HMI sets");
            }
            int bx = c.right - S(18) - S(32);
            if ((row->flags & SIM_LAB) && row->step) {
                button(dc, bx, ry, S(32), S(28), "+", 0, ACT_PLUS, idx, r);
                button(dc, bx - S(38), ry, S(32), S(28), "-", 0, ACT_MINUS, idx, r);
            } else {
                RECT ar = { bx - S(38), ry, bx + S(32), ry + S(28) };
                text_in(dc, g_f_small, C_DIM, ar, "process", DT_CENTER);
            }
            RECT vr = { c.left + S(190), ry, bx - S(50), ry + S(28) };
            text_in(dc, g_f_value, C_TEXT, vr, val, DT_RIGHT);
        }
        ry += S(32);
    }

    /* coils: switches (lab inputs), buttons (pulse), status chips (read only) */
    int px = c.left + S(18);
    bool any_coil = false;
    ry += S(6);
    for (uint8_t r = 0; r < ti->nrows; r++) {
        const sim_row_t *row = &ti->rows[r];
        if (!(row->flags & SIM_COIL)) continue;
        any_coil = true;
        bool on = sim_row_value(&p->sim, r) != 0;
        bool pulse = (row->flags & SIM_PULSE) != 0;
        bool lab = (row->flags & SIM_LAB) != 0;
        bool danger = (lab && !(row->flags & SIM_HMI_W)) || strstr(row->label, "stop") || strstr(row->label, "Stop");
        char label[48];
        if (pulse) snprintf(label, sizeof(label), "%s", row->label);
        else snprintf(label, sizeof(label), "%s  %s", row->label, on ? "ON" : "OFF");
        int w = text_w(dc, g_f_body, label) + S(28);
        if (px + w > c.right - S(18) && px > c.left + S(18)) { px = c.left + S(18); ry += S(40); }
        if (draw) {
            RECT pr = { px, ry, px + w, ry + S(32) };
            COLORREF fill = on ? (danger ? RGB(0x4A, 0x16, 0x1B) : RGB(0x0C, 0x3B, 0x2E)) : C_BTN;
            COLORREF bd = on ? (danger ? C_RED : C_GREEN) : (lab ? C_BORDER : C_CARD);
            if (lab && g_hover == g_nhits && !on) fill = C_HOVER;
            fill_round(dc, pr, pulse ? S(10) : S(32), fill, bd);
            text_in(dc, g_f_body, on ? (danger ? C_RED : C_GREEN) : (lab ? C_TEXT : C_DIM), pr, label, DT_CENTER);
            if (lab) add_hit(pr, ACT_COIL, idx, r);
        }
        px += w + S(8);
    }
    if (any_coil) ry += S(40);

    /* footer */
    int fy = ry + S(8);
    if (draw) {
        int fx = c.left + S(18);
        RECT b = button(dc, fx, fy, S(96), S(34), p->plugged ? "Unplug" : "Plug in", p->plugged ? 0 : 1, ACT_PLUG, idx, 0);
        fx = b.right + S(8);
        if (ti->app) {
            b = button(dc, fx, fy, S(96), S(34), p->sim.stopped ? "RUN" : "STOP", p->sim.stopped ? 2 : 0, ACT_RUNSTOP, idx, 0);
            fx = b.right + S(8);
        }
        b = button(dc, fx, fy, S(110), S(34), p->sim.freeze ? "Unfreeze" : "Freeze link", p->sim.freeze ? 2 : 0, ACT_FREEZE, idx, 0);
        fx = b.right + S(8);
        button(dc, fx, fy, S(118), S(34), p->sim.errors ? "Stop errors" : "Error replies", p->sim.errors ? 2 : 0, ACT_ERRORS, idx, 0);
        button(dc, c.right - S(18) - S(84), fy, S(84), S(34), "Remove", 3, ACT_REMOVE, idx, 0);
    }
    return fy + S(34) + S(16) - c.top;
}

static const char * const s_add_labels[SIM_TYPE_COUNT] = {
    [SIM_ATV340] = "+  Altivar ATV340",         [SIM_ATV630] = "+  Altivar ATV630",
    [SIM_METER] = "+  EM6400NG meter",          [SIM_TANK] = "+  Tank skid  (M340)",
    [SIM_CONVEYOR] = "+  Conveyor  (M241)",    [SIM_AHU] = "+  Air handler  (M251)",
    [SIM_BOILER] = "+  Boiler  (M580)",        [SIM_PUMPS] = "+  Pump station  (S7-1200)",
    [SIM_COMPRESSOR] = "+  Compressor  (Micro850)", [SIM_VFD] = "+  ABB drive  (demo map)",
    [SIM_ANON] = "+  Unknown device",
};

static void paint(HDC dc, int w, int h)
{
    g_nhits = 0;
    HBRUSH bg = CreateSolidBrush(C_BG);
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    int log_h = S(150), m = S(24);

    /* header layout first: the "Add" chips wrap, so the header height depends on the width */
    RECT chips[SIM_TYPE_COUNT];
    int ax = m + S(40), ay = S(76);
    for (int t = 0; t < SIM_TYPE_COUNT; t++) {
        int bw = text_w(dc, g_f_body, s_add_labels[t]) + S(28);
        if (ax + bw > w - m && ax > m + S(40)) { ax = m + S(40); ay += S(42); }
        RECT r = { ax, ay, ax + bw, ay + S(34) };
        chips[t] = r;
        ax += bw + S(8);
    }
    int top_h = ay + S(34) + S(14);

    /* cards (scrolling area): rows as tall as their tallest card */
    int cols = w >= S(1060) ? 2 : 1;
    int gap = S(16);
    int cw = (w - 2 * m - gap * (cols - 1)) / cols;
    int order[LAB_MAX_PLC], hgt[LAB_MAX_PLC], n = 0;
    for (int i = 0; i < LAB_MAX_PLC; i++) {
        if (!bench_station(i)) continue;
        RECT probe = { 0, 0, cw, 0 };
        order[n] = i;
        hgt[n] = card_layout(dc, i, probe, false);
        n++;
    }
    int rows = (n + cols - 1) / cols;
    int row_h[LAB_MAX_PLC];
    g_content_h = 0;
    for (int r = 0; r < rows; r++) {
        row_h[r] = 0;
        for (int k = r * cols; k < n && k < (r + 1) * cols; k++) if (hgt[k] > row_h[r]) row_h[r] = hgt[k];
        g_content_h += row_h[r] + gap;
    }
    int view_h = h - top_h - log_h;
    int max_scroll = g_content_h > view_h ? g_content_h - view_h : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    HRGN clip = CreateRectRgn(0, top_h, w, h - log_h);
    SelectClipRgn(dc, clip);
    int yrow = top_h + S(4) - g_scroll;
    for (int r = 0; r < rows; r++) {
        for (int k = r * cols; k < n && k < (r + 1) * cols; k++) {
            int col = k % cols;
            RECT c = { m + col * (cw + gap), yrow, m + col * (cw + gap) + cw, yrow + row_h[r] };
            int first_hit = g_nhits;
            card_layout(dc, order[k], c, true);
            for (int hh = first_hit; hh < g_nhits; hh++)          /* hidden parts of a scrolled card are not clickable */
                if (g_hits[hh].r.bottom <= top_h || g_hits[hh].r.top >= h - log_h) g_hits[hh].act = ACT_NONE;
        }
        yrow += row_h[r] + gap;
    }
    if (n == 0) {
        RECT er = { m, top_h + S(40), w - m, top_h + S(80) };
        text_in(dc, g_f_head, C_TEXT, er, "The bench is empty", DT_CENTER);
        RECT er2 = { m, top_h + S(80), w - m, top_h + S(110) };
        text_in(dc, g_f_body, C_MUTED, er2, "Add a device above, then press Open HMI. Each device is a real Modbus TCP server.", DT_CENTER);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);

    /* header */
    HBRUSH hb = CreateSolidBrush(C_BG);
    RECT hr = { 0, 0, w, top_h };
    FillRect(dc, &hr, hb);
    text_at(dc, g_f_title, C_TEXT, m, S(14), "PLC Simulator");
    char sub[160];
    unsigned b0 = bench_base_port(), b1 = b0 + PLC_MAX_STATIONS - 1;
    if (bench_network()) snprintf(sub, sizeof(sub), "Network mode: other computers and HMI panels reach these devices at %s, ports %u-%u",
                                  bench_lan_ip()[0] ? bench_lan_ip() : "<this PC>", b0, b1);
    else snprintf(sub, sizeof(sub), "Simulated PLCs, drives and meters  -  local mode (this PC only), ports %u-%u", b0, b1);
    text_at(dc, g_f_small, C_MUTED, m, S(44), sub);

    RECT ob = button(dc, w - m - S(130), S(16), S(130), S(38), "Open HMI", 1, ACT_OPEN_HMI, 0, 0);
    int sx = ob.left - S(16) - S(220);
    RECT seg = { sx, S(16), sx + S(220), S(16) + S(38) };
    fill_round(dc, seg, S(12), C_BTN, C_BORDER);
    RECT l = { seg.left + S(3), seg.top + S(3), seg.left + S(110), seg.bottom - S(3) };
    RECT r = { seg.left + S(110), seg.top + S(3), seg.right - S(3), seg.bottom - S(3) };
    fill_round(dc, bench_network() ? r : l, S(10), C_HOVER, C_BORDER);
    text_in(dc, g_f_body, bench_network() ? C_MUTED : C_TEXT, l, "This PC", DT_CENTER);
    text_in(dc, g_f_body, bench_network() ? C_TEXT : C_MUTED, r, "Network", DT_CENTER);
    add_hit(l, ACT_LOCAL, 0, 0);
    add_hit(r, ACT_NETWORK, 0, 0);

    text_at(dc, g_f_small, C_MUTED, m, S(84), "Add");
    for (int t = 0; t < SIM_TYPE_COUNT; t++)
        button(dc, chips[t].left, chips[t].top, chips[t].right - chips[t].left, S(34), s_add_labels[t], 0, ACT_ADD, t, 0);
    HPEN lp = CreatePen(PS_SOLID, 1, C_BORDER);
    HGDIOBJ old = SelectObject(dc, lp);
    MoveToEx(dc, 0, top_h - 1, NULL); LineTo(dc, w, top_h - 1);
    MoveToEx(dc, 0, h - log_h, NULL); LineTo(dc, w, h - log_h);
    SelectObject(dc, old);
    DeleteObject(lp);

    /* activity log */
    RECT lr = { 0, h - log_h + 1, w, h };
    FillRect(dc, &lr, hb);
    DeleteObject(hb);
    text_at(dc, g_f_small, C_MUTED, m, h - log_h + S(10), "ACTIVITY");
    int lines = (log_h - S(36)) / S(20);
    int first = g_log_count > lines ? g_log_count - lines : 0;
    for (int i = first; i < g_log_count; i++)
        text_at(dc, g_f_body, i == g_log_count - 1 ? C_TEXT : C_MUTED, m, h - log_h + S(32) + (i - first) * S(20), g_log[i]);
}

/* ------------------------------------------------------------------------- */
/* Window                                                                     */
/* ------------------------------------------------------------------------- */
static int hit_at(int x, int y)
{
    for (int i = g_nhits - 1; i >= 0; i--) {
        POINT pt = { x, y };
        if (g_hits[i].act != ACT_NONE && PtInRect(&g_hits[i].r, pt)) return i;
    }
    return -1;
}

static void click(int hi)
{
    if (hi < 0) return;
    hit_t h = g_hits[hi];
    switch ((action_t)h.act) {
        case ACT_OPEN_HMI: open_hmi(); InvalidateRect(g_hwnd, NULL, FALSE); return;
        case ACT_LOCAL:    bench_set_network(false); InvalidateRect(g_hwnd, NULL, FALSE); return;
        case ACT_NETWORK:  bench_set_network(true); InvalidateRect(g_hwnd, NULL, FALSE); return;
        case ACT_ADD:      bench_add((sim_type_t)h.plc); InvalidateRect(g_hwnd, NULL, FALSE); return;   /* plc = device type */
        default: break;
    }
    lab_plc_t *p = bench_station(h.plc);
    if (!p) return;
    switch ((action_t)h.act) {
        case ACT_PLUG:
            if (p->plugged) { bench_unplug(h.plc); lab_log("%s unplugged: the HMI should show it offline", p->name); }
            else if (bench_plug(h.plc)) lab_log("%s plugged in on port %u", p->name, (unsigned)p->port);
            break;
        case ACT_RUNSTOP:
            sim_set_stopped(&p->sim, !p->sim.stopped);
            break;
        case ACT_FREEZE:
            p->sim.freeze = !p->sim.freeze;
            lab_log(p->sim.freeze ? "%s: link frozen (requests are not answered)" : "%s: link back to normal", p->name);
            break;
        case ACT_ERRORS:
            p->sim.errors = !p->sim.errors;
            lab_log(p->sim.errors ? "%s: answering every request with exception 04" : "%s: answering normally", p->name);
            break;
        case ACT_REMOVE: bench_remove(h.plc); break;
        case ACT_MINUS:  sim_nudge(&p->sim, h.row, -1); break;
        case ACT_PLUS:   sim_nudge(&p->sim, h.row, +1); break;
        case ACT_COIL: {
            const sim_row_t *row = &sim_type_info(p->sim.type)->rows[h.row];
            if (row->flags & SIM_PULSE) {
                if (!sim_row_value(&p->sim, h.row)) sim_toggle_coil(&p->sim, h.row);
                lab_log("%s: %s pressed on the device", p->name, row->label);
            } else {
                sim_toggle_coil(&p->sim, h.row);
                lab_log("%s: %s forced %s", p->name, row->label, sim_row_value(&p->sim, h.row) ? "ON" : "OFF");
            }
            break;
        }
        default: break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void on_timer(void)
{
    uint32_t ms = now_ms();
    bench_poll(ms);
    if (ms - g_last_paint_ms >= 200u) { g_last_paint_ms = ms; InvalidateRect(g_hwnd, NULL, FALSE); }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            paint(mem, cr.right, cr.bottom);
            BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = S(820);
            mm->ptMinTrackSize.y = S(600);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int hi = hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hi != g_hover) {
                g_hover = hi;
                SetCursor(LoadCursor(NULL, hi >= 0 ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd, NULL, FALSE);
            }
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
            break;
        case WM_LBUTTONUP: click(hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))); return 0;
        case WM_MOUSEWHEEL:
            g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_TIMER: if (wp == TIMER_POLL) on_timer(); return 0;
        case WM_DPICHANGED: {
            g_scale = HIWORD(wp) / 96.0f;
            make_fonts();
            const RECT *r = (const RECT *)lp;
            SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;
    typedef BOOL (WINAPI *dpi_fn)(HANDLE);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    dpi_fn set_ctx = u32 ? (dpi_fn)(void (*)(void))GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (!set_ctx || !set_ctx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
    HDC sdc = GetDC(NULL);
    g_scale = GetDeviceCaps(sdc, LOGPIXELSY) / 96.0f;
    ReleaseDC(NULL, sdc);
    make_fonts();

    /* command line: --network, --port <first port>, --add <type> (repeatable), --open-hmi */
    uint16_t base = PLC_DEFAULT_PORT;
    bool network = false, open_hmi_now = false;
    sim_type_t adds[PLC_MAX_STATIONS];
    int nadds = 0;
    for (int i = 1; i < __argc; i++) {
        const char *a = __argv[i], *next = i + 1 < __argc ? __argv[i + 1] : NULL;
        if (strcmp(a, "--network") == 0) network = true;
        else if (strcmp(a, "--open-hmi") == 0) open_hmi_now = true;   /* one-click bench: start the HMI too */
        else if (strcmp(a, "--port") == 0 && next) { long v = strtol(next, NULL, 10); if (v > 0 && v < 65536 - PLC_MAX_STATIONS) base = (uint16_t)v; i++; }
        else if (strcmp(a, "--add") == 0 && next) {
            sim_type_t t = bench_type_by_name(next);
            if (t != SIM_TYPE_COUNT && nadds < PLC_MAX_STATIONS) adds[nadds++] = t;
            i++;
        }
    }
    if (!bench_init(on_bench_log, base)) return 1;
    bench_set_network(network);
    timeBeginPeriod(1);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));                /* res/plc_simulator.rc */
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = "PrometheusPlcSimulator";
    RegisterClassExA(&wc);
    g_hwnd = CreateWindowExA(0, wc.lpszClassName, "PLC Simulator - simulated PLCs, drives and meters on Modbus TCP", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(860), NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    if (nadds == 0) adds[nadds++] = SIM_TANK;                 /* start with one PLC on the bench */
    for (int i = 0; i < nadds; i++) bench_add(adds[i]);
    lab_log("Press Open HMI (or start the HMI runtime anywhere on the network). Add, unplug or break devices here.");
    SetTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    if (open_hmi_now) open_hmi();

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    bench_shutdown();
    timeEndPeriod(1);
    return 0;
}
