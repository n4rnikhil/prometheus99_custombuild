/**
 * @file test_modbus_live.c
 * @brief Drive the Modbus TCP driver against a live server (tests/modbus_plc_sim.py)
 *
 *   python tests/modbus_plc_sim.py --port 1502 --outage-at 4 --outage-for 3 --run-for 14 &
 *   tests\test_modbus_live.exe 127.0.0.1:1502 12
 *
 * Runs the driver at 1 kHz for N seconds, issues writes, and checks that values
 * arrive, writes take effect, an outage is detected and the link recovers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "../include/drivers/driver_modbus.h"
#include "../include/hal/timer.h"

int main(int argc, char **argv)
{
    const char *ep = argc > 1 ? argv[1] : "127.0.0.1:1502";
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    const hmi_plc_driver_t *d = hmi_driver_modbus_get();
    static hmi_tag_table_t tags;

    hmi_timer_init();
    hmi_tag_registry_reset();
    hmi_tag_register_meta(d->tag_metadata, d->tag_count);
    hmi_tags_init(&tags);
    d->init();
    if (!d->connect(ep)) { printf("connect() rejected endpoint\n"); return 2; }

    hmi_driver_stats_t st;
    uint32_t start = hmi_timer_get_ms();
    bool saw_up = false, saw_down_after_up = false, recovered = false, valve_written = false, valve_seen = false;
    int last_print = -1;
    while ((uint32_t)(hmi_timer_get_ms() - start) < (uint32_t)seconds * 1000u) {
        uint32_t now = hmi_timer_get_ms();
        d->poll_cyclic(&tags, now);
        d->get_stats(&st);
        bool up = st.link_state == HMI_LINK_UP && tags.tags[SCHNEIDER_TAG_TANK_LEVEL].quality == QUALITY_GOOD;
        if (up && !saw_up) saw_up = true;
        if (saw_up && tags.tags[SCHNEIDER_TAG_COMM_LOSS].value) saw_down_after_up = true;
        if (saw_down_after_up && up) recovered = true;
        if (up && !valve_written && (uint32_t)(now - start) > 1500u) {
            valve_written = d->write_register(MB_HR_VALVE_POS, 33);
        }
        if (valve_written && tags.tags[SCHNEIDER_TAG_VALVE_POS].value == 33 && tags.tags[SCHNEIDER_TAG_VALVE_POS].quality == QUALITY_GOOD) valve_seen = true;
        int sec = (int)((now - start) / 1000u);
        if (sec != last_print) {
            last_print = sec;
            printf("t=%2ds link=%d q=%d level=%d valve=%d req=%lu ok=%lu to=%lu ex=%lu perr=%lu conn=%lu rtt=%luus\n",
                   sec, st.link_state, tags.tags[0].quality, tags.tags[0].value, tags.tags[2].value,
                   (unsigned long)st.requests_sent, (unsigned long)st.responses_ok, (unsigned long)st.timeouts,
                   (unsigned long)st.exceptions, (unsigned long)st.protocol_errors, (unsigned long)st.reconnects,
                   (unsigned long)st.last_rtt_us);
            fflush(stdout);
        }
        Sleep(1);
    }
    d->disconnect();
    printf("saw_up=%d write_accepted=%d write_seen=%d outage_detected=%d recovered=%d\n",
           saw_up, valve_written, valve_seen, saw_down_after_up, recovered);
    return (saw_up && valve_seen) ? 0 : 1;
}
