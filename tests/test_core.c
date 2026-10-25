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
#include "../include/hal/atomic.h"
#include "../include/hal/fs.h"
#include "../include/hal/sys.h"
#include "../include/hal/net.h"
#include "../include/drivers/plc_sim.h"
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
    CHECK(HMI_MAX_TAGS == HMI_DEVICE_TAG_BASE + HMI_MAX_DEVICES * HMI_DEVICE_TAG_SPAN);
    CHECK(HMI_DEV_TAG(HMI_MAX_DEVICES - 1, HMI_DEVICE_TAG_SPAN - 1) == HMI_MAX_TAGS - 1);
#if HMI_MAX_DEVICES == 4
    CHECK(HMI_MAX_TAGS == 128 && HMI_DEV_TAG(3, 23) == 127);
#endif
    CHECK(hmi_tag_register_meta(T_META, 2));
    hmi_tag_bind_meta(HMI_DEV_TAG(HMI_MAX_DEVICES - 1, 0), &T_META[0]);
    CHECK(hmi_tag_get_meta(HMI_DEV_TAG(HMI_MAX_DEVICES - 1, 0)) == &T_META[0]);
    hmi_tag_bind_meta(HMI_DEV_TAG(HMI_MAX_DEVICES - 1, 0), NULL);
    CHECK(hmi_tag_get_meta(HMI_DEV_TAG(HMI_MAX_DEVICES - 1, 0)) == NULL);
    CHECK(!hmi_tag_register_meta(T_META, 1));           /* duplicate id rejected */
    CHECK(hmi_tag_get_meta(20) == &T_META[0]);
    CHECK(hmi_tag_get_meta(63) == NULL);
    CHECK(hmi_tag_get_meta(200) == NULL);

    hmi_tag_table_t t;
    hmi_tags_init(&t);
    CHECK(t.tags[20].quality == QUALITY_COMM_FAULT);
    const uint8_t ts = HMI_MAX_DEVICES - 1;              /* last slot */
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, ts, (int16_t)i);
    int16_t out[HMI_TREND_SAMPLES];
    uint8_t n = hmi_tags_copy_trend(&t, ts, out, HMI_TREND_SAMPLES);
    CHECK(n == 60 && out[0] == 10 && out[59] == 69);     /* chronological after wrap */
#if HMI_MAX_DEVICES > 1
    CHECK(hmi_tags_copy_trend(&t, 0, out, HMI_TREND_SAMPLES) == 0);   /* other slots untouched */
#endif
    CHECK(hmi_tags_copy_trend(&t, HMI_MAX_DEVICES, out, HMI_TREND_SAMPLES) == 0);  /* out of range */
    hmi_tags_push_trend(&t, HMI_MAX_DEVICES, 5);                      /* ignored, no overflow */
    hmi_tags_clear_trend(&t, ts);
    CHECK(hmi_tags_copy_trend(&t, ts, out, HMI_TREND_SAMPLES) == 0);
    for (int i = 0; i < 70; i++) hmi_tags_push_trend(&t, ts, (int16_t)i);
    n = hmi_tags_copy_trend(&t, ts, out, 5);
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
                                                            NULL, 0, devs, (uint8_t)n, 0xFF, r.x + r.w / 2, r.y + r.h / 2);
                        CHECK(hit.kind == HMI_HIT_WIDGET_BUTTON && hit.param == HMI_DEV_TAG(devs[a].slot, hmi_token_get_tag(tk[i])));
                        CHECK(hmi_profile_action(devs[a].profile, hmi_token_get_tag(tk[i])) != NULL);
                    }
                }
                CHECK(hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE, NULL, 0,
                                          devs, (uint8_t)n, 0xFF, c.x + 30, c.y + 46).kind == HMI_HIT_NONE);
            }
            /* header controls do not overlap each other */
            hmi_rect_t e = hmi_layout_estop_button(w, h), ro = hmi_layout_role_pill(w, h), th = hmi_layout_theme_button(w, h);
            CHECK(ro.x + ro.w <= e.x && th.x > 200 && th.x + th.w <= ro.x);
            CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, 0xFF, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
            for (int x = 0; x < w; x += 7) {
                hmi_hit_t t = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, 0xFF, x, h - 5);
                CHECK(t.kind == HMI_HIT_NAV_TAB && t.param < SCREEN_COUNT);
            }
            hmi_hit_kind_t ab[] = { HMI_HIT_ALARM_ACK, HMI_HIT_ALARM_SILENCE, HMI_HIT_ESTOP_RESET };
            for (int i = 0; i < 3; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, ab[i]);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_ALARM_MATRIX, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, 0xFF, r.x + r.w / 2, r.y + r.h / 2).kind == ab[i]);
            }
            hmi_hit_kind_t sb[] = { HMI_HIT_SET_ROLE, HMI_HIT_SET_THEME, HMI_HIT_SET_WD_TEST, HMI_HIT_SET_WD_RESET };
            for (int i = 0; i < 4; i++) {
                hmi_rect_t r = hmi_layout_screen_button(w, h, banner != 0, sb[i]);
                CHECK(r.w >= 60 && r.h >= HMI_MIN_TOUCH_TARGET_PX - 2 * HMI_LAYOUT_TOUCH_SLOP);
                CHECK(hmi_layout_hit_test(w, h, SCREEN_SECURITY_SETTINGS, banner != 0, HMI_MODAL_NONE, NULL, 0, NULL, 0, 0xFF, r.x + r.w / 2, r.y + r.h / 2).kind == sb[i]);
            }
            /* Dialogs */
            make_devices(devs, 2);
            for (uint8_t md = HMI_MODAL_ROLE; md <= HMI_MODAL_PIN; md++) {
                hmi_rect_t m = hmi_layout_modal(w, h, md);
                CHECK(m.x >= 0 && m.y >= HMI_LAYOUT_HEADER_H && m.x + m.w <= w && m.y + m.h <= h);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, NULL, 0, 0xFF, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
                hmi_rect_t cn = hmi_layout_modal_cancel(w, h, md);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, NULL, 0, 0xFF, cn.x + 4, cn.y + 4).kind == HMI_HIT_MODAL_CANCEL);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, devs, (uint8_t)(HMI_MAX_DEVICES < 2 ? HMI_MAX_DEVICES : 2), 0xFF, 3, h - 5).kind == HMI_HIT_MODAL_CANCEL);
            }
            for (int key = 0; key <= HMI_KEY_OK; key++) {
                hmi_rect_t kr = hmi_layout_modal_key(w, h, key);
                hmi_hit_t kh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_PIN, NULL, 0, NULL, 0, 0xFF, kr.x + kr.w / 2, kr.y + kr.h / 2);
                CHECK(kh.kind == HMI_HIT_MODAL_KEY && kh.param == key && kr.h >= 40);
            }
            for (int rl = 0; rl < HMI_ROLE_COUNT; rl++) {
                hmi_rect_t rr = hmi_layout_modal_role(w, h, rl);
                hmi_hit_t rh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_ROLE, NULL, 0, NULL, 0, 0xFF, rr.x + 10, rr.y + 10);
                CHECK(rh.kind == HMI_HIT_MODAL_ROLE && rh.param == rl);
            }
            /* device panel header opens the faceplate of that device */
            make_devices(devs, 1);
            hmi_rect_t ph = hmi_layout_device_open(hmi_layout_device_panel(c, 1, 0));
            hmi_hit_t oh = hmi_layout_hit_test(w, h, SCREEN_PROCESS_OVERVIEW, banner != 0, HMI_MODAL_NONE, NULL, 0, devs, 1, 0xFF,
                                               ph.x + ph.w / 2, ph.y + ph.h / 2);
            CHECK(oh.kind == HMI_HIT_DEVICE_OPEN && oh.param == devs[0].slot);
            /* new dialogs fit the screen and keep the E-STOP reachable */
            for (uint8_t md = HMI_MODAL_FACEPLATE; md <= HMI_MODAL_CONFIRM; md++) {
                hmi_rect_t m = hmi_layout_modal(w, h, md);
                CHECK(m.w > 0 && m.x >= 0 && m.y >= HMI_LAYOUT_HEADER_H && m.x + m.w <= w && m.y + m.h <= h);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, devs, 1, devs[0].slot, e.x + 5, e.y + 5).kind == HMI_HIT_ESTOP);
                CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, md, NULL, 0, devs, 1, devs[0].slot, 2, h - 2).kind == HMI_HIT_MODAL_CANCEL);
            }
            /* faceplate of every driver module: buttons inside their row, no overlap, every one hit-tests */
            for (uint8_t pi = 0; pi < hmi_profile_count(); pi++) {
                devs[0].profile = hmi_profile_at(pi);
                hmi_rect_t m = hmi_layout_modal(w, h, HMI_MODAL_FACEPLATE);
                hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
                uint8_t nc = hmi_profile_fp_controls(devs[0].profile, cs, HMI_FP_MAX_CTRLS);
                for (uint8_t q = 0; q < nc; q++) {
                    hmi_rect_t br = hmi_layout_fp_ctrl(w, h, devs[0].profile, q);
                    hmi_rect_t rr = hmi_layout_fp_row(w, h, cs[q].row);
                    CHECK(br.w >= 40 && br.h >= 20 && br.x >= rr.x && br.x + br.w <= rr.x + rr.w && br.y >= rr.y && br.y + br.h <= rr.y + rr.h);
                    CHECK(rr.y + rr.h <= m.y + m.h);
                    if (q + 1 < nc && cs[q + 1].row == cs[q].row) {
                        hmi_rect_t nb = hmi_layout_fp_ctrl(w, h, devs[0].profile, q + 1);
                        CHECK(br.x + br.w <= nb.x);
                    }
                    hmi_hit_t fh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_FACEPLATE, NULL, 0, devs, 1, devs[0].slot,
                                                       br.x + br.w / 2, br.y + br.h / 2);
                    CHECK(fh.kind == HMI_HIT_FP_CTRL && fh.param == q);
                }
                CHECK(hmi_layout_fp_ctrl(w, h, devs[0].profile, nc).w == 0);
                hmi_rect_t vc = hmi_layout_fp_values(w, h);
                CHECK(vc.x >= m.x && vc.y + vc.h <= m.y + m.h && vc.x + vc.w < hmi_layout_fp_row(w, h, 0).x);
            }
            make_devices(devs, 1);
            /* numeric keypad: 14 keys inside the dialog, each hit-tests to itself */
            hmi_rect_t nm = hmi_layout_modal(w, h, HMI_MODAL_NUMPAD);
            for (int key = 0; key < HMI_NP_KEYS; key++) {
                hmi_rect_t kr = hmi_layout_numpad_key(w, h, key);
                CHECK(kr.w > 40 && kr.h >= 30 && kr.x >= nm.x && kr.x + kr.w <= nm.x + nm.w && kr.y + kr.h <= nm.y + nm.h);
                hmi_hit_t kh = hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_NUMPAD, NULL, 0, NULL, 0, 0xFF, kr.x + kr.w / 2, kr.y + kr.h / 2);
                CHECK(kh.kind == HMI_HIT_NUMPAD_KEY && kh.param == key);
            }
            CHECK(hmi_layout_numpad_key(w, h, HMI_NP_KEYS).w == 0);
            /* confirmation: Confirm and Cancel */
            hmi_rect_t ok = hmi_layout_confirm_button(w, h, true), no = hmi_layout_confirm_button(w, h, false);
            CHECK(no.x + no.w <= ok.x && ok.h >= 40);
            CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_CONFIRM, NULL, 0, NULL, 0, 0xFF, ok.x + 5, ok.y + 5).kind == HMI_HIT_CONFIRM_OK);
            CHECK(hmi_layout_hit_test(w, h, 0, banner != 0, HMI_MODAL_CONFIRM, NULL, 0, NULL, 0, 0xFF, no.x + 5, no.y + 5).kind == HMI_HIT_MODAL_CANCEL);
            CHECK(hmi_layout_modal_cancel(w, h, HMI_MODAL_CONFIRM).w == 0);
        }
    }
}

/* A driver module and its PLC Lab device must agree on every address, range and option. */
static void check_module_vs_sim(const hmi_device_profile_t *p, sim_type_t st)
{
    const sim_type_info_t *ti = sim_type_info(st);
    CHECK(p->regs.start == 0 && p->regs.count <= SIM_HR && p->coils.start == 0 && p->coils.count <= SIM_COILS);
    for (uint8_t a = 0; a < p->n_actions; a++) {
        const hmi_device_action_t *ac = &p->actions[a];
        bool coil = ac->kind == HMI_ACT_COIL_ON || ac->kind == HMI_ACT_COIL_OFF || ac->kind == HMI_ACT_PULSE_COIL ||
                    ac->kind == HMI_ACT_TOGGLE_COIL;
        const sim_row_t *row = NULL;
        for (uint8_t r = 0; r < ti->nrows; r++)
            if (((ti->rows[r].flags & SIM_COIL) != 0) == coil && ti->rows[r].addr == ac->address) row = &ti->rows[r];
        CHECK(row != NULL);
        if (!row) { printf("  %s action %u: no device row at %s %u\n", p->key, a, coil ? "coil" : "HR", ac->address); continue; }
        CHECK(row->flags & SIM_HMI_W);                                   /* the device accepts this write */
        if (ac->kind == HMI_ACT_SET_REG) CHECK(ac->min >= row->min && ac->max <= row->max && ac->min < ac->max);
        if (ac->kind == HMI_ACT_SET_ENUM) CHECK(hmi_action_option_count(ac) == row->max + 1);
        if (ac->kind == HMI_ACT_PULSE_COIL) CHECK(row->flags & SIM_PULSE);
        if (ac->kind == HMI_ACT_COIL_ON && ac->block_tag != HMI_REL_NONE) {
            int baddr = -1;                                              /* coil that feeds the interlock tag */
            for (uint8_t i = 0; i < p->coils.count; i++) if (p->coils.rel_tags[i] == ac->block_tag) baddr = i;
            CHECK(baddr >= 0 && (row->block_mask & (1u << baddr)));      /* the PLC enforces the same interlock */
        }
    }
    /* every mapped address exists on the device */
    for (uint8_t i = 0; i < p->regs.count; i++) {
        if (p->regs.rel_tags[i] == HMI_REL_NONE) continue;
        bool found = false;
        for (uint8_t r = 0; r < ti->nrows; r++) if (!(ti->rows[r].flags & SIM_COIL) && ti->rows[r].addr == i) found = true;
        CHECK(found);
    }
    for (uint8_t i = 0; i < p->coils.count; i++) {
        if (p->coils.rel_tags[i] == HMI_REL_NONE) continue;
        bool found = false;
        for (uint8_t r = 0; r < ti->nrows; r++) if ((ti->rows[r].flags & SIM_COIL) && ti->rows[r].addr == i) found = true;
        CHECK(found);
    }
    if (p->estop_coil != 0xFFFFu) {
        bool found = false;
        for (uint8_t r = 0; r < ti->nrows; r++)
            if ((ti->rows[r].flags & SIM_COIL) && ti->rows[r].addr == p->estop_coil && (ti->rows[r].flags & SIM_HMI_W)) found = true;
        CHECK(found);
    }
}

static void test_profiles(void)
{
    /* Identity -> module; only PLC / drive models auto-attach */
    CHECK(hmi_profile_match("Schneider Electric", "BMX P34 2020") == &g_profile_tank_skid);
    CHECK(hmi_profile_match("SCHNEIDER ELECTRIC", "BME P58 2040") == &g_profile_boiler);
    CHECK(hmi_profile_match("Schneider Electric", "ATV320U15N4C") == &g_profile_drive);
    CHECK(hmi_profile_match("Schneider Electric", "TM241CEC24T") == &g_profile_conveyor);
    CHECK(hmi_profile_match("Schneider Electric", "TM251MESE") == &g_profile_ahu);
    CHECK(hmi_profile_match("Siemens", "6ES7 214-1AG40-0XB0") == &g_profile_pump_station);
    CHECK(hmi_profile_match("Rockwell Automation", "2080-LC50-24QWB") == &g_profile_compressor);
    CHECK(hmi_profile_match("ABB", "ACS580-01-12A7-4") == &g_profile_abb_drive);
    CHECK(hmi_profile_match("Schneider Electric", "PM5560") == &g_profile_generic);     /* power meter: not a PLC */
    CHECK(hmi_profile_match("Acme", "BMX clone") == &g_profile_generic);
    CHECK(hmi_profile_match(NULL, NULL) == &g_profile_generic);
    CHECK(hmi_profile_count() == 9 && !g_profile_generic.auto_attach);

    for (uint8_t i = 0; i < hmi_profile_count(); i++) {
        const hmi_device_profile_t *p = hmi_profile_at(i);
        CHECK(p->meta_count <= HMI_DEVICE_TAG_SPAN - 1 && p->comm_tag < HMI_DEVICE_TAG_SPAN);
        CHECK(p == &g_profile_generic || p->auto_attach);
        bool comm_meta = false, has[HMI_DEVICE_TAG_SPAN] = { false };
        for (uint8_t m = 0; m < p->meta_count; m++) {
            CHECK(p->meta[m].tag_id < HMI_DEVICE_TAG_SPAN - 1);
            has[p->meta[m].tag_id] = true;
            if (p->meta[m].tag_id == p->comm_tag) comm_meta = p->meta[m].digital_alarm_sev >= ALARM_SEV_CRITICAL;
        }
        CHECK(comm_meta);
        CHECK(p->n_actions <= HMI_MAX_ACTIONS && p->ui_full && p->ui_compact && p->n_full && p->n_compact);
        int ctrls = 0;
        for (uint8_t a = 0; a < p->n_actions; a++) {
            const hmi_device_action_t *ac = &p->actions[a];
            CHECK(ac->label && ac->label[0] && ac->log_name && ac->kind >= HMI_ACT_TOGGLE_COIL && ac->kind <= HMI_ACT_SET_ENUM);
            CHECK(ac->rel_tag < HMI_DEVICE_TAG_SPAN && has[ac->rel_tag]);             /* acts on a tag the HMI shows */
            CHECK(ac->block_tag == HMI_REL_NONE || (ac->block_tag < HMI_DEVICE_TAG_SPAN && has[ac->block_tag]));
            CHECK(ac->command < HMI_CMD_COUNT && hmi_security_required_role((hmi_command_t)ac->command) >= ROLE_OPERATOR);
            if (ac->kind == HMI_ACT_TOGGLE_COIL || ac->kind == HMI_ACT_TOGGLE_REG) CHECK(ac->label_on != NULL);
            if (ac->kind == HMI_ACT_SET_REG) CHECK(ac->min < ac->max);
            uint8_t opts = hmi_action_option_count(ac);
            if (ac->kind == HMI_ACT_SET_ENUM) CHECK(opts >= 2 && opts <= HMI_MAX_OPTIONS);
            else CHECK(opts == 0);
            ctrls += opts ? opts : 1;
        }
        /* faceplate: every control of the module gets a button */
        uint8_t rels[HMI_FP_MAX_ROWS];
        uint8_t nrows = hmi_profile_fp_rows(p, rels);
        hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
        CHECK(hmi_profile_fp_controls(p, cs, HMI_FP_MAX_CTRLS) == ctrls && ctrls <= HMI_FP_MAX_CTRLS);
        for (uint8_t a = 0; a < p->n_actions; a++) {
            bool in_rows = false;
            for (uint8_t r = 0; r < nrows; r++) if (rels[r] == p->actions[a].rel_tag) in_rows = true;
            CHECK(in_rows);
        }
        /* dashboard buttons always resolve to a control, in both states */
        for (int lay = 0; lay < 2; lay++) {
            const uint32_t *tk = lay ? p->ui_compact : p->ui_full;
            uint8_t nt = lay ? p->n_compact : p->n_full;
            for (uint8_t k = 0; k < nt; k++) {
                uint8_t rel = hmi_token_get_tag(tk[k]);
                CHECK(rel == HMI_REL_DEVICE || (rel < HMI_DEVICE_TAG_SPAN && has[rel]));
                if (hmi_token_get_type(tk[k]) != WIDGET_ACTION_BUTTON) continue;
                CHECK(hmi_profile_quick_action(p, rel, 0) >= 0 && hmi_profile_quick_action(p, rel, 1) >= 0);
            }
        }
    }
    /* every PLC Lab device has a module, and they agree */
    static const struct { sim_type_t t; const hmi_device_profile_t *p; } pairs[] = {
        { SIM_TANK, &g_profile_tank_skid }, { SIM_DRIVE, &g_profile_drive }, { SIM_CONVEYOR, &g_profile_conveyor },
        { SIM_BOILER, &g_profile_boiler }, { SIM_PUMPS, &g_profile_pump_station }, { SIM_COMPRESSOR, &g_profile_compressor },
        { SIM_AHU, &g_profile_ahu }, { SIM_VFD, &g_profile_abb_drive } };
    for (unsigned k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
        const sim_type_info_t *ti = sim_type_info(pairs[k].t);
        CHECK(ti->is_plc && hmi_profile_match(ti->vendor, ti->product) == pairs[k].p);
        check_module_vs_sim(pairs[k].p, pairs[k].t);
    }
    CHECK(!sim_type_info(SIM_METER)->is_plc && !hmi_profile_match(sim_type_info(SIM_METER)->vendor, sim_type_info(SIM_METER)->product)->auto_attach);

    /* quick action follows the state: Stop while running, Start while stopped */
    const hmi_device_profile_t *tk = &g_profile_tank_skid;
    int start = hmi_profile_quick_action(tk, 7, 0), stop = hmi_profile_quick_action(tk, 7, 1);
    CHECK(start >= 0 && stop >= 0 && tk->actions[start].kind == HMI_ACT_COIL_ON && tk->actions[stop].kind == HMI_ACT_COIL_OFF);
    CHECK(hmi_profile_quick_action(tk, 0, 0) == -1);                      /* level has no control */
    const hmi_device_action_t *mode = hmi_profile_enum_action(tk, 6);
    char opt[16];
    CHECK(mode && hmi_action_option_count(mode) == 2);
    hmi_action_option_label(mode, 1, opt, sizeof(opt));
    CHECK(strcmp(opt, "Auto") == 0);
    hmi_action_option_label(mode, 2, opt, sizeof(opt));
    CHECK(opt[0] == '\0');
    hmi_action_option_label(hmi_profile_enum_action(&g_profile_ahu, 5), 3, opt, sizeof(opt));
    CHECK(strcmp(opt, "Auto") == 0);
    CHECK(hmi_profile_enum_action(tk, 7) == NULL);
    hmi_device_action_t tr = { .kind = HMI_ACT_TOGGLE_REG, .threshold = 50, .value_low = 25, .value_high = 85 };
    CHECK(hmi_action_reg_target(&tr, 65) == 25 && hmi_action_reg_target(&tr, 25) == 85);

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
static bool journal_has(const hmi_alarm_supervisor_t *al, const char *text)
{
    for (uint8_t i = 0; i < al->journal_count; i++) if (strstr(al->journal[i].message, text)) return true;
    return false;
}

static void poll_ms(hmi_tag_table_t *t, hmi_alarm_supervisor_t *al, uint32_t from, uint32_t to)
{
    for (uint32_t ms = from; ms < to; ms++) hmi_devmgr_poll(t, al, ms);
}

static void test_device_manager(void)
{
    static hmi_tag_table_t t;
    static hmi_alarm_supervisor_t al;
    hmi_tag_registry_reset();
    hmi_tags_init(&t);
    hmi_alarms_init(&al);
    hmi_devmgr_init(1000);
    CHECK(!hmi_devmgr_attach_simulator(SIM_METER, &t, &al, 100));   /* not a PLC */
    CHECK(hmi_devmgr_attach_simulator(SIM_TANK, &t, &al, 100));
#if HMI_MAX_DEVICES < 2
    CHECK(!hmi_devmgr_attach_simulator(SIM_TANK, &t, &al, 100));   /* the only slot is taken */
    hmi_devmgr_shutdown();
    return;
#else
    CHECK(hmi_devmgr_attach_simulator(SIM_TANK, &t, &al, 100));
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    poll_ms(&t, &al, 100, 600);
    hmi_devmgr_publish(d, 600);
    CHECK(d[0].state == HMI_DEV_ONLINE && d[1].state == HMI_DEV_ONLINE && d[0].profile == &g_profile_tank_skid);
#if HMI_MAX_DEVICES > 2
    CHECK(d[2].state == HMI_DEV_EMPTY);
#endif
    CHECK(strcmp(d[0].label, "Tank skid 1") == 0 && strcmp(d[1].label, "Tank skid 2") == 0);   /* unique labels */
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(1, 0));
    CHECK(m && m->group && strcmp(m->group, "Tank skid 2") == 0 && m->tag_id == HMI_DEV_TAG(1, 0));
    enum { LEVEL = 0, VALVE = 2, LEVEL_SP = 5, MODE = 6, PUMP_A = 7, ESTOP = 9 };
    enum { A_START = 0, A_STOP = 1, A_VALVE = 4, A_LEVEL_SP = 5, A_MODE = 6, A_RESET = 7 };
    CHECK(t.tags[HMI_DEV_TAG(1, LEVEL)].quality == QUALITY_SIMULATED && t.tags[HMI_DEV_TAG(1, PUMP_A)].value == 1);

    /* Stop on slot 1 only */
    hmi_devmgr_action(1, A_STOP, 0, &t, &al, 700);
    poll_ms(&t, &al, 700, 900);
    CHECK(t.tags[HMI_DEV_TAG(1, PUMP_A)].value == 0 && t.tags[HMI_DEV_TAG(0, PUMP_A)].value == 1);
    CHECK(journal_has(&al, "Pump A - Stop pump"));
    /* Start again */
    hmi_devmgr_action(1, A_START, 0, &t, &al, 900);
    poll_ms(&t, &al, 900, 1000);
    CHECK(t.tags[HMI_DEV_TAG(1, PUMP_A)].value == 1);
    /* setpoints: in range is written, out of range is refused */
    hmi_devmgr_action(0, A_VALVE, 30, &t, &al, 1000);
    poll_ms(&t, &al, 1000, 1100);
    CHECK(t.tags[HMI_DEV_TAG(0, VALVE)].value == 30);
    hmi_devmgr_action(0, A_VALVE, 150, &t, &al, 1100);
    poll_ms(&t, &al, 1100, 1200);
    CHECK(t.tags[HMI_DEV_TAG(0, VALVE)].value == 30 && journal_has(&al, "out of range"));
    hmi_devmgr_action(0, A_LEVEL_SP, 800, &t, &al, 1200);
    /* mode selector: option index is written; invalid option ignored */
    hmi_devmgr_action(0, A_MODE, 1, &t, &al, 1200);
    hmi_devmgr_action(0, A_MODE, 5, &t, &al, 1200);
    poll_ms(&t, &al, 1200, 1300);
    CHECK(t.tags[HMI_DEV_TAG(0, MODE)].value == 1 && t.tags[HMI_DEV_TAG(0, LEVEL_SP)].value == 800);
    CHECK(journal_has(&al, "Control mode -> Auto"));
    /* pulse: logged, and the PLC acts on it */
    hmi_devmgr_action(0, A_RESET, 0, &t, &al, 1300);
    poll_ms(&t, &al, 1300, 1900);
    CHECK(journal_has(&al, "Pump A overload - Reset"));
    hmi_devmgr_action(0, A_MODE, 0, &t, &al, 1900);                   /* back to Manual */
    poll_ms(&t, &al, 1900, 2000);

    /* E-stop trips every device that has one, and blocks restart */
    CHECK(hmi_devmgr_estop(true, &t, &al, 2000) == 2);
    poll_ms(&t, &al, 2000, 2200);
    CHECK(t.tags[HMI_DEV_TAG(0, ESTOP)].value == 1 && t.tags[HMI_DEV_TAG(0, PUMP_A)].value == 0);
    hmi_devmgr_action(0, A_START, 0, &t, &al, 2200);                 /* start pump -> blocked by the HMI */
    poll_ms(&t, &al, 2200, 2400);
    CHECK(t.tags[HMI_DEV_TAG(0, PUMP_A)].value == 0 && journal_has(&al, "blocked by Emergency stop"));
    /* reset refused while the E-stop alarm is unacknowledged, accepted after ack */
    hmi_alarms_evaluate(&al, &t, 2400);
    CHECK(hmi_devmgr_estop(false, &t, &al, 2400) == 0);
    hmi_alarms_acknowledge_all(&al, 2401);
    CHECK(hmi_devmgr_estop(false, &t, &al, 2402) == 2);
    poll_ms(&t, &al, 2402, 2600);
    CHECK(t.tags[HMI_DEV_TAG(0, ESTOP)].value == 0);

    /* a device that is not a known PLC is refused unless explicitly configured */
    hmi_device_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "127.0.0.1");
    c.port = 1599; c.unit = 1; c.identified = true;
    snprintf(c.id.vendor, sizeof(c.id.vendor), "Schneider Electric");
    snprintf(c.id.product, sizeof(c.id.product), "PM5560");
    CHECK(!hmi_devmgr_attach(&c, &t, &al, 2600));
    c.explicit_target = true;
#if HMI_MAX_DEVICES >= 3
    CHECK(hmi_devmgr_attach(&c, &t, &al, 2600));
    CHECK(!hmi_devmgr_attach(&c, &t, &al, 2601));                /* duplicate */
    hmi_devmgr_publish(d, 2601);
    CHECK(d[2].state == HMI_DEV_CONNECTING && d[2].profile == &g_profile_generic);
#else
    CHECK(!hmi_devmgr_attach(&c, &t, &al, 2600));                /* every slot is in use */
#endif
    hmi_devmgr_shutdown();

    /* every simulated PLC type attaches with its own module and comes online */
    static const sim_type_t types[] = { SIM_DRIVE, SIM_CONVEYOR, SIM_BOILER, SIM_PUMPS, SIM_COMPRESSOR, SIM_AHU, SIM_VFD };
    for (unsigned k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
        hmi_tag_registry_reset();
        hmi_tags_init(&t);
        hmi_alarms_init(&al);
        hmi_devmgr_init(1000);
        CHECK(hmi_devmgr_attach_simulator(types[k], &t, &al, 100));
        poll_ms(&t, &al, 100, 400);
        hmi_devmgr_publish(d, 400);
        const sim_type_info_t *ti = sim_type_info(types[k]);
        CHECK(d[0].state == HMI_DEV_ONLINE && d[0].profile == hmi_profile_match(ti->vendor, ti->product));
        CHECK(t.tags[HMI_DEV_TAG(0, d[0].profile->comm_tag)].value == 0);
        hmi_devmgr_shutdown();
    }
#endif
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
                    v.modal = (uint8_t)((nd + scr) % 6);
                    v.fp_slot = 0;
                    v.np_title = "Speed setpoint"; v.np_entry = "35.5"; v.np_unit = "Hz";
                    v.np_info = "Now 50.0 Hz   -   allowed 0.0 to 60.0";
                    v.cf_title = "Switch Station mode to Hand?"; v.cf_detail = "Pump station 1  -  127.0.0.1:1505";
                    v.cf_ok = "Switch to Hand"; v.cf_danger = (nd & 1) != 0;
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
    char path[HMI_PATH_MAX] = "hmi_test_alarms.log";      /* in the working directory: portable */
    hal_file_remove(path);
    char p1[HMI_PATH_MAX + 4]; snprintf(p1, sizeof(p1), "%s.1", path); hal_file_remove(p1);

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
    hal_file_t *f = hal_file_open(path, HAL_FILE_READ);
    int n = f ? hal_file_read(f, buf, sizeof(buf) - 1) : 0;
    hal_file_close(f);
    if (n < 0) n = 0;
    buf[n] = 0;
    char *pos = strstr(buf, "burst");
    CHECK(pos != NULL);
    if (pos) {
        pos[0] = 'B';
        f = hal_file_open(path, HAL_FILE_WRITE_NEW);
        hal_file_write(f, buf, (size_t)n); hal_file_close(f);
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
        f = hal_file_open(path, HAL_FILE_WRITE_NEW);
        hal_file_write(f, buf, strlen(buf)); hal_file_close(f);
        CHECK(!hmi_alarm_log_verify_file(path, &lines, &bad, &first) && first == 3);
    }

    /* rotation keeps both files verifiable */
    hal_file_remove(path);
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
    hal_file_remove(path); hal_file_remove(p1);
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

static void test_hal(void)
{
    /* IPv4 text helpers */
    uint32_t ip = 0;
    CHECK(hal_ip_parse("192.168.1.50", &ip) && ip == 0xC0A80132u);
    CHECK(hal_ip_parse("localhost", &ip) && ip == 0x7F000001u);
    CHECK(hal_ip_parse("0.0.0.0", &ip) && ip == 0);
    CHECK(hal_ip_parse("255.255.255.255", &ip) && ip == 0xFFFFFFFFu);
    CHECK(!hal_ip_parse("256.1.1.1", &ip));
    CHECK(!hal_ip_parse("1.2.3", &ip));
    CHECK(!hal_ip_parse("1.2.3.4.5", &ip));
    CHECK(!hal_ip_parse("1.2.3.4x", &ip));
    CHECK(!hal_ip_parse("1..2.3", &ip));
    CHECK(!hal_ip_parse("", &ip));
    CHECK(!hal_ip_parse(NULL, &ip));
    CHECK(!hal_ip_parse("1.2.3.0004", &ip));
    char text[16];
    hal_ip_format(0x0A000005u, text);
    CHECK(strcmp(text, "10.0.0.5") == 0);

    /* file layer: append keeps data, pread sees it, replace is atomic, pool is bounded */
    const char *a = "hmi_test_fs_a.bin", *b = "hmi_test_fs_b.bin";
    hal_file_remove(a); hal_file_remove(b);
    hal_file_t *f = hal_file_open(a, HAL_FILE_APPEND);
    CHECK(f != NULL);
    CHECK(hal_file_write(f, "hello", 5));
    CHECK(hal_file_size(f) == 5);
    char got[8] = {0};
    CHECK(hal_file_pread(f, 1, got, 3) && memcmp(got, "ell", 3) == 0);
    CHECK(hal_file_write(f, "!", 1));                       /* still appends after a pread */
    CHECK(hal_file_sync(f));
    hal_file_close(f);
    f = hal_file_open(a, HAL_FILE_READ);
    CHECK(f != NULL && hal_file_read(f, got, sizeof(got)) == 6 && memcmp(got, "hello!", 6) == 0);
    CHECK(hal_file_read(f, got, sizeof(got)) == 0);         /* end of file */
    hal_file_close(f);
    f = hal_file_open(b, HAL_FILE_WRITE_NEW);
    CHECK(f && hal_file_write(f, "new", 3));
    hal_file_close(f);
    CHECK(hal_file_replace(b, a));
    f = hal_file_open(a, HAL_FILE_READ);
    CHECK(f && hal_file_read(f, got, sizeof(got)) == 3 && memcmp(got, "new", 3) == 0);
    hal_file_close(f);
    CHECK(hal_file_open(b, HAL_FILE_READ) == NULL);         /* tmp is gone after the replace */
    CHECK(hal_file_open("no_such_dir_xyz/none.bin", HAL_FILE_READ) == NULL);
    hal_file_t *pool[HAL_FILE_POOL + 1];
    int opened = 0;
    for (int i = 0; i <= HAL_FILE_POOL; i++) { pool[i] = hal_file_open(a, HAL_FILE_READ); if (pool[i]) opened++; }
    CHECK(opened == HAL_FILE_POOL);
    for (int i = 0; i <= HAL_FILE_POOL; i++) hal_file_close(pool[i]);
    hal_file_close(NULL);
    CHECK(hal_file_remove(a));

    /* system helpers */
    char utc[48];
    hal_utc_now(utc, sizeof(utc));
    CHECK(strlen(utc) == 24 && utc[4] == '-' && utc[10] == 'T' && utc[19] == '.' && utc[23] == 'Z');
    uint8_t r1[16], r2[16];
    CHECK(hal_random(r1, sizeof(r1)) && hal_random(r2, sizeof(r2)));
    CHECK(memcmp(r1, r2, sizeof(r1)) != 0);                 /* 2^-128 chance of a false failure */
    uint8_t z[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    hal_secure_zero(z, sizeof(z));
    CHECK(z[0] == 0 && z[7] == 0);
    hal_host_stats_t hs;
    hal_host_stats_init();
    hal_host_stats_poll(&hs);
    CHECK(!hs.ram_ok || (hs.ram_pct >= 0 && hs.ram_pct <= 100));
}

/* PLC Lab device models: the HMI's own Modbus codec talks to them */
static size_t lab_req(sim_plc_t *p, const mb_request_t *rq, uint8_t *out, size_t cap)
{
    uint8_t adu[16];
    size_t n = mb_encode_request(rq, adu, sizeof(adu));
    return n ? sim_handle(p, adu, n, out, cap) : 0;
}

static mb_status_t lab_write(sim_plc_t *p, bool coil, uint16_t addr, int16_t value)
{
    static mb_response_t rsp;
    uint8_t out[SIM_MAX_ADU];
    mb_request_t rq = { 21, 1, coil ? MB_FC_WRITE_SINGLE_COIL : MB_FC_WRITE_SINGLE_REG, addr, (uint16_t)value };
    size_t n = lab_req(p, &rq, out, sizeof(out));
    return n ? mb_decode_response(&rq, out, n, &rsp) : MB_ERR_LEN;
}

static void steps(sim_plc_t *p, int n) { for (int i = 0; i < n; i++) sim_step(p); }

static int count_events(sim_plc_t *p, const char *needle)
{
    char ev[SIM_EVENT_LEN];
    int hits = 0;
    while (sim_pop_event(p, ev, sizeof(ev))) if (!needle || strstr(ev, needle)) hits++;
    return hits;
}

static void test_plc_lab(void)
{
    static sim_plc_t p;
    static mb_response_t rsp;
    uint8_t out[SIM_MAX_ADU];

    /* --- row tables are consistent --- */
    for (int ty = 0; ty < SIM_TYPE_COUNT; ty++) {
        const sim_type_info_t *ti = sim_type_info((sim_type_t)ty);
        CHECK(ti && ti->nrows > 0 && ti->nrows <= SIM_MAX_ROWS);
        if (!ti) continue;
        CHECK((ti->vendor != NULL) == (ti->product != NULL) && (ty == SIM_ANON || ti->vendor));
        for (uint8_t r = 0; r < ti->nrows; r++) {
            const sim_row_t *row = &ti->rows[r];
            CHECK(row->addr < ((row->flags & SIM_COIL) ? SIM_COILS : SIM_HR) && row->min <= row->max);
            if (row->options) {
                int opts = 1;
                for (const char *o = row->options; *o; o++) if (*o == '|') opts++;
                CHECK(row->min == 0 && row->max == opts - 1);
            }
            if (row->flags & SIM_PULSE) CHECK((row->flags & SIM_COIL) && (row->flags & SIM_HMI_W));
            for (uint8_t r2 = (uint8_t)(r + 1); r2 < ti->nrows; r2++)       /* no address twice */
                CHECK(!(((ti->rows[r2].flags ^ row->flags) & SIM_COIL) == 0 && ti->rows[r2].addr == row->addr));
        }
        /* defaults stay finite and the model never runs away in 5 minutes */
        sim_init(&p, (sim_type_t)ty);
        steps(&p, 3000);
        for (int k = 0; k < SIM_HR; k++) CHECK(p.hr[k] > -32000 && p.hr[k] < 32000);
    }
    CHECK(sim_type_info(SIM_TYPE_COUNT) == NULL);

    /* --- tank skid: Modbus reads, identity, writes, interlocks, auto mode --- */
    sim_init(&p, SIM_TANK);
    mb_request_t rd = { 7, 1, MB_FC_READ_HOLDING, 0, 7 };
    size_t n = lab_req(&p, &rd, out, sizeof(out));
    CHECK(n > 0 && mb_decode_response(&rd, out, n, &rsp) == MB_RESP_OK);
    CHECK(rsp.count == 7 && rsp.regs[0] == 720 && rsp.regs[2] == 65 && rsp.regs[5] == 600 && rsp.regs[6] == 0);
    CHECK(sim_frame_length(out, n) == (int32_t)n && sim_frame_length(out, 6) == 0);
    mb_request_t rc = { 8, 1, MB_FC_READ_COILS, 0, 5 };
    n = lab_req(&p, &rc, out, sizeof(out));
    CHECK(n > 0 && mb_decode_response(&rc, out, n, &rsp) == MB_RESP_OK && mb_coil_get(&rsp, 0) && !mb_coil_get(&rsp, 2));
    uint8_t q[16];
    size_t qn = mb_encode_devid_request(0x4301, 1, q, sizeof(q));
    mb_device_id_t id;
    n = sim_handle(&p, q, qn, out, sizeof(out));
    CHECK(n > 0 && mb_decode_devid(out, n, 0x4301, 1, &id) == MB_RESP_OK);
    CHECK(strcmp(id.vendor, "Schneider Electric") == 0 && strcmp(id.product, "BMX P34 2020") == 0);

    CHECK(lab_write(&p, false, 2, 25) == MB_RESP_OK && p.hr[2] == 25);
    CHECK(lab_write(&p, false, 2, 250) == MB_RESP_EXCEPTION && p.hr[2] == 25);       /* out of range: refused */
    CHECK(lab_write(&p, false, 0, 5) == MB_RESP_EXCEPTION);                          /* level is read-only */
    CHECK(lab_write(&p, true, 4, 1) == MB_RESP_EXCEPTION);                           /* overload is a field input */
    CHECK(count_events(&p, "HMI set Valve V-101 to 25 %") == 1);
    CHECK(lab_write(&p, true, 2, 1) == MB_RESP_OK);                                  /* E-stop */
    steps(&p, 1);
    CHECK(p.coils[0] == 0 && p.coils[1] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);               /* start refused: latched */
    CHECK(count_events(&p, "refused, E-stop is active") == 1);
    CHECK(lab_write(&p, true, 2, 0) == MB_RESP_OK);
    p.coils[4] = 1;                                                                   /* lab: pump A overload */
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);
    CHECK(count_events(&p, "Pump A overload is active") == 1);
    CHECK(lab_write(&p, true, 3, 1) == MB_RESP_OK);                                  /* reset (pulse) */
    steps(&p, 1);
    CHECK(p.coils[4] == 0 && p.coils[3] == 0);                                       /* PLC cleared both */
    CHECK(count_events(&p, "HMI pressed Fault reset") == 1);
    /* Auto: the PLC starts pump A below the setpoint band and stops it above */
    CHECK(lab_write(&p, false, 5, 900) == MB_RESP_OK && lab_write(&p, false, 6, 1) == MB_RESP_OK);
    steps(&p, 2);
    CHECK(p.coils[0] == 1);
    p.hr[0] = 950;
    steps(&p, 1);
    CHECK(p.coils[0] == 0);
    CHECK(lab_write(&p, false, 6, 2) == MB_RESP_EXCEPTION);                          /* only Manual / Auto */
    char txt[32];
    sim_row_text(&p, 6, txt, sizeof(txt));
    CHECK(strcmp(txt, "Auto") == 0);
    sim_row_text(&p, 0, txt, sizeof(txt));
    CHECK(strncmp(txt, "95.", 3) == 0 && strstr(txt, " %") != NULL);
    /* fault modes */
    p.freeze = true;
    CHECK(lab_req(&p, &rd, out, sizeof(out)) == 0);
    p.freeze = false;
    p.errors = true;
    n = lab_req(&p, &rd, out, sizeof(out));
    CHECK(n > 0 && mb_decode_response(&rd, out, n, &rsp) == MB_RESP_EXCEPTION && rsp.exception_code == 4);
    p.errors = false;
    uint8_t junk[12] = { 0, 1, 0x12, 0x34, 0, 6, 1, 3, 0, 0, 0, 1 };
    CHECK(sim_frame_length(junk, sizeof(junk)) == -1 && sim_handle(&p, junk, sizeof(junk), out, sizeof(out)) == 0);
    CHECK(sim_handle(&p, q, 5, out, sizeof(out)) == 0);
    /* lab nudges: stored values move, computed values keep an offset through the process */
    sim_init(&p, SIM_TANK);
    sim_nudge(&p, 0, +1);
    CHECK(p.hr[0] == 770);
    steps(&p, 1);
    int16_t t0 = p.hr[4];
    sim_nudge(&p, 4, +1); sim_nudge(&p, 4, +1);
    steps(&p, 1);
    CHECK(p.hr[4] >= t0 + 9);
    sim_nudge(&p, 1, +1);                                                            /* pump speed: process only */
    steps(&p, 1);
    CHECK(p.hr[1] == 1450);

    /* --- drive: ramp, direction, fault and reset --- */
    sim_init(&p, SIM_DRIVE);
    CHECK(lab_write(&p, false, 2, 300) == MB_RESP_OK);
    steps(&p, 40);
    CHECK(p.hr[0] == 300 && p.hr[1] > 0 && p.hr[6] > 0);
    CHECK(lab_write(&p, false, 5, 1) == MB_RESP_OK);                                 /* reverse: stops first */
    steps(&p, 5);
    CHECK(p.hr[0] < 300);
    steps(&p, 60);
    CHECK(p.hr[0] == 300 && p.st[0] == 1);
    p.coils[1] = 1;                                                                   /* lab: drive trips */
    steps(&p, 10);
    CHECK(p.hr[0] == 0 && p.coils[0] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);               /* start refused while faulted */
    CHECK(lab_write(&p, true, 2, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.coils[1] == 0 && lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 1);

    /* --- conveyor: speed, counting, jam, pull-cord, resets --- */
    sim_init(&p, SIM_CONVEYOR);
    steps(&p, 600);                                                                   /* 60 s at 30 m/min */
    CHECK(p.hr[0] == 300 && p.hr[2] >= 29 && p.hr[2] <= 31 && p.hr[4] == 30);
    p.coils[1] = 1;                                                                   /* jam */
    steps(&p, 20);
    CHECK(p.hr[0] == 0 && p.coils[0] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);
    CHECK(lab_write(&p, true, 3, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.coils[1] == 0 && lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 1);
    CHECK(lab_write(&p, true, 4, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.hr[2] == 0);
    CHECK(lab_write(&p, true, 2, 1) == MB_RESP_OK);                                  /* pull-cord */
    steps(&p, 1);
    CHECK(p.coils[0] == 0);

    /* --- boiler: auto control holds the setpoint; low water trips and latches --- */
    sim_init(&p, SIM_BOILER);
    steps(&p, 3000);
    CHECK(p.hr[0] >= 780 && p.hr[0] <= 820);
    CHECK(lab_write(&p, false, 5, 0) == MB_RESP_OK && lab_write(&p, false, 6, 0) == MB_RESP_OK);   /* manual 0 % */
    steps(&p, 5);
    CHECK(p.hr[2] == 0 && p.coils[1] == 0);
    p.hr[4] = 10;                                                                     /* lab: drum runs dry */
    steps(&p, 1);
    CHECK(p.coils[2] == 1 && p.coils[0] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);
    CHECK(lab_write(&p, true, 4, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.coils[2] == 1);                                                           /* reset refused: still dry */
    p.hr[4] = 50;
    CHECK(lab_write(&p, true, 4, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.coils[2] == 0);

    /* --- pump station: auto pressure, standby takeover, hand and off --- */
    sim_init(&p, SIM_PUMPS);
    steps(&p, 1200);
    CHECK(p.hr[0] >= 360 && p.hr[0] <= 440 && p.coils[0] == 1 && p.coils[1] == 0);
    p.coils[2] = 1;                                                                   /* pump 1 fails */
    steps(&p, 600);
    CHECK(p.coils[0] == 0 && p.coils[1] == 1 && p.hr[0] >= 340 && p.hr[0] <= 460);
    CHECK(lab_write(&p, true, 4, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.coils[2] == 0);
    CHECK(lab_write(&p, false, 5, 1) == MB_RESP_OK && lab_write(&p, false, 6, 40) == MB_RESP_OK);   /* hand 40 % */
    steps(&p, 100);
    CHECK(p.hr[3] == 40);
    CHECK(lab_write(&p, false, 5, 0) == MB_RESP_OK);                                 /* off */
    steps(&p, 50);
    CHECK(p.hr[3] == 0 && p.hr[4] == 0 && p.coils[0] == 0 && p.coils[1] == 0);

    /* --- compressor: load / unload band, high-temperature trip --- */
    sim_init(&p, SIM_COMPRESSOR);
    steps(&p, 4000);
    CHECK(p.hr[0] >= 620 && p.hr[0] <= 780 && p.coils[0] == 1);
    for (int k = 0; k < 10; k++) sim_nudge(&p, 3, +1);                              /* +50 C on the discharge */
    steps(&p, 1);
    CHECK(p.coils[1] == 1 && p.coils[0] == 0 && p.hr[5] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);

    /* --- air handler: supply follows setpoint; fire alarm stops the fan --- */
    sim_init(&p, SIM_AHU);
    CHECK(lab_write(&p, false, 1, 180) == MB_RESP_OK);
    steps(&p, 200);
    CHECK(p.hr[0] >= 175 && p.hr[0] <= 185 && p.hr[3] == 70);
    p.coils[2] = 1;
    steps(&p, 40);
    CHECK(p.coils[0] == 0 && p.hr[3] == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_OK && p.coils[0] == 0);

    /* --- ABB drive: speed reference, fault code --- */
    sim_init(&p, SIM_VFD);
    CHECK(lab_write(&p, false, 1, 900) == MB_RESP_OK);
    steps(&p, 20);
    CHECK(p.hr[0] == 900 && p.hr[3] > 0);
    p.coils[1] = 1;
    steps(&p, 10);
    CHECK(p.hr[0] == 0 && p.hr[5] == 2310);
    CHECK(lab_write(&p, true, 2, 1) == MB_RESP_OK);
    steps(&p, 1);
    CHECK(p.hr[5] == 0);

    /* --- power meter: identifies, refuses writes; unknown device: no FC43 --- */
    sim_init(&p, SIM_METER);
    n = sim_handle(&p, q, qn, out, sizeof(out));
    CHECK(n > 0 && mb_decode_devid(out, n, 0x4301, 1, &id) == MB_RESP_OK && strcmp(id.product, "PM5560") == 0);
    CHECK(lab_write(&p, true, 0, 1) == MB_RESP_EXCEPTION && lab_write(&p, false, 0, 1) == MB_RESP_EXCEPTION);
    sim_init(&p, SIM_ANON);
    n = sim_handle(&p, q, qn, out, sizeof(out));
    CHECK(n > 0 && mb_decode_devid(out, n, 0x4301, 1, &id) == MB_RESP_EXCEPTION);
    mb_request_t r4 = { 15, 1, MB_FC_READ_HOLDING, 0, 4 };
    n = lab_req(&p, &r4, out, sizeof(out));
    CHECK(n > 0 && mb_decode_response(&r4, out, n, &rsp) == MB_RESP_OK && rsp.regs[3] == 44);
    /* event ring keeps the newest when it overflows */
    sim_init(&p, SIM_TANK);
    for (int k = 0; k < SIM_EVENTS + 3; k++) lab_write(&p, false, 2, (int16_t)k);
    CHECK(count_events(&p, NULL) == SIM_EVENTS);
}

static void test_atomics(void)
{
    hal_atomic_t a = 0;
    CHECK(hal_atomic_load(&a) == 0);
    hal_atomic_store(&a, 5);
    CHECK(hal_atomic_load(&a) == 5);
    CHECK(hal_atomic_exchange(&a, 9) == 5 && hal_atomic_load(&a) == 9);
    CHECK(hal_atomic_or(&a, 0x10) == 9 && hal_atomic_load(&a) == 0x19);
    CHECK(hal_atomic_add(&a, 1) == 0x1A);
    CHECK(hal_atomic_add(&a, -0x1A) == 0);
    hal_atomic_store(&a, INT32_MAX);
    CHECK(hal_atomic_load(&a) == INT32_MAX);
}

int main(void)
{
    test_crc();
test_atomics();
test_plc_lab();
test_hal();
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
