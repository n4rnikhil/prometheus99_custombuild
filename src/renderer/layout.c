/**
 * @file layout.c
 * @brief Screen geometry shared by rendering and hit-testing (pure functions)
 */

#include "../../include/renderer/layout.h"
#include "../../include/renderer/bytecode.h"

static hmi_rect_t mk(int x, int y, int w, int h)
{
    hmi_rect_t r;
    r.x = x; r.y = y; r.w = (w > 0) ? w : 0; r.h = (h > 0) ? h : 0;
    return r;
}

bool hmi_rect_contains(hmi_rect_t r, int x, int y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

hmi_rect_t hmi_rect_inset(hmi_rect_t r, int dx, int dy)
{
    return mk(r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy);
}

/* ---------------- Chrome ---------------- */

hmi_rect_t hmi_layout_header(int sw, int sh)
{
    (void)sh;
    return mk(0, 0, sw, HMI_LAYOUT_HEADER_H);
}

hmi_rect_t hmi_layout_banner(int sw, int sh)
{
    (void)sh;
    return mk(HMI_LAYOUT_MARGIN, HMI_LAYOUT_HEADER_H + 10, sw - 2 * HMI_LAYOUT_MARGIN, HMI_LAYOUT_BANNER_H);
}

hmi_rect_t hmi_layout_banner_ack(int sw, int sh)
{
    hmi_rect_t b = hmi_layout_banner(sw, sh);
    int w = 140, h = 38;
    return mk(b.x + b.w - 12 - w, b.y + (b.h - h) / 2, w, h);
}

hmi_rect_t hmi_layout_content(int sw, int sh, bool banner_active)
{
    int top = HMI_LAYOUT_HEADER_H + HMI_LAYOUT_GAP;
    if (banner_active) top += 10 + HMI_LAYOUT_BANNER_H - 2;
    int bottom = sh - HMI_LAYOUT_DOCK_H - HMI_LAYOUT_GAP;
    return mk(HMI_LAYOUT_MARGIN, top, sw - 2 * HMI_LAYOUT_MARGIN, bottom - top);
}

hmi_rect_t hmi_layout_dock(int sw, int sh)
{
    return mk(0, sh - HMI_LAYOUT_DOCK_H, sw, HMI_LAYOUT_DOCK_H);
}

hmi_rect_t hmi_layout_dock_tab(int sw, int sh, int idx)
{
    if (idx < 0 || idx >= SCREEN_COUNT) return mk(0, 0, 0, 0);
    int x0 = (idx * sw) / SCREEN_COUNT;
    int x1 = ((idx + 1) * sw) / SCREEN_COUNT;
    return mk(x0, sh - HMI_LAYOUT_DOCK_H, x1 - x0, HMI_LAYOUT_DOCK_H);
}

/* Header right-hand group, laid out from the right edge. */
#define HDR_BTN_Y   12
#define HDR_BTN_H   32
#define HDR_GAP     8
/* Narrow screens (< 900 px) use a compact header: smaller chips, health as a dot. */
static bool compact(int sw) { return sw < 900; }
#define W_ESTOP(sw)   (compact(sw) ? 84 : 92)
#define W_ROLE(sw)    (compact(sw) ? 100 : 112)
#define W_ALARM(sw)   (compact(sw) ? 100 : 116)
#define W_HEALTH(sw)  (compact(sw) ? 32 : 124)
#define W_THEME       32

hmi_rect_t hmi_layout_estop_button(int sw, int sh)
{
    (void)sh;
    return mk(sw - HMI_LAYOUT_MARGIN - W_ESTOP(sw), HDR_BTN_Y, W_ESTOP(sw), HDR_BTN_H);
}

hmi_rect_t hmi_layout_role_pill(int sw, int sh)
{
    hmi_rect_t e = hmi_layout_estop_button(sw, sh);
    return mk(e.x - HDR_GAP - W_ROLE(sw), HDR_BTN_Y, W_ROLE(sw), HDR_BTN_H);
}

hmi_rect_t hmi_layout_alarm_pill(int sw, int sh)
{
    hmi_rect_t r = hmi_layout_role_pill(sw, sh);
    return mk(r.x - HDR_GAP - W_ALARM(sw), HDR_BTN_Y, W_ALARM(sw), HDR_BTN_H);
}

hmi_rect_t hmi_layout_health_pill(int sw, int sh)
{
    hmi_rect_t a = hmi_layout_alarm_pill(sw, sh);
    return mk(a.x - HDR_GAP - W_HEALTH(sw), HDR_BTN_Y, W_HEALTH(sw), HDR_BTN_H);
}

hmi_rect_t hmi_layout_theme_button(int sw, int sh)
{
    hmi_rect_t hp = hmi_layout_health_pill(sw, sh);
    return mk(hp.x - HDR_GAP - W_THEME, HDR_BTN_Y, W_THEME, HDR_BTN_H);
}

/* ---------------- Manifest widgets ---------------- */

hmi_rect_t hmi_layout_token_rect(uint32_t token, hmi_rect_t c)
{
    int gx = hmi_token_grid_x(token), gy = hmi_token_grid_y(token);
    int gw = hmi_token_grid_w(token), gh = hmi_token_grid_h(token);
    if (gx + gw > HMI_GRID_CELLS) gw = HMI_GRID_CELLS - gx;
    if (gy + gh > HMI_GRID_CELLS) gh = HMI_GRID_CELLS - gy;

    int x0 = c.x + (gx * c.w) / HMI_GRID_CELLS;
    int x1 = c.x + ((gx + gw) * c.w) / HMI_GRID_CELLS;
    int y0 = c.y + (gy * c.h) / HMI_GRID_CELLS;
    int y1 = c.y + ((gy + gh) * c.h) / HMI_GRID_CELLS;
    hmi_rect_t cell = mk(x0, y0, x1 - x0, y1 - y0);

    switch (hmi_token_get_type(token)) {
        case WIDGET_CONTAINER_CARD: return hmi_rect_inset(cell, 6, 0);
        case WIDGET_TANK:           return hmi_rect_inset(cell, 8, 4);
        case WIDGET_PUMP:           return hmi_rect_inset(cell, 18, 4);
        case WIDGET_STATUS_PILL:    return hmi_rect_inset(cell, 8, 3);
        default:                    return hmi_rect_inset(cell, 22, 3);
    }
}

/* ---------------- Fixed screens ---------------- */

hmi_rect_t hmi_layout_screen_body(int sw, int sh, bool banner_active)
{
    hmi_rect_t c = hmi_layout_content(sw, sh, banner_active);
    return mk(c.x, c.y + HMI_LAYOUT_TITLE_H, c.w, c.h - HMI_LAYOUT_TITLE_H);
}

hmi_rect_t hmi_layout_settings_card(int sw, int sh, bool banner_active, int idx)
{
    hmi_rect_t b = hmi_layout_screen_body(sw, sh, banner_active);
    int half = (b.w - HMI_LAYOUT_GAP) / 2;
    if (idx == 0) return mk(b.x, b.y, half, b.h);
    return mk(b.x + half + HMI_LAYOUT_GAP, b.y, b.w - half - HMI_LAYOUT_GAP, b.h);
}

hmi_rect_t hmi_layout_screen_button(int sw, int sh, bool banner_active, hmi_hit_kind_t kind)
{
    hmi_rect_t c = hmi_layout_content(sw, sh, banner_active);
    int bh = 36;
    int by = c.y + (HMI_LAYOUT_TITLE_H - 8 - bh) / 2;
    int right = c.x + c.w;

    switch (kind) {
        /* Alarm screen: title-row buttons, primary action right-most */
        case HMI_HIT_ALARM_ACK:     return mk(right - 150, by, 150, bh);
        case HMI_HIT_ALARM_SILENCE: return mk(right - 150 - 8 - 120, by, 120, bh);
        case HMI_HIT_ESTOP_RESET:   return mk(right - 150 - 8 - 120 - 8 - 124, by, 124, bh);

        /* Settings screen: buttons along the bottom of each card */
        case HMI_HIT_SET_ROLE: {
            hmi_rect_t k = hmi_layout_settings_card(sw, sh, banner_active, 0);
            return mk(k.x + 16, k.y + k.h - 16 - HMI_LAYOUT_BUTTON_H, k.w - 32, HMI_LAYOUT_BUTTON_H);
        }
        case HMI_HIT_SET_THEME:
        case HMI_HIT_SET_WD_TEST:
        case HMI_HIT_SET_WD_RESET: {
            hmi_rect_t k = hmi_layout_settings_card(sw, sh, banner_active, 1);
            int n = (kind == HMI_HIT_SET_THEME) ? 0 : (kind == HMI_HIT_SET_WD_TEST) ? 1 : 2;
            int inner = k.w - 32;
            int bw = (inner - 2 * 8) / 3;
            return mk(k.x + 16 + n * (bw + 8), k.y + k.h - 16 - HMI_LAYOUT_BUTTON_H, bw, HMI_LAYOUT_BUTTON_H);
        }
        default:
            return mk(0, 0, 0, 0);
    }
}

/* ---------------- Dialogs ---------------- */

#define MODAL_TITLE_H  52
#define MODAL_ROW_H    44
#define MODAL_GAP      8
#define PIN_DOTS_H     36
#define PIN_MSG_H      22

/* Numeric keypad: 5 rows of keys, sized to fit the screen height */
#define NP_TOP   132                                   /* title + display + info line */
static int np_key_h(int sh)
{
    int avail = sh - (HMI_LAYOUT_HEADER_H + 4) - 4;
    int kh = (avail - NP_TOP - 8) / 5 - MODAL_GAP;
    return kh < 30 ? 30 : kh > 44 ? 44 : kh;
}

hmi_rect_t hmi_layout_modal(int sw, int sh, uint8_t modal)
{
    int w, h;
    if (modal == HMI_MODAL_ROLE) { w = 380; h = MODAL_TITLE_H + 4 * (MODAL_ROW_H + MODAL_GAP) + 8; }
    else if (modal == HMI_MODAL_PIN) { w = 320; h = MODAL_TITLE_H + PIN_DOTS_H + PIN_MSG_H + 4 * (MODAL_ROW_H + MODAL_GAP) + 8; }
    else if (modal == HMI_MODAL_FACEPLATE) {           /* nearly full screen, over the dock */
        int y = HMI_LAYOUT_HEADER_H + 4;
        return mk(12, y, sw - 24, sh - y - 6);
    }
    else if (modal == HMI_MODAL_NUMPAD) { w = 360; h = NP_TOP + 5 * (np_key_h(sh) + MODAL_GAP) + 8; }
    else if (modal == HMI_MODAL_CONFIRM) { w = 440; h = 196; }
    else return mk(0, 0, 0, 0);
    if (w > sw - 32) w = sw - 32;
    int top = HMI_LAYOUT_HEADER_H + 4;
    int avail = sh - top - 4;
    int y = top + (avail - h) / 2;
    if (y < top) y = top;
    return mk((sw - w) / 2, y, w, h);
}

hmi_rect_t hmi_layout_modal_cancel(int sw, int sh, uint8_t modal)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, modal);
    if (m.w == 0 || modal == HMI_MODAL_CONFIRM) return mk(0, 0, 0, 0);   /* confirm: Cancel at the bottom */
    return mk(m.x + m.w - 12 - 84, m.y + 10, 84, 34);
}

hmi_rect_t hmi_layout_modal_role(int sw, int sh, int role)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_ROLE);
    if (role < 0 || role >= HMI_ROLE_COUNT) return mk(0, 0, 0, 0);
    return mk(m.x + 16, m.y + MODAL_TITLE_H + role * (MODAL_ROW_H + MODAL_GAP), m.w - 32, MODAL_ROW_H);
}

hmi_rect_t hmi_layout_modal_key(int sw, int sh, int key)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_PIN);
    int col, row;
    if (key >= 1 && key <= 9) { col = (key - 1) % 3; row = (key - 1) / 3; }
    else if (key == HMI_KEY_DELETE) { col = 0; row = 3; }
    else if (key == 0) { col = 1; row = 3; }
    else if (key == HMI_KEY_OK) { col = 2; row = 3; }
    else return mk(0, 0, 0, 0);
    int kw = (m.w - 32 - 2 * MODAL_GAP) / 3;
    int ky = m.y + MODAL_TITLE_H + PIN_DOTS_H + PIN_MSG_H;
    return mk(m.x + 16 + col * (kw + MODAL_GAP), ky + row * (MODAL_ROW_H + MODAL_GAP), kw, MODAL_ROW_H);
}

/* ---------------- Faceplate ---------------- */

#define FP_TITLE_H  60
#define FP_ROW_H    46
#define FP_FOOT_H   26

hmi_rect_t hmi_layout_fp_values(int sw, int sh)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_FACEPLATE);
    int w = (m.w - 48) * 40 / 100;
    return mk(m.x + 16, m.y + FP_TITLE_H, w, m.h - FP_TITLE_H - FP_FOOT_H);
}

hmi_rect_t hmi_layout_fp_row(int sw, int sh, int row)
{
    if (row < 0 || row >= HMI_FP_MAX_ROWS) return mk(0, 0, 0, 0);
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_FACEPLATE);
    hmi_rect_t v = hmi_layout_fp_values(sw, sh);
    int x = v.x + v.w + 16;
    int rh = (m.h - FP_TITLE_H - FP_FOOT_H) / HMI_FP_MAX_ROWS;          /* 7 rows always fit */
    if (rh > FP_ROW_H + 6) rh = FP_ROW_H + 6;
    return mk(x, m.y + FP_TITLE_H + row * rh, m.x + m.w - 16 - x, rh - 6);
}

hmi_rect_t hmi_layout_fp_ctrl(int sw, int sh, const hmi_device_profile_t *p, int ctrl)
{
    hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
    uint8_t n = hmi_profile_fp_controls(p, cs, HMI_FP_MAX_CTRLS);
    if (ctrl < 0 || ctrl >= n) return mk(0, 0, 0, 0);
    int k = 0, cnt = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (cs[i].row != cs[ctrl].row) continue;
        if (i < ctrl) k++;
        cnt++;
    }
    hmi_rect_t r = hmi_layout_fp_row(sw, sh, cs[ctrl].row);
    int text_w = r.w * 36 / 100;
    int bx = r.x + text_w, bw_all = r.w - text_w;
    int bw = (bw_all - (cnt - 1) * 6) / cnt;
    int bh = r.h - 6 < 40 ? r.h - 6 : 40;
    return mk(bx + k * (bw + 6), r.y + (r.h - bh) / 2, bw, bh);
}

/* ---------------- Numeric keypad / confirmation ---------------- */

hmi_rect_t hmi_layout_numpad_display(int sw, int sh)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_NUMPAD);
    return mk(m.x + 16, m.y + 56, m.w - 32, 48);
}

hmi_rect_t hmi_layout_numpad_key(int sw, int sh, int key)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_NUMPAD);
    int kh = np_key_h(sh);
    int col, row, span = 1;
    if (key >= 1 && key <= 9) { col = (key - 1) % 3; row = 2 - (key - 1) / 3; }     /* 7 8 9 on top */
    else if (key == HMI_NP_MINUS) { col = 0; row = 3; }
    else if (key == 0)            { col = 1; row = 3; }
    else if (key == HMI_NP_DOT)   { col = 2; row = 3; }
    else if (key == HMI_NP_DEL)   { col = 0; row = 4; }
    else if (key == HMI_NP_OK)    { col = 1; row = 4; span = 2; }
    else return mk(0, 0, 0, 0);
    int kw = (m.w - 32 - 2 * MODAL_GAP) / 3;
    int y = m.y + NP_TOP + row * (kh + MODAL_GAP);
    return mk(m.x + 16 + col * (kw + MODAL_GAP), y, span * kw + (span - 1) * MODAL_GAP, kh);
}

hmi_rect_t hmi_layout_confirm_button(int sw, int sh, bool ok)
{
    hmi_rect_t m = hmi_layout_modal(sw, sh, HMI_MODAL_CONFIRM);
    int bw = (m.w - 32 - MODAL_GAP) / 2;
    return mk(m.x + 16 + (ok ? bw + MODAL_GAP : 0), m.y + m.h - 16 - 44, bw, 44);
}

/* ---------------- Plug-and-play dashboard ---------------- */

hmi_rect_t hmi_layout_device_panel(hmi_rect_t c, int n, int idx)
{
    if (n <= 0 || idx < 0 || idx >= n) return mk(0, 0, 0, 0);
    int cols = 1, rows = 1;
    if (n == 2) { if (c.w * 5 >= c.h * 6) cols = 2; else rows = 2; }
    else if (n == 3) { if (c.w >= 1100) cols = 3; else { cols = 2; rows = 2; } }
    else if (n >= 4) { cols = 2; rows = 2; }
    int col = idx % cols, row = idx / cols;
    int g = HMI_LAYOUT_GAP;
    int x0 = c.x + (col * (c.w + g)) / cols;
    int x1 = c.x + ((col + 1) * (c.w + g)) / cols - g;
    int y0 = c.y + (row * (c.h + g)) / rows;
    int y1 = c.y + ((row + 1) * (c.h + g)) / rows - g;
    return mk(x0, y0, x1 - x0, y1 - y0);
}

hmi_rect_t hmi_layout_device_open(hmi_rect_t p)
{
    return mk(p.x, p.y, p.w, HMI_LAYOUT_DEVICE_HEADER_H - 4);
}

hmi_rect_t hmi_layout_device_body(hmi_rect_t p)
{
    return mk(p.x, p.y + HMI_LAYOUT_DEVICE_HEADER_H, p.w, p.h - HMI_LAYOUT_DEVICE_HEADER_H);
}

bool hmi_layout_device_compact(hmi_rect_t body)
{
    return body.w < HMI_LAYOUT_COMPACT_W || body.h < HMI_LAYOUT_COMPACT_H;
}

const uint32_t* hmi_layout_device_manifest(const hmi_device_profile_t *p, hmi_rect_t body, uint8_t *count)
{
    if (!p) { if (count) *count = 0; return NULL; }
    bool compact = hmi_layout_device_compact(body) && p->ui_compact;
    if (count) *count = compact ? p->n_compact : p->n_full;
    return compact ? p->ui_compact : p->ui_full;
}

/* ---------------- Hit test ---------------- */

static bool hit(hmi_rect_t r, int x, int y)
{
    return hmi_rect_contains(hmi_rect_inset(r, -HMI_LAYOUT_TOUCH_SLOP, -HMI_LAYOUT_TOUCH_SLOP), x, y);
}

static hmi_hit_t result(hmi_hit_kind_t k, uint8_t param, hmi_rect_t r)
{
    hmi_hit_t h;
    h.kind = k; h.param = param; h.rect = r;
    return h;
}

hmi_hit_t hmi_layout_hit_test(int sw, int sh, uint8_t screen, bool banner_active, uint8_t modal,
                              const uint32_t *tokens, uint8_t ntok,
                              const hmi_device_info_t *devs, uint8_t ndev, uint8_t fp_slot, int x, int y)
{
    /* The E-STOP is reachable even while a dialog is open */
    {
        hmi_rect_t e = hmi_layout_estop_button(sw, sh);
        if (y < HMI_LAYOUT_HEADER_H && hit(e, x, y)) return result(HMI_HIT_ESTOP, 0, e);
    }
    if (modal != HMI_MODAL_NONE) {
        hmi_rect_t m = hmi_layout_modal(sw, sh, modal);
        hmi_rect_t c = hmi_layout_modal_cancel(sw, sh, modal);
        if (hmi_rect_contains(c, x, y)) return result(HMI_HIT_MODAL_CANCEL, 0, c);
        if (modal == HMI_MODAL_ROLE) {
            for (int i = 0; i < HMI_ROLE_COUNT; i++) {
                hmi_rect_t r = hmi_layout_modal_role(sw, sh, i);
                if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_MODAL_ROLE, (uint8_t)i, r);
            }
        } else if (modal == HMI_MODAL_PIN) {
            for (int k = 0; k <= HMI_KEY_OK; k++) {
                hmi_rect_t r = hmi_layout_modal_key(sw, sh, k);
                if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_MODAL_KEY, (uint8_t)k, r);
            }
        } else if (modal == HMI_MODAL_FACEPLATE) {
            const hmi_device_profile_t *p = NULL;
            for (uint8_t d = 0; devs && d < ndev; d++) if (devs[d].slot == fp_slot) p = devs[d].profile;
            for (int k = 0; p && k < HMI_FP_MAX_CTRLS; k++) {
                hmi_rect_t r = hmi_layout_fp_ctrl(sw, sh, p, k);
                if (r.w == 0) break;
                if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_FP_CTRL, (uint8_t)k, r);
            }
        } else if (modal == HMI_MODAL_NUMPAD) {
            for (int k = 0; k < HMI_NP_KEYS; k++) {
                hmi_rect_t r = hmi_layout_numpad_key(sw, sh, k);
                if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_NUMPAD_KEY, (uint8_t)k, r);
            }
        } else if (modal == HMI_MODAL_CONFIRM) {
            hmi_rect_t r = hmi_layout_confirm_button(sw, sh, true);
            if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_CONFIRM_OK, 0, r);
            r = hmi_layout_confirm_button(sw, sh, false);
            if (hmi_rect_contains(r, x, y)) return result(HMI_HIT_MODAL_CANCEL, 0, r);
        }
        if (hmi_rect_contains(m, x, y)) return result(HMI_HIT_MODAL_BLOCK, 0, m);
        return result(HMI_HIT_MODAL_CANCEL, 0, m);      /* tap outside closes the dialog */
    }

    /* Dock */
    if (y >= sh - HMI_LAYOUT_DOCK_H) {
        for (int i = 0; i < SCREEN_COUNT; i++) {
            hmi_rect_t t = hmi_layout_dock_tab(sw, sh, i);
            if (hmi_rect_contains(t, x, y)) return result(HMI_HIT_NAV_TAB, (uint8_t)i, t);
        }
        return result(HMI_HIT_NONE, 0, mk(0, 0, 0, 0));
    }

    /* Header */
    if (y < HMI_LAYOUT_HEADER_H) {
        hmi_rect_t r;
        r = hmi_layout_estop_button(sw, sh); if (hit(r, x, y)) return result(HMI_HIT_ESTOP, 0, r);
        r = hmi_layout_role_pill(sw, sh);    if (hit(r, x, y)) return result(HMI_HIT_ROLE, 0, r);
        r = hmi_layout_theme_button(sw, sh);  if (hit(r, x, y)) return result(HMI_HIT_THEME, 0, r);
        return result(HMI_HIT_NONE, 0, mk(0, 0, 0, 0));
    }

    /* Banner */
    if (banner_active) {
        hmi_rect_t r = hmi_layout_banner_ack(sw, sh);
        if (hit(r, x, y)) return result(HMI_HIT_BANNER_ACK, 0, r);
        if (hmi_rect_contains(hmi_layout_banner(sw, sh), x, y)) return result(HMI_HIT_NONE, 0, mk(0, 0, 0, 0));
    }

    /* Screen-specific buttons */
    static const hmi_hit_kind_t alarm_btns[] = { HMI_HIT_ALARM_ACK, HMI_HIT_ALARM_SILENCE, HMI_HIT_ESTOP_RESET };
    static const hmi_hit_kind_t set_btns[]   = { HMI_HIT_SET_ROLE, HMI_HIT_SET_THEME, HMI_HIT_SET_WD_TEST, HMI_HIT_SET_WD_RESET };
    const hmi_hit_kind_t *btns = NULL;
    int nb = 0;
    if (screen == SCREEN_ALARM_MATRIX)      { btns = alarm_btns; nb = 3; }
    if (screen == SCREEN_SECURITY_SETTINGS) { btns = set_btns;   nb = 4; }
    for (int i = 0; i < nb; i++) {
        hmi_rect_t r = hmi_layout_screen_button(sw, sh, banner_active, btns[i]);
        if (hit(r, x, y)) return result(btns[i], 0, r);
    }

    /* Device panels (process screen) */
    if (screen == SCREEN_PROCESS_OVERVIEW && devs) {
        hmi_rect_t c = hmi_layout_content(sw, sh, banner_active);
        for (uint8_t d = 0; d < ndev; d++) {
            hmi_rect_t panel = hmi_layout_device_panel(c, ndev, d);
            hmi_rect_t head = hmi_layout_device_open(panel);
            if (hmi_rect_contains(head, x, y)) return result(HMI_HIT_DEVICE_OPEN, devs[d].slot, head);
            hmi_rect_t body = hmi_layout_device_body(panel);
            uint8_t nt = 0;
            const uint32_t *tk = hmi_layout_device_manifest(devs[d].profile, body, &nt);
            for (uint8_t i = 0; tk && i < nt; i++) {
                if (hmi_token_get_type(tk[i]) != WIDGET_ACTION_BUTTON) continue;
                uint8_t rel = hmi_token_get_tag(tk[i]);
                if (rel >= HMI_DEVICE_TAG_SPAN) continue;
                hmi_rect_t r = hmi_layout_token_rect(tk[i], body);
                if (hit(r, x, y)) return result(HMI_HIT_WIDGET_BUTTON, HMI_DEV_TAG(devs[d].slot, rel), r);
            }
        }
        return result(HMI_HIT_NONE, 0, mk(0, 0, 0, 0));
    }

    /* Fixed-manifest buttons */
    if (tokens) {
        hmi_rect_t c = hmi_layout_content(sw, sh, banner_active);
        for (uint8_t i = 0; i < ntok; i++) {
            if (hmi_token_get_type(tokens[i]) != WIDGET_ACTION_BUTTON) continue;
            hmi_rect_t r = hmi_layout_token_rect(tokens[i], c);
            if (hit(r, x, y)) return result(HMI_HIT_WIDGET_BUTTON, hmi_token_get_tag(tokens[i]), r);
        }
    }
    return result(HMI_HIT_NONE, 0, mk(0, 0, 0, 0));
}
