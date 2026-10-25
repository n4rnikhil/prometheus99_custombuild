/**
 * @file test_modbus_live.c
 * @brief Headless plug-and-play test: real discovery thread + device manager + Modbus TCP
 *
 *   start "" python tests/modbus_plc_sim.py --profile tank  --port 1502 --outage-at 6 --outage-for 3 --run-for 30
 *   start "" python tests/modbus_plc_sim.py --profile meter --port 1504 --run-for 30
 *   tests\test_modbus_live.exe 20
 *
 * Checks: the tank PLC is discovered and attached with the tank-skid driver, the
 * power meter is NOT attached (not a PLC), a valve command reaches the PLC, the
 * outage takes the device offline and it recovers when the PLC comes back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../include/drivers/device_manager.h"
#include "../include/drivers/discovery.h"
#include "../include/hal/timer.h"
#include "../include/hal/atomic.h"

static hal_atomic_t g_run = 1;
static hmi_device_candidate_t g_q[8];
static hal_atomic_t g_head, g_tail;
static hmi_device_info_t g_pub[HMI_MAX_DEVICES];
static hal_atomic_t g_pub_lock;

static bool attached(const char *host, uint16_t port, uint8_t unit)
{
    char ep[24];
    snprintf(ep, sizeof(ep), "%s:%u", host, (unsigned)port);
    while (hal_atomic_exchange(&g_pub_lock, 1)) Sleep(0);
    bool hit = false;
    for (int i = 0; i < HMI_MAX_DEVICES; i++)
        if (g_pub[i].state && g_pub[i].unit_id == unit && strcmp(g_pub[i].endpoint, ep) == 0) hit = true;
    hal_atomic_store(&g_pub_lock, 0);
    return hit;
}

static void found(const hmi_device_candidate_t *c)
{
    if ((uint32_t)(g_head - g_tail) >= 8) return;
    g_q[(uint32_t)g_head % 8] = *c;
    hal_atomic_add(&g_head, 1);
}

static DWORD WINAPI disc_thread(LPVOID p)
{
    (void)p;
    hmi_discovery_run(&g_run, attached, found);
    return 0;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    static hmi_tag_table_t tags;
    static hmi_alarm_supervisor_t al;
    hmi_timer_init();
    hmi_tag_registry_reset();
    hmi_tags_init(&tags);
    hmi_alarms_init(&al);
    hmi_devmgr_init(0);                          /* never remove: we want to see the recovery */
    hmi_discovery_add_defaults();
    HANDLE th = CreateThread(NULL, 0, disc_thread, NULL, 0, NULL);

    bool tank_up = false, meter_attached = false, wrote = false, valve_seen = false, offline = false, recovered = false;
    int tank_slot = -1;
    uint32_t start = hmi_timer_get_ms(), last_print = 0;
    while ((uint32_t)(hmi_timer_get_ms() - start) < (uint32_t)seconds * 1000u) {
        uint32_t now = hmi_timer_get_ms();
        while (g_tail != g_head) { hmi_devmgr_attach(&g_q[(uint32_t)g_tail % 8], &tags, &al, now); hal_atomic_add(&g_tail, 1); }
        hmi_devmgr_poll(&tags, &al, now);
        hmi_alarms_evaluate(&al, &tags, now);
        while (hal_atomic_exchange(&g_pub_lock, 1)) Sleep(0);
        hmi_devmgr_publish(g_pub, now);
        hal_atomic_store(&g_pub_lock, 0);

        for (int i = 0; i < HMI_MAX_DEVICES; i++) {
            const hmi_device_info_t *d = &g_pub[i];
            if (!d->state) continue;
            if (strstr(d->endpoint, ":1504")) meter_attached = true;
            if (d->profile == &g_profile_tank_skid && strstr(d->endpoint, ":1502")) {
                tank_slot = i;
                if (d->state == HMI_DEV_ONLINE) { if (offline) recovered = true; tank_up = true; }
                if (d->state == HMI_DEV_OFFLINE && tank_up) offline = true;
            }
        }
        if (tank_up && !wrote && tank_slot >= 0 && (uint32_t)(now - start) > 3000u) {
            hmi_devmgr_action((uint8_t)tank_slot, 4, 25, &tags, &al, now);   /* Set valve: 65 -> 25 % */
            wrote = true;
        }
        if (wrote && tank_slot >= 0 && tags.tags[HMI_DEV_TAG(tank_slot, 2)].value == 25 &&
            tags.tags[HMI_DEV_TAG(tank_slot, 2)].quality == QUALITY_GOOD) valve_seen = true;

        if ((uint32_t)(now - last_print) >= 1000u) {
            last_print = now;
            hmi_discovery_stats_t ds;
            hmi_discovery_get_stats(&ds);
            printf("t=%2lus", (unsigned long)((now - start) / 1000u));
            for (int i = 0; i < HMI_MAX_DEVICES; i++) {
                if (g_pub[i].state) printf("  [%s %s st%u ok%lu]", g_pub[i].label, g_pub[i].endpoint,
                                           (unsigned)g_pub[i].state, (unsigned long)g_pub[i].responses);
            }
            printf("  disc: sweeps %lu found %lu ignored %lu\n", (unsigned long)ds.sweeps, (unsigned long)ds.found, (unsigned long)ds.ignored);
            fflush(stdout);
        }
        Sleep(1);
    }
    g_run = 0;
    WaitForSingleObject(th, 5000);
    hmi_devmgr_shutdown();
    printf("tank_attached=%d meter_ignored=%d valve_write_seen=%d outage_detected=%d recovered=%d\n",
           tank_up, !meter_attached, valve_seen, offline, recovered);
    for (uint8_t i = 0; i < hmi_alarms_journal_count(&al); i++) {
        const hmi_alarm_record_t *r = hmi_alarms_journal_newest(&al, i);
        printf("  journal: %s\n", r->message);
    }
    return (tank_up && !meter_attached && valve_seen && offline && recovered) ? 0 : 1;
}
