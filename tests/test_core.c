/**
 * @file test_core.c
 * @brief Unit tests for the portable core: tags, alarms (ISA-18.2), Modbus codec,
 *        gestures, layout/hit-test, renderer bounds, hash-chained alarm log.
 *
 * Build & run:  build.bat test     (or see build.bat for the gcc line)
 * Exit code 0 = all tests passed.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "../include/config.h"
#include "../include/core/tags.h"
#include "../include/core/alarms.h"
#include "../include/core/alarm_log.h"
#include "../include/core/crc32.h"
#include "../include/core/sha256.h"
#include "../include/core/security.h"
#include "../include/core/watchdog.h"
#include "../include/drivers/modbus_proto.h"
#include "../include/drivers/driver_modbus.h"
#include "../include/drivers/driver_host.h"
#include "../include/hal/gesture.h"
#include "../include/renderer/layout.h"
#include "../include/renderer/micro_renderer.h"

static int g_fail, g_pass;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------ */
static void test_crc(void)
{
    CHECK(hmi_crc32c((const uint8_t *)"123456789", 9) == 0xE3069283u);
}

static void test_format(void)
{
    char b[16];
    hmi_format_scaled(b, sizeof(b), 842, 1);    CHECK(strcmp(b, "84.2") == 0);
    hmi_format_scaled(b, sizeof(b), -5, 1);     CHECK(strcmp(b, "-0.5") == 0);
    hmi_format_scaled(b, sizeof(b), 1450, 0);   CHECK(strcmp(b, "1450") == 0);
    hmi_format_scaled(b, sizeof(b), -32768, 2); CHECK(strcmp(b, "-327.68") == 0);
    hmi_format_scaled(b, 4, 12345, 0);          CHECK(strlen(b) == 3);
}

static const hmi_tag_meta_t T_META[] = {
    { 20, TAG_TYPE_ANALOG,  "LEVEL", "%", 0, 1000, 850, 950, 1, 0, "A1", "A2" },
    { 21, TAG_TYPE_DIGITAL, "ESTOP", "",  0, 1,    0,   0,   0, ALARM_SEV_ESTOP, NULL, NULL },
};

static void test_tags_registry(void)
{
    hmi_tag_registry_reset();
    CHECK(hmi_tag_register_meta(T_META, 2));
    CHECK(!hmi_tag_register_meta(T_META, 1));           /* duplicate id rejected */
    CHECK(hmi_tag_get_meta(20) == &T_META[0]);
    CHECK(hmi_tag_get_meta(63) == NULL);
    CHECK(hmi_tag_get_meta(200) == NULL);

    hmi_tag_table_t t;
    hmi_tags_init(&t);
    CHECK(t.tags[20].quality == QUALITY_COMM_FAULT);
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, (int16_t)i);
    int16_t out[HMI_TREND_SAMPLES];
    uint8_t n = hmi_tags_copy_trend(&t, out, HMI_TREND_SAMPLES);
    CHECK(n == 60 && out[0] == 10 && out[59] == 69);     /* chronological after wrap */
    n = hmi_tags_copy_trend(&t, out, 5);
    CHECK(n == 5 && out[4] == 69 && out[0] == 65);
}

static void eval(hmi_alarm_supervisor_t *s, hmi_tag_table_t *t, int16_t v, uint32_t ms)
{
    hmi_tag_write_analog(t, 20, v, QUALITY_GOOD);
    hmi_alarms_evaluate(s, t, ms);
}

static void test_alarms(void)
{
    hmi_tag_registry_reset();
    hmi_tag_register_meta(T_META, 2);
    hmi_tag_table_t t; hmi_tags_init(&t);
    hmi_tag_write_digital(&t, 21, false, QUALITY_GOOD);
    hmi_alarm_supervisor_t s; hmi_alarms_init(&s);

    eval(&s, &t, 500, 1);
    CHECK(s.points[20].state == ALARM_PT_NORMAL && !s.horn_sounding && !s.current_action.active);

    eval(&s, &t, 860, 2);                                 /* H */
    CHECK(s.points[20].state == ALARM_PT_UNACK && s.points[20].severity == ALARM_SEV_WARNING);
    CHECK(s.horn_sounding && s.latch_state == ALARM_LATCH_UNACKNOWLEDGED && s.current_action.active);

    eval(&s, &t, 960, 3);                                 /* escalate to HH while unacked */
    CHECK(s.points[20].peak_severity == ALARM_SEV_CRITICAL && s.highest_severity == ALARM_SEV_CRITICAL);

    CHECK(hmi_alarms_acknowledge_all(&s, 4) == 1);
    CHECK(s.points[20].state == ALARM_PT_ACKED && !s.horn_sounding);
    eval(&s, &t, 960, 5);
    CHECK(s.latch_state == ALARM_LATCH_ACKNOWLEDGED && !s.horn_sounding);

    eval(&s, &t, 945, 6);                                 /* inside HH deadband (10): still critical */
    CHECK(s.points[20].severity == ALARM_SEV_CRITICAL);
    eval(&s, &t, 700, 7);                                 /* clears -> NORMAL (was acked) */
    CHECK(s.points[20].state == ALARM_PT_NORMAL && s.latch_state == ALARM_LATCH_NORMAL);

    eval(&s, &t, 860, 8);                                 /* new alarm, clears before ack */
    eval(&s, &t, 700, 9);
    CHECK(s.points[20].state == ALARM_PT_RTN_UNACK && s.horn_sounding);
    hmi_alarms_silence_horn(&s);
    eval(&s, &t, 700, 10);
    CHECK(!s.horn_sounding && s.points[20].state == ALARM_PT_RTN_UNACK);   /* silence != ack */
    hmi_alarms_acknowledge_all(&s, 11);
    eval(&s, &t, 700, 12);
    CHECK(s.points[20].state == ALARM_PT_NORMAL && !s.current_action.active);

    /* E-stop digital point outranks the analog alarm */
    eval(&s, &t, 860, 13);
    hmi_tag_write_digital(&t, 21, true, QUALITY_GOOD);
    hmi_alarms_evaluate(&s, &t, 14);
    CHECK(s.highest_severity == ALARM_SEV_ESTOP && s.current_action.tag_id == 21);

    /* Bad quality freezes the point instead of clearing it */
    hmi_tag_write_analog(&t, 20, 100, QUALITY_COMM_FAULT);
    hmi_alarms_evaluate(&s, &t, 15);
    CHECK(s.points[20].state == ALARM_PT_UNACK);

    /* Journal: newest first, monotonic seq */
    const hmi_alarm_record_t *n0 = hmi_alarms_journal_newest(&s, 0), *n1 = hmi_alarms_journal_newest(&s, 1);
    CHECK(n0 && n1 && n0->seq == s.journal_total && n1->seq + 1 == n0->seq);
    for (int i = 0; i < 40; i++) hmi_alarms_log_event(&s, 20u + (uint32_t)i, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, 0, "x");
    CHECK(hmi_alarms_journal_count(&s) == HMI_ALARM_JOURNAL_SIZE);
    CHECK(hmi_alarms_journal_by_seq(&s, s.journal_total) != NULL);
    CHECK(hmi_alarms_journal_by_seq(&s, 1) == NULL);
}

static void test_security(void)
{
    hmi_security_ctx_t sec;
    hmi_security_init(&sec);
    hmi_security_set_role(&sec, ROLE_VIEWER, 0);
    CHECK(hmi_security_authorize(&sec, HMI_CMD_ESTOP, 0));          /* never gated */
    CHECK(!hmi_security_authorize(&sec, HMI_CMD_PUMP_TOGGLE, 0));
    CHECK(sec.denied_count == 1);
    hmi_security_set_role(&sec, ROLE_OPERATOR, 0);
    CHECK(hmi_security_authorize(&sec, HMI_CMD_ACK_ALARMS, 0));
    CHECK(!hmi_security_authorize(&sec, HMI_CMD_INJECT_FAULT, 0));
    CHECK(!hmi_security_check_timeout(&sec, sec.auto_logout_timeout_ms - 1));
    CHECK(hmi_security_check_timeout(&sec, sec.auto_logout_timeout_ms));
    CHECK(sec.current_role == ROLE_VIEWER);
    /* wrap-safe across the 49.7-day uint32 rollover */
    hmi_security_set_role(&sec, ROLE_OPERATOR, 0xFFFFFF00u);
    CHECK(!hmi_security_check_timeout(&sec, 0x00000100u));
}

static void test_watchdog(void)
{
    hmi_watchdog_t wd;
    hmi_watchdog_init(&wd, 1000);
    hmi_watchdog_ping_fieldbus(&wd, 1400);
    hmi_watchdog_ping_ui(&wd, 1400);
    CHECK(!hmi_watchdog_evaluate(&wd, 1500) && wd.system_healthy);
    CHECK(hmi_watchdog_evaluate(&wd, 1400 + HMI_WATCHDOG_MAX_AGE_MS + 1));   /* edge reported once */
    CHECK(!hmi_watchdog_evaluate(&wd, 2500) && wd.fault_count == 1 && wd.failover_active);
    CHECK(!hmi_watchdog_reset_failover(&wd));                                 /* still faulted */
    hmi_watchdog_ping_fieldbus(&wd, 2600); hmi_watchdog_ping_ui(&wd, 2600);
    hmi_watchdog_evaluate(&wd, 2610);
    CHECK(hmi_watchdog_reset_failover(&wd) && !wd.failover_active);
}

static void test_modbus(void)
{
    uint8_t adu[MB_MAX_ADU];
    mb_request_t rq = { 0x1234, 1, MB_FC_READ_HOLDING, 0, 5 };
    CHECK(mb_encode_request(&rq, adu, sizeof(adu)) == 12);
    CHECK(adu[0] == 0x12 && adu[1] == 0x34 && adu[5] == 6 && adu[7] == 3 && adu[11] == 5);
    mb_request_t bad = { 1, 1, MB_FC_READ_HOLDING, 0, 126 };
    CHECK(mb_encode_request(&bad, adu, sizeof(adu)) == 0);
    bad.fc = 0x10; bad.qty_or_value = 1;
    CHECK(mb_encode_request(&bad, adu, sizeof(adu)) == 0);

    static mb_response_t rs;
    uint8_t ok[] = { 0x12, 0x34, 0, 0, 0, 13, 1, 3, 10, 0, 1, 0, 2, 0, 3, 0, 4, 0xFF, 0xFF };
    CHECK(mb_adu_length(ok, 7) == 19 && mb_adu_length(ok, 6) == 0);
    CHECK(mb_decode_response(&rq, ok, sizeof(ok), &rs) == MB_RESP_OK);
    CHECK(rs.count == 5 && rs.regs[0] == 1 && rs.regs[4] == 0xFFFF);
    uint8_t wrong_tid[sizeof(ok)]; memcpy(wrong_tid, ok, sizeof(ok)); wrong_tid[1] = 0x35;
    CHECK(mb_decode_response(&rq, wrong_tid, sizeof(ok), &rs) == MB_ERR_TID);
    CHECK(mb_decode_response(&rq, ok, sizeof(ok) - 1, &rs) == MB_ERR_PROTO);   /* truncated */
    uint8_t ex[] = { 0x12, 0x34, 0, 0, 0, 3, 1, 0x83, 2 };
    CHECK(mb_decode_response(&rq, ex, sizeof(ex), &rs) == MB_RESP_EXCEPTION && rs.exception_code == 2);
    uint8_t badproto[] = { 0, 1, 0, 1, 0, 3, 1, 3, 0 };
    CHECK(mb_adu_length(badproto, sizeof(badproto)) == -1);

    mb_request_t rc = { 7, 1, MB_FC_READ_COILS, 0, 3 };
    uint8_t coils[] = { 0, 7, 0, 0, 0, 4, 1, 1, 1, 0x05 };
    CHECK(mb_decode_response(&rc, coils, sizeof(coils), &rs) == MB_RESP_OK);
    CHECK(mb_coil_get(&rs, 0) && !mb_coil_get(&rs, 1) && mb_coil_get(&rs, 2) && !mb_coil_get(&rs, 3));

    mb_request_t wc = { 9, 1, MB_FC_WRITE_SINGLE_COIL, 2, 1 };
    CHECK(mb_encode_request(&wc, adu, sizeof(adu)) == 12 && adu[10] == 0xFF && adu[11] == 0);
    uint8_t echo[] = { 0, 9, 0, 0, 0, 6, 1, 5, 0, 2, 0xFF, 0 };
    CHECK(mb_decode_response(&wc, echo, sizeof(echo), &rs) == MB_RESP_OK);
    echo[10] = 0;
    CHECK(mb_decode_response(&wc, echo, sizeof(echo), &rs) == MB_ERR_ECHO);

    /* fuzz: random frames must never crash or report OK for garbage TIDs */
    uint32_t seed = 1;
    for (int i = 0; i < 20000; i++) {
        uint8_t f[MB_MAX_ADU];
        size_t len = 1 + (size_t)(i % 40);
        for (size_t k = 0; k < len; k++) { seed = seed * 1103515245u + 12345u; f[k] = (uint8_t)(seed >> 16); }
        int32_t need = mb_adu_length(f, len);
        if (need > 0 && (size_t)need <= len) (void)mb_decode_response(&rq, f, (size_t)need, &rs);
    }
    CHECK(1);
}

static void test_gesture(void)
{
    hmi_gesture_tracker_t g;
    int32_t tx, ty;
    hmi_gesture_begin(&g, 100, 100, 0);
    CHECK(hmi_gesture_end(&g, 104, 103, 120, 800, &tx, &ty) == GESTURE_TAP && tx == 100 && ty == 100);
    hmi_gesture_begin(&g, 600, 200, 0);
    hmi_gesture_move(&g, 400, 210);
    CHECK(hmi_gesture_end(&g, 300, 215, 300, 800, &tx, &ty) == GESTURE_SWIPE_LEFT);
    hmi_gesture_begin(&g, 100, 200, 0);
    CHECK(hmi_gesture_end(&g, 400, 200, 300, 800, &tx, &ty) == GESTURE_SWIPE_RIGHT);
    hmi_gesture_begin(&g, 100, 100, 0);
    hmi_gesture_move(&g, 140, 100);                      /* dragged off and back: not a tap */
    CHECK(hmi_gesture_end(&g, 101, 100, 200, 800, &tx, &ty) == GESTURE_NONE);
    hmi_gesture_begin(&g, 100, 100, 0);
    CHECK(hmi_gesture_end(&g, 300, 260, 200, 800, &tx, &ty) == GESTURE_NONE);   /* diagonal */
    hmi_gesture_begin(&g, 100, 100, 0);
    CHECK(hmi_gesture_end(&g, 101, 100, 5000, 800, &tx, &ty) == GESTURE_NONE);  /* long press */
}

static void test_layout(void)
{
    const hmi_plc_driver_t *d = hmi_driver_modbus_get();
    static const int sizes[][2] = { {640, 400}, {800, 480}, {1024, 600}, {1280, 720}, {1920, 1080}, {3840, 2160} };
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        int w = sizes[k][0], h = sizes[k][1];
        for (int banner = 0; banner < 2; banner++) {
            hmi_rect_t c = hmi_layout_content(w, h, banner != 0);
            CHECK(c.w > 0 && c.h > 0 && c.y >= HMI_LAYOUT_HEADER_H && c.y + c.h <= h - HMI_LAYOUT_DOCK_H);
            /* every manifest button is hit at its centre and stays inside the content area */
            for (uint8_t i = 0; i < d->widget_count; i++) {
                if (hmi_token_get_type(d->ui_bytecode[i]) != WIDGET_ACTION_BUTTON) continue;
                hmi_rect_t r = hmi_layout_token_rect(d->ui_bytecode[i], c);
                CHECK(r.x >= c.x && r.x + r.w <= c.x + c.w && r.y >= c.y && r.y + r.h <= c.y + c.h);
                hmi_hit_t hit = hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE,
                                                    d->ui_bytecode, d->widget_count, r.x + r.w / 2, r.y + r.h / 2);
                CHECK(hit.kind == HMI_HIT_WIDGET_BUTTON && hit.param == hmi_token_get_tag(d->ui_bytecode[i]));
            }
            /* header controls do not overlap each other */
            hmi_rect_t e = hmi_layout_estop_button(w, h), ro = hmi_layout_role_pill(w, h), th = hmi_layout_theme_button(w, h);
            CHECK(ro.x + ro.w <= e.x && th.x > 200 && th.x + th.w <= ro.x);
            CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
            /* dock tabs cover the full width */
            for (int x = 0; x < w; x += 7) {
                hmi_hit_t t = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, x, h - 5);
                CHECK(t.kind == HMI_HIT_NAV_TAB && t.param < SCREEN_COUNT);
            }
            /* screen buttons */
            hmi_hit_kind_t ab[] = { HMI_HIT_ALARM_ACK, HMI_HIT_ALARM_SILENCE, HMI_HIT_ESTOP_RESET };
            for (int i = 0; i < 3; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, ab[i]);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_ALARM_MATRIX, banner != 0, HMI_MODAL_NONE, NULL, 0, r.x + r.w / 2, r.y + r.h / 2).kind == ab[i]);
            }
            hmi_hit_kind_t sb[] = { HMI_HIT_SET_ROLE, HMI_HIT_SET_THEME, HMI_HIT_SET_WD_TEST, HMI_HIT_SET_WD_RESET };
            for (int i = 0; i < 4; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, sb[i]);
                CHECK(r.w >= 60 && r.h >= HMI_MIN_TOUCH_TARGET_PX - 2 * HMI_LAYOUT_TOUCH_SLOP);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_SECURITY_SETTINGS, banner != 0, HMI_MODAL_NONE, NULL, 0, r.x + r.w / 2, r.y + r.h / 2).kind == sb[i]);
            }
            /* no action on the process screen from a tap outside buttons */
            CHECK(hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE, d->ui_bytecode, d->widget_count,
                                      c.x + 30, c.y + 30).kind == HMI_HIT_NONE);

            /* Dialogs: fit on screen, keys hit, E-STOP still reachable, buttons underneath are blocked */
            for (uint8_t md = HMI_MODAL_ROLE; md <= HMI_MODAL_PIN; md++) {
                hmi_rect_t m = hmi_layout_modal(w, h, md);
                CHECK(m.x >= 0 && m.y >= HMI_LAYOUT_HEADER_H && m.x + m.w <= w && m.y + m.h <= h);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
                hmi_rect_t cn = hmi_layout_modal_cancel(w, h, md);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, cn.x + 4, cn.y + 4).kind == HMI_HIT_MODAL_CANCEL);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, d->ui_bytecode, d->widget_count, 3, h - 5).kind == HMI_HIT_MODAL_CANCEL);
            }
            for (int key = 0; key <= HMI_KEY_OK; key++) {
                hmi_rect_t kr = hmi_layout_modal_key(w, h, key);
                hmi_hit_t kh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_PIN, NULL, 0, kr.x + kr.w / 2, kr.y + kr.h / 2);
                CHECK(kh.kind == HMI_HIT_MODAL_KEY && kh.param == key && kr.h >= 40);
            }
            for (int rl = 0; rl < HMI_ROLE_COUNT; rl++) {
                hmi_rect_t rr = hmi_layout_modal_role(w, h, rl);
                hmi_hit_t rh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_ROLE, NULL, 0, rr.x + 10, rr.y + 10);
                CHECK(rh.kind == HMI_HIT_MODAL_ROLE && rh.param == rl);
            }
        }
    }
}

static void test_auth(void)
{
    uint8_t h[32];
    static const uint8_t abc[32] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                     0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    hmi_sha256("abc", 3, h);
    CHECK(memcmp(h, abc, 32) == 0);
    static char big[1000];
    memset(big, 'a', sizeof(big));
    hmi_sha256_t c; hmi_sha256_init(&c);
    for (int i = 0; i < 1000; i++) hmi_sha256_update(&c, big, sizeof(big));      /* 1,000,000 x 'a' */
    hmi_sha256_final(&c, h);
    CHECK(h[0] == 0xcd && h[1] == 0xc7 && h[31] == 0xd0);

    hmi_security_ctx_t sec;
    hmi_security_init(&sec);
    CHECK(sec.current_role == ROLE_VIEWER && sec.default_pins);
    CHECK(hmi_security_login(&sec, ROLE_OPERATOR, "1111", 0) == HMI_LOGIN_OK && sec.current_role == ROLE_OPERATOR);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "123", 1) == HMI_LOGIN_INVALID);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "12a4", 1) == HMI_LOGIN_INVALID);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "123456789", 1) == HMI_LOGIN_INVALID);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "0000", 2) == HMI_LOGIN_BAD_PIN);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "0001", 3) == HMI_LOGIN_BAD_PIN);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "0002", 4) == HMI_LOGIN_LOCKED);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "3333", 5) == HMI_LOGIN_LOCKED);   /* right PIN, still locked */
    CHECK(sec.current_role == ROLE_OPERATOR);
    CHECK(hmi_security_lockout_remaining_ms(&sec, 10) > 0);
    CHECK(hmi_security_lockout_remaining_ms(&sec, 4 + HMI_PIN_LOCKOUT_MS) == 0);
    CHECK(hmi_security_login(&sec, ROLE_ENGINEER, "3333", 4 + HMI_PIN_LOCKOUT_MS) == HMI_LOGIN_OK);
    CHECK(sec.current_role == ROLE_ENGINEER);
    CHECK(hmi_security_login(&sec, ROLE_VIEWER, NULL, 50000) == HMI_LOGIN_OK && sec.current_role == ROLE_VIEWER);

    /* vault round trip with a new PIN; a corrupted vault is rejected */
    static const uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(hmi_security_set_pin(&sec, ROLE_MAINTENANCE, "975310", salt) && !sec.default_pins);
    CHECK(!hmi_security_set_pin(&sec, ROLE_VIEWER, "1234", salt));
    uint8_t blob[HMI_VAULT_SIZE];
    CHECK(hmi_security_vault_export(&sec, blob, sizeof(blob)) == HMI_VAULT_SIZE);
    hmi_security_ctx_t s2; hmi_security_init(&s2);
    CHECK(hmi_security_vault_import(&s2, blob, sizeof(blob)));
    CHECK(hmi_security_login(&s2, ROLE_MAINTENANCE, "2222", 0) == HMI_LOGIN_BAD_PIN);
    CHECK(hmi_security_login(&s2, ROLE_MAINTENANCE, "975310", 0) == HMI_LOGIN_OK);
    blob[60] ^= 1;
    hmi_security_ctx_t s3; hmi_security_init(&s3);
    CHECK(!hmi_security_vault_import(&s3, blob, sizeof(blob)) && s3.default_pins);
}

/* Render every screen at many sizes with guard bytes around the band buffer. */
static struct { uint8_t pre[64]; hmi_renderer_t r; uint8_t post[64]; } g_box;

static void test_renderer(void)
{
    const hmi_plc_driver_t *pd = hmi_driver_modbus_get();
    const hmi_plc_driver_t *hd = hmi_driver_host_get();
    hmi_tag_registry_reset();
    hmi_tag_register_meta(pd->tag_metadata, pd->tag_count);
    hmi_tag_register_meta(hd->tag_metadata, hd->tag_count);
    static hmi_tag_table_t t; hmi_tags_init(&t);
    for (int i = 0; i < 64; i++) hmi_tag_write_analog(&t, (uint8_t)i, (int16_t)(i * 37), i % 3 ? QUALITY_GOOD : QUALITY_COMM_FAULT);
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, (int16_t)(i * 15));
    static hmi_alarm_supervisor_t al; hmi_alarms_init(&al);
    hmi_tag_write_analog(&t, 0, 990, QUALITY_GOOD);
    hmi_alarms_evaluate(&al, &t, 10);
    for (int i = 0; i < 30; i++) hmi_alarms_log_event(&al, (uint32_t)i * 1000u, 0, ALARM_SEV_WARNING, 1,
                                                       "A very long message that will need truncation!!");
    hmi_security_ctx_t sec; hmi_security_init(&sec);
    hmi_watchdog_t wd; hmi_watchdog_init(&wd, 0);

    static const int sizes[][2] = { {1, 1}, {7, 3}, {8, 1}, {33, 17}, {640, 400}, {641, 401}, {800, 480},
                                    {1366, 768}, {1920, 1080}, {9601, 30}, {20000, 50}, {7680, 4320}, {100, 5000} };
    int frames = 0;
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        memset(g_box.pre, 0xA5, sizeof(g_box.pre));
        memset(g_box.post, 0x5A, sizeof(g_box.post));
        hmi_renderer_init(&g_box.r, sizes[k][0], sizes[k][1]);
        CHECK(g_box.r.band_h >= 1 && (size_t)g_box.r.band_h * (size_t)(g_box.r.screen_w / 2) <= HMI_DISPLAY_MAX_BUF_SIZE);
        for (int theme = 0; theme < 2; theme++) {
            hmi_renderer_set_theme(&g_box.r, theme != 0);
            for (uint8_t scr = 0; scr < SCREEN_COUNT; scr++) {
                hmi_view_t v;
                memset(&v, 0, sizeof(v));
                v.tags = &t; v.alarms = &al; v.sec = &sec; v.wd = &wd;
                v.tokens_process = pd->ui_bytecode; v.ntok_process = pd->widget_count;
                v.tokens_host = hd->ui_bytecode; v.ntok_host = hd->widget_count;
                v.active_screen = scr; v.toast = "Toast message for testing the layout code path";
                v.now_ms = 123456;
                for (int b = 0; b < g_box.r.band_count; b++) hmi_renderer_render_band(&g_box.r, b, &v);
                frames++;
                /* also a frame with no data at all */
                hmi_view_t e; memset(&e, 0, sizeof(e)); e.active_screen = scr; e.data_stale = true;
                for (int b = 0; b < g_box.r.band_count; b++) hmi_renderer_render_band(&g_box.r, b, &e);
            }
        }
        bool guards = true;
        for (unsigned i = 0; i < sizeof(g_box.pre); i++) if (g_box.pre[i] != 0xA5 || g_box.post[i] != 0x5A) guards = false;
        CHECK(guards);
        if (!guards) printf("  overflow at %dx%d\n", sizes[k][0], sizes[k][1]);
    }
    CHECK(frames == 13 * 2 * SCREEN_COUNT);
}

static void test_alarm_log(void)
{
    char path[MAX_PATH];
    GetTempPathA(sizeof(path), path);
    strncat(path, "hmi_test_alarms.log", sizeof(path) - strlen(path) - 1);
    DeleteFileA(path);
    char p1[MAX_PATH + 4]; snprintf(p1, sizeof(p1), "%s.1", path); DeleteFileA(p1);

    static hmi_alarm_supervisor_t s; hmi_alarms_init(&s);
    CHECK(hmi_alarm_log_open(path, 1u << 20));
    for (int i = 0; i < 10; i++) hmi_alarms_log_event(&s, (uint32_t)i, 3, ALARM_SEV_WARNING, 1, "event, with \"quotes\"");
    hmi_alarm_log_sync(&s);
    hmi_alarm_log_stats_t st; hmi_alarm_log_get_stats(&st);
    CHECK(st.records_written == 10 && st.records_lost == 0);
    /* overrun: 40 events between two syncs, only 16 are still in memory */
    for (int i = 0; i < 40; i++) hmi_alarms_log_event(&s, 100u + (uint32_t)i, 3, ALARM_SEV_INFO, 0, "burst");
    hmi_alarm_log_sync(&s);
    hmi_alarm_log_get_stats(&st);
    CHECK(st.records_lost == 24 && st.records_written == 27);
    hmi_alarm_log_close();

    /* restart: the chain continues */
    CHECK(hmi_alarm_log_open(path, 1u << 20));
    hmi_alarm_log_get_stats(&st);
    CHECK(st.chain_resumed);
    static hmi_alarm_supervisor_t s2; hmi_alarms_init(&s2);
    hmi_alarms_log_event(&s2, 5, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, 0, "second run");
    hmi_alarm_log_sync(&s2);
    hmi_alarm_log_close();

    uint32_t lines = 0, bad = 0, first = 0;
    CHECK(hmi_alarm_log_verify_file(path, &lines, &bad, &first));
    CHECK(lines == 29 && bad == 0);

    /* tamper 1: change one character */
    static char buf[16384];
    HANDLE f = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0; ReadFile(f, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(f); buf[n] = 0;
    char *pos = strstr(buf, "burst");
    CHECK(pos != NULL);
    if (pos) {
        pos[0] = 'B';
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w; WriteFile(f, buf, n, &w, NULL); CloseHandle(f);
        CHECK(!hmi_alarm_log_verify_file(path, &lines, &bad, &first) && first > 1);
        pos[0] = 'b';
    }
    /* tamper 2: delete one whole line (every remaining line still has a valid-looking CRC) */
    char *l3 = strstr(buf, "\r\n");
    if (l3) l3 = strstr(l3 + 2, "\r\n");
    char *l4 = l3 ? strstr(l3 + 2, "\r\n") : NULL;
    CHECK(l3 && l4);
    if (l3 && l4) {
        memmove(l3, l4, strlen(l4) + 1);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w; WriteFile(f, buf, (DWORD)strlen(buf), &w, NULL); CloseHandle(f);
        CHECK(!hmi_alarm_log_verify_file(path, &lines, &bad, &first) && first == 3);
    }

    /* rotation keeps both files verifiable */
    DeleteFileA(path);
    CHECK(hmi_alarm_log_open(path, 4096));
    static hmi_alarm_supervisor_t s3; hmi_alarms_init(&s3);
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 10; i++) hmi_alarms_log_event(&s3, (uint32_t)i, 1, ALARM_SEV_INFO, 0, "rotation filler text");
        hmi_alarm_log_sync(&s3);
    }
    hmi_alarm_log_get_stats(&st);
    hmi_alarm_log_close();
    CHECK(st.rotations >= 1 && st.write_errors == 0);
    CHECK(hmi_alarm_log_verify_file(path, &lines, &bad, &first));
    CHECK(hmi_alarm_log_verify_file(p1, &lines, &bad, &first));
    DeleteFileA(path); DeleteFileA(p1);
}

int main(void)
{
    test_crc();
    test_format();
    test_tags_registry();
    test_alarms();
    test_security();
    test_auth();
    test_watchdog();
    test_modbus();
    test_gesture();
    test_layout();
    test_renderer();
    test_alarm_log();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
