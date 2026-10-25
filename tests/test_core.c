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
#include "../include/drivers/device_profile.h"
#include "../include/drivers/device_info.h"
#include "../include/drivers/device_manager.h"
#include "../include/drivers/discovery.h"
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
    { 20, TAG_TYPE_ANALOG,  "LEVEL", "%", 0, 1000, 850, 950, 1, 0, "A1", "A2", NULL },
    { 21, TAG_TYPE_DIGITAL, "ESTOP", "",  0, 1,    0,   0,   0, ALARM_SEV_ESTOP, NULL, NULL, NULL },
};

static void test_tags_registry(void)
{
    hmi_tag_registry_reset();
    CHECK(HMI_MAX_TAGS == 128 && HMI_DEV_TAG(3, 23) == 127);
    CHECK(hmi_tag_register_meta(T_META, 2));
    hmi_tag_bind_meta(HMI_DEV_TAG(1, 0), &T_META[0]);
    CHECK(hmi_tag_get_meta(HMI_DEV_TAG(1, 0)) == &T_META[0]);
    hmi_tag_bind_meta(HMI_DEV_TAG(1, 0), NULL);
    CHECK(hmi_tag_get_meta(HMI_DEV_TAG(1, 0)) == NULL);
    CHECK(!hmi_tag_register_meta(T_META, 1));           /* duplicate id rejected */
    CHECK(hmi_tag_get_meta(20) == &T_META[0]);
    CHECK(hmi_tag_get_meta(63) == NULL);
    CHECK(hmi_tag_get_meta(200) == NULL);

    hmi_tag_table_t t;
    hmi_tags_init(&t);
    CHECK(t.tags[20].quality == QUALITY_COMM_FAULT);
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, 2, (int16_t)i);
    int16_t out[HMI_TREND_SAMPLES];
    uint8_t n = hmi_tags_copy_trend(&t, 2, out, HMI_TREND_SAMPLES);
    CHECK(n == 60 && out[0] == 10 && out[59] == 69);     /* chronological after wrap */
    CHECK(hmi_tags_copy_trend(&t, 1, out, HMI_TREND_SAMPLES) == 0);   /* other slots untouched */
    hmi_tags_clear_trend(&t, 2);
    CHECK(hmi_tags_copy_trend(&t, 2, out, HMI_TREND_SAMPLES) == 0);
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, 2, (int16_t)i);
    n = hmi_tags_copy_trend(&t, 2, out, 5);
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

    /* The response buffers are fixed-size (regs[125], coil_bytes[250]): a request that
     * mb_encode_request would refuse must not be decoded against a well-formed reply. */
    {
        static uint8_t big[MB_MAX_ADU + 8];
        mb_request_t r127 = { 3, 1, MB_FC_READ_HOLDING, 0, 127 };       /* 127 regs = 254 data bytes */
        memset(big, 0, sizeof(big));
        big[1] = 3; big[4] = (uint8_t)((3 + 254) >> 8); big[5] = (uint8_t)((3 + 254) & 0xFF);   /* MBAP length 257 */
        big[6] = 1; big[7] = 3; big[8] = 254;
        CHECK(mb_decode_response(&r127, big, 9 + 254, &rs) == MB_ERR_ARG);
        mb_request_t r126 = { 3, 1, MB_FC_READ_HOLDING, 0, 126 };
        big[4] = 0; big[5] = (uint8_t)(3 + 252); big[8] = 252;
        CHECK(mb_decode_response(&r126, big, 9 + 252, &rs) == MB_ERR_ARG);
        mb_request_t r0 = { 3, 1, MB_FC_READ_HOLDING, 0, 0 };
        big[5] = 3; big[8] = 0;
        CHECK(mb_decode_response(&r0, big, 9, &rs) == MB_ERR_ARG);
        mb_request_t r125 = { 3, 1, MB_FC_READ_HOLDING, 0, 125 };       /* the largest legal read still decodes */
        big[5] = (uint8_t)(3 + 250); big[8] = 250;
        CHECK(mb_decode_response(&r125, big, 9 + 250, &rs) == MB_RESP_OK && rs.count == 125);

        mb_request_t c2001 = { 4, 1, MB_FC_READ_COILS, 0, 2001 };       /* 251 bytes > coil_bytes[250] */
        memset(big, 0, sizeof(big));
        big[1] = 4; big[5] = (uint8_t)(3 + 251); big[6] = 1; big[7] = 1; big[8] = 251;
        CHECK(mb_decode_response(&c2001, big, 9 + 251, &rs) == MB_ERR_ARG);
        mb_request_t c2000 = { 4, 1, MB_FC_READ_COILS, 0, 2000 };
        big[5] = (uint8_t)(3 + 250); big[8] = 250;
        CHECK(mb_decode_response(&c2000, big, 9 + 250, &rs) == MB_RESP_OK && rs.count == 2000);
        CHECK(!mb_coil_get(&rs, 2000));                                  /* past the end: false, no read */
    }

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

static void make_devices(hmi_device_info_t *d, int n)
{
    static const hmi_device_profile_t * const prof[4] = { &g_profile_tank_skid, &g_profile_drive, &g_profile_generic, &g_profile_tank_skid };
    memset(d, 0, sizeof(*d) * (size_t)n);
    for (int i = 0; i < n; i++) {
        d[i].state = (uint8_t)(i == 2 ? HMI_DEV_OFFLINE : HMI_DEV_ONLINE);
        d[i].slot = (uint8_t)i;
        d[i].transport = HMI_TRANSPORT_TCP;
        d[i].profile = prof[i];
        d[i].remove_in_ms = i == 2 ? 4200 : 0;
        snprintf(d[i].label, sizeof(d[i].label), "%s %d", prof[i]->name, i + 1);
        snprintf(d[i].endpoint, sizeof(d[i].endpoint), "127.0.0.1:%d", 1502 + i);
        if (i != 2) snprintf(d[i].product, sizeof(d[i].product), "BMX P34 2020");
    }
}

static void test_layout(void)
{
    static const int sizes[][2] = { {640, 400}, {800, 480}, {1024, 600}, {1280, 720}, {1920, 1080}, {3840, 2160} };
    hmi_device_info_t devs[HMI_MAX_DEVICES];
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        int w = sizes[k][0], h = sizes[k][1];
        for (int banner = 0; banner < 2; banner++) {
            hmi_rect_t c = hmi_layout_content(w, h, banner != 0);
            CHECK(c.w > 0 && c.h > 0 && c.y >= HMI_LAYOUT_HEADER_H && c.y + c.h <= h - HMI_LAYOUT_DOCK_H);

            /* 1..4 devices: panels tile the content area without overlap; every button hits its absolute tag */
            for (int n = 1; n <= HMI_MAX_DEVICES; n++) {
                make_devices(devs, n);
                for (int a = 0; a < n; a++) {
                    hmi_rect_t pa = hmi_layout_device_panel(c, n, a);
                    CHECK(pa.w > 0 && pa.h > 0 && pa.x >= c.x && pa.y >= c.y && pa.x + pa.w <= c.x + c.w && pa.y + pa.h <= c.y + c.h);
                    for (int b2 = a + 1; b2 < n; b2++) {
                        hmi_rect_t pb = hmi_layout_device_panel(c, n, b2);
                        bool overlap = pa.x < pb.x + pb.w && pb.x < pa.x + pa.w && pa.y < pb.y + pb.h && pb.y < pa.y + pa.h;
                        CHECK(!overlap);
                    }
                    hmi_rect_t body = hmi_layout_device_body(pa);
                    uint8_t nt = 0;
                    const uint32_t *tk = hmi_layout_device_manifest(devs[a].profile, body, &nt);
                    CHECK(tk != NULL && nt > 0);
                    for (uint8_t i = 0; i < nt; i++) {
                        if (hmi_token_get_type(tk[i]) != WIDGET_ACTION_BUTTON) continue;
                        hmi_rect_t r = hmi_layout_token_rect(tk[i], body);
                        CHECK(r.x >= body.x && r.x + r.w <= body.x + body.w && r.y >= body.y && r.y + r.h <= body.y + body.h);
                        hmi_hit_t hit = hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE,
                                                            NULL, 0, devs, (uint8_t)n, r.x + r.w / 2, r.y + r.h / 2);
                        CHECK(hit.kind == HMI_HIT_WIDGET_BUTTON && hit.param == HMI_DEV_TAG(devs[a].slot, hmi_token_get_tag(tk[i])));
                        CHECK(hmi_profile_action(devs[a].profile, hmi_token_get_tag(tk[i])) != NULL);
                    }
                }
                CHECK(hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE, NULL, 0,
                                          devs, (uint8_t)n, c.x + 30, c.y + 46).kind == HMI_HIT_NONE);
            }
            /* header controls do not overlap each other */
            hmi_rect_t e = hmi_layout_estop_button(w, h), ro = hmi_layout_role_pill(w, h), th = hmi_layout_theme_button(w, h);
            CHECK(ro.x + ro.w <= e.x && th.x > 200 && th.x + th.w <= ro.x);
            CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
            for (int x = 0; x < w; x += 7) {
                hmi_hit_t t = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, x, h - 5);
                CHECK(t.kind == HMI_HIT_NAV_TAB && t.param < SCREEN_COUNT);
            }
            hmi_hit_kind_t ab[] = { HMI_HIT_ALARM_ACK, HMI_HIT_ALARM_SILENCE, HMI_HIT_ESTOP_RESET };
            for (int i = 0; i < 3; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, ab[i]);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_ALARM_MATRIX, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, r.x + r.w / 2, r.y + r.h / 2).kind == ab[i]);
            }
            hmi_hit_kind_t sb[] = { HMI_HIT_SET_ROLE, HMI_HIT_SET_THEME, HMI_HIT_SET_WD_TEST, HMI_HIT_SET_WD_RESET };
            for (int i = 0; i < 4; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, sb[i]);
                CHECK(r.w >= 60 && r.h >= HMI_MIN_TOUCH_TARGET_PX - 2 * HMI_LAYOUT_TOUCH_SLOP);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_SECURITY_SETTINGS, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, r.x + r.w / 2, r.y + r.h / 2).kind == sb[i]);
            }
            /* Dialogs */
            make_devices(devs, 2);
            for (uint8_t md = HMI_MODAL_ROLE; md <= HMI_MODAL_PIN; md++) {
                hmi_rect_t m = hmi_layout_modal(w, h, md);
                CHECK(m.x >= 0 && m.y >= HMI_LAYOUT_HEADER_H && m.x + m.w <= w && m.y + m.h <= h);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, NULL, 0, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
                hmi_rect_t cn = hmi_layout_modal_cancel(w, h, md);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, NULL, 0, cn.x + 4, cn.y + 4).kind == HMI_HIT_MODAL_CANCEL);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, devs, 2, 3, h - 5).kind == HMI_HIT_MODAL_CANCEL);
            }
            for (int key = 0; key <= HMI_KEY_OK; key++) {
                hmi_rect_t kr = hmi_layout_modal_key(w, h, key);
                hmi_hit_t kh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_PIN, NULL, 0, NULL, 0, kr.x + kr.w / 2, kr.y + kr.h / 2);
                CHECK(kh.kind == HMI_HIT_MODAL_KEY && kh.param == key && kr.h >= 40);
            }
            for (int rl = 0; rl < HMI_ROLE_COUNT; rl++) {
                hmi_rect_t rr = hmi_layout_modal_role(w, h, rl);
                hmi_hit_t rh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_ROLE, NULL, 0, NULL, 0, rr.x + 10, rr.y + 10);
                CHECK(rh.kind == HMI_HIT_MODAL_ROLE && rh.param == rl);
            }
        }
    }
}

static void test_profiles(void)
{
    /* Only PLC / drive models auto-attach; everything else needs an explicit --plc */
    CHECK(hmi_profile_match("Schneider Electric", "BMX P34 2020") == &g_profile_tank_skid);
    CHECK(hmi_profile_match("SCHNEIDER ELECTRIC", "BME P58 4040") == &g_profile_tank_skid);
    CHECK(hmi_profile_match("Schneider Electric", "ATV320U15N4C") == &g_profile_drive);
    CHECK(hmi_profile_match("Schneider Electric", "PM5560") == &g_profile_generic);     /* power meter: not a PLC */
    CHECK(hmi_profile_match("Acme", "BMX clone") == &g_profile_generic);
    CHECK(hmi_profile_match(NULL, NULL) == &g_profile_generic);
    CHECK(g_profile_tank_skid.auto_attach && g_profile_drive.auto_attach && !g_profile_generic.auto_attach);
    for (uint8_t i = 0; i < hmi_profile_count(); i++) {
        const hmi_device_profile_t *p = hmi_profile_at(i);
        CHECK(p->meta_count <= HMI_DEVICE_TAG_SPAN - 1 && p->comm_tag < HMI_DEVICE_TAG_SPAN);
        bool comm_meta = false;
        for (uint8_t m = 0; m < p->meta_count; m++) {
            CHECK(p->meta[m].tag_id < HMI_DEVICE_TAG_SPAN - 1);
            if (p->meta[m].tag_id == p->comm_tag) comm_meta = p->meta[m].digital_alarm_sev >= ALARM_SEV_CRITICAL;
        }
        CHECK(comm_meta);
        for (uint8_t a = 0; a < p->n_actions; a++) CHECK(p->actions[a].label_on && p->actions[a].label_off && p->actions[a].log_name);
        CHECK(p->n_actions <= HMI_MAX_ACTIONS && p->ui_full && p->ui_compact && p->n_full && p->n_compact);
    }
    const hmi_device_action_t *va = hmi_profile_action(&g_profile_tank_skid, 2);
    CHECK(va && hmi_action_reg_target(va, 65) == 25 && hmi_action_reg_target(va, 25) == 85);

    /* FC43 device identification codec */
    uint8_t adu[64];
    CHECK(mb_encode_devid_request(0x4301, 1, adu, sizeof(adu)) == 11 && adu[7] == 0x2B && adu[8] == 0x0E);
    static const uint8_t rsp[] = { 0x43, 0x01, 0, 0, 0, 0x22, 1, 0x2B, 0x0E, 1, 1, 0, 0, 3,
        0, 9, 'S','c','h','n','e','i','d','e','r', 1, 12, 'B','M','X',' ','P','3','4',' ','2','0','2','0', 2, 3, 'V','3','.' };
    mb_device_id_t id;
    CHECK(mb_decode_devid(rsp, sizeof(rsp), 0x4301, 1, &id) == MB_ERR_PROTO);   /* MBAP length wrong on purpose */
    uint8_t ok[sizeof(rsp)];
    memcpy(ok, rsp, sizeof(rsp));
    ok[5] = (uint8_t)(sizeof(rsp) - 6);
    CHECK(mb_decode_devid(ok, sizeof(ok), 0x4301, 1, &id) == MB_RESP_OK);
    CHECK(strcmp(id.vendor, "Schneider") == 0 && strcmp(id.product, "BMX P34 2020") == 0 && strcmp(id.revision, "V3.") == 0);
    ok[5] = (uint8_t)(sizeof(rsp) - 6 - 2);
    CHECK(mb_decode_devid(ok, sizeof(ok) - 2, 0x4301, 1, &id) == MB_ERR_LEN);   /* truncated object */
    static const uint8_t ex[] = { 0x43, 0x01, 0, 0, 0, 3, 1, 0xAB, 1 };
    CHECK(mb_decode_devid(ex, sizeof(ex), 0x4301, 1, &id) == MB_RESP_EXCEPTION);
}

/* Device manager with the built-in simulator transport (no sockets needed) */
static void test_device_manager(void)
{
    static hmi_tag_table_t t;
    static hmi_alarm_supervisor_t al;
    hmi_tag_registry_reset();
    hmi_tags_init(&t);
    hmi_alarms_init(&al);
    hmi_devmgr_init(1000);
    CHECK(hmi_devmgr_attach_simulator(&t, &al, 100));
    CHECK(hmi_devmgr_attach_simulator(&t, &al, 100));
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    for (uint32_t ms = 100; ms < 600; ms++) hmi_devmgr_poll(&t, &al, ms);
    hmi_devmgr_publish(d, 600);
    CHECK(d[0].state == HMI_DEV_ONLINE && d[1].state == HMI_DEV_ONLINE && d[2].state == HMI_DEV_EMPTY);
    CHECK(strcmp(d[0].label, "Tank skid 1") == 0 && strcmp(d[1].label, "Tank skid 2") == 0);   /* unique labels */
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(1, 0));
    CHECK(m && m->group && strcmp(m->group, "Tank skid 2") == 0 && m->tag_id == HMI_DEV_TAG(1, 0));
    CHECK(t.tags[HMI_DEV_TAG(1, 0)].quality == QUALITY_SIMULATED && t.tags[HMI_DEV_TAG(1, 5)].value == 1);

    /* action 0 = pump toggle on slot 1 only */
    hmi_devmgr_action(1, 0, &t, &al, 700);
    for (uint32_t ms = 700; ms < 900; ms++) hmi_devmgr_poll(&t, &al, ms);
    CHECK(t.tags[HMI_DEV_TAG(1, 5)].value == 0 && t.tags[HMI_DEV_TAG(0, 5)].value == 1);
    /* E-stop trips every device that has one, and blocks restart */
    CHECK(hmi_devmgr_estop(true, &t, &al, 900) == 2);
    for (uint32_t ms = 900; ms < 1100; ms++) hmi_devmgr_poll(&t, &al, ms);
    CHECK(t.tags[HMI_DEV_TAG(0, 7)].value == 1 && t.tags[HMI_DEV_TAG(0, 5)].value == 0);
    hmi_devmgr_action(0, 0, &t, &al, 1100);                       /* start pump -> blocked */
    for (uint32_t ms = 1100; ms < 1300; ms++) hmi_devmgr_poll(&t, &al, ms);
    CHECK(t.tags[HMI_DEV_TAG(0, 5)].value == 0);
    /* reset refused while the E-stop alarm is unacknowledged, accepted after ack */
    hmi_alarms_evaluate(&al, &t, 1300);
    CHECK(hmi_devmgr_estop(false, &t, &al, 1300) == 0);
    hmi_alarms_acknowledge_all(&al, 1301);
    CHECK(hmi_devmgr_estop(false, &t, &al, 1302) == 2);
    for (uint32_t ms = 1302; ms < 1500; ms++) hmi_devmgr_poll(&t, &al, ms);
    CHECK(t.tags[HMI_DEV_TAG(0, 7)].value == 0);

    /* a device that is not a known PLC is refused unless explicitly configured */
    hmi_device_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "127.0.0.1");
    c.port = 1599; c.unit = 1; c.identified = true;
    snprintf(c.id.vendor, sizeof(c.id.vendor), "Schneider Electric");
    snprintf(c.id.product, sizeof(c.id.product), "PM5560");
    CHECK(!hmi_devmgr_attach(&c, &t, &al, 1500));
    c.explicit_target = true;
    CHECK(hmi_devmgr_attach(&c, &t, &al, 1500));
    CHECK(!hmi_devmgr_attach(&c, &t, &al, 1501));                /* duplicate */
    hmi_devmgr_publish(d, 1501);
    CHECK(d[2].state == HMI_DEV_CONNECTING && d[2].profile == &g_profile_generic);
    hmi_devmgr_shutdown();
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
    hmi_tag_registry_reset();
    static hmi_tag_table_t t; hmi_tags_init(&t);
    static hmi_tag_meta_t slot_meta[HMI_MAX_DEVICES][HMI_DEVICE_TAG_SPAN];
    hmi_device_info_t devs[HMI_MAX_DEVICES];
    make_devices(devs, HMI_MAX_DEVICES);
    for (int d = 0; d < HMI_MAX_DEVICES; d++) {               /* bind like the device manager does */
        const hmi_device_profile_t *p = devs[d].profile;
        for (uint8_t i = 0; i < p->meta_count; i++) {
            uint8_t rel = p->meta[i].tag_id;
            slot_meta[d][rel] = p->meta[i];
            slot_meta[d][rel].group = devs[d].label;
            hmi_tag_bind_meta(HMI_DEV_TAG(d, rel), &slot_meta[d][rel]);
        }
        for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, (uint8_t)d, (int16_t)(i * 15));
    }
    for (int i = 0; i < HMI_MAX_TAGS; i++) hmi_tag_write_analog(&t, (uint8_t)i, (int16_t)(i * 37), i % 3 ? QUALITY_GOOD : QUALITY_COMM_FAULT);
    static hmi_alarm_supervisor_t al; hmi_alarms_init(&al);
    hmi_tag_write_analog(&t, HMI_DEV_TAG(0, 0), 990, QUALITY_GOOD);
    hmi_alarms_evaluate(&al, &t, 10);
    for (int i = 0; i < 30; i++) hmi_alarms_log_event(&al, (uint32_t)i * 1000u, 0, ALARM_SEV_WARNING, 1,
                                                       "A very long message that will need truncation!!");
    hmi_security_ctx_t sec; hmi_security_init(&sec);
    hmi_watchdog_t wd; hmi_watchdog_init(&wd, 0);
    hmi_discovery_stats_t ds; memset(&ds, 0, sizeof(ds)); ds.sweeps = 3; ds.targets = 9;

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
                for (int nd = 0; nd <= HMI_MAX_DEVICES; nd += (scr == SCREEN_PROCESS_OVERVIEW ? 1 : HMI_MAX_DEVICES)) {
                    hmi_view_t v;
                    memset(&v, 0, sizeof(v));
                    v.tags = &t; v.alarms = &al; v.sec = &sec; v.wd = &wd; v.disc = &ds;
                    v.devices = devs; v.ndev = (uint8_t)nd;
                    v.tokens_host = NULL; v.ntok_host = 0;
                    v.active_screen = scr; v.toast = "Toast message for testing the layout code path";
                    v.now_ms = 123456;
                    v.modal = (uint8_t)(nd % 3);
                    for (int b = 0; b < g_box.r.band_count; b++) hmi_renderer_render_band(&g_box.r, b, &v);
                    frames++;
                }
                hmi_view_t e; memset(&e, 0, sizeof(e)); e.active_screen = scr; e.data_stale = true;
                for (int b = 0; b < g_box.r.band_count; b++) hmi_renderer_render_band(&g_box.r, b, &e);
            }
        }
        bool guards = true;
        for (unsigned i = 0; i < sizeof(g_box.pre); i++) if (g_box.pre[i] != 0xA5 || g_box.post[i] != 0x5A) guards = false;
        CHECK(guards);
        if (!guards) printf("  overflow at %dx%d\n", sizes[k][0], sizes[k][1]);
    }
    CHECK(frames == 13 * 2 * ((HMI_MAX_DEVICES + 1) + (SCREEN_COUNT - 1) * 2));   /* process: 0..4 devices; others: 0 and 4 */
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

static void test_discovery_targets(void)
{
    hmi_discovery_clear();
    CHECK(hmi_discovery_target_count() == 0);

    /* accepted forms and their expansion */
    CHECK(hmi_discovery_add("127.0.0.1", false));              CHECK(hmi_discovery_target_count() == 1);
    CHECK(hmi_discovery_add("127.0.0.1:1502-1509", false));    CHECK(hmi_discovery_target_count() == 9);
    CHECK(hmi_discovery_add("localhost:1502/u3", true));       CHECK(hmi_discovery_target_count() == 10);
    CHECK(hmi_discovery_add("192.168.1.0/24", false));         CHECK(hmi_discovery_target_count() == 10 + 254);

    /* malformed specs are refused */
    CHECK(!hmi_discovery_add("300.1.1.1", false));
    CHECK(!hmi_discovery_add("10.0.0.1:0", false));
    CHECK(!hmi_discovery_add("10.0.0.1:70000", false));
    CHECK(!hmi_discovery_add("10.0.0.1:20-10", false));
    CHECK(!hmi_discovery_add("10.0.0.1/u256", false));
    CHECK(!hmi_discovery_add("10.0.0.1:502junk", false));
    CHECK(!hmi_discovery_add("10.0.0.0/8", false));
    CHECK(!hmi_discovery_add("", false));
    CHECK(!hmi_discovery_add(NULL, false));
    CHECK(hmi_discovery_target_count() == 10 + 254);           /* refusals leave the config untouched */

    /* The sweep budget is shared in configuration order. A range that does not fit must be
     * refused up front: accepting it silently starves every target added after it (an explicit
     * --plc address would never be probed). */
    uint32_t before = hmi_discovery_target_count();
    CHECK(!hmi_discovery_add("10.0.0.0/16", false));
    CHECK(hmi_discovery_target_count() == before);
    CHECK(!hmi_discovery_add("10.0.0.0/22:502-503", false));   /* 1022 hosts x 2 ports */
    CHECK(hmi_discovery_add("10.1.1.1", true));                /* an explicit PLC still fits */
    CHECK(hmi_discovery_target_count() == before + 1);

    /* filling the budget exactly is allowed, one more is not */
    hmi_discovery_clear();
    CHECK(hmi_discovery_add("10.0.0.0/22", false));            /* 1022 targets */
    CHECK(hmi_discovery_target_count() == 1022);
    CHECK(hmi_discovery_add("10.9.9.1:1-2", false));           /* 1024 = HMI_DISCOVERY_MAX_TARGETS */
    CHECK(hmi_discovery_target_count() == HMI_DISCOVERY_MAX_TARGETS);
    CHECK(!hmi_discovery_add("10.9.9.9", false));
    hmi_discovery_clear();
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
    test_profiles();
    test_device_manager();
    test_discovery_targets();
    test_renderer();
    test_alarm_log();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
