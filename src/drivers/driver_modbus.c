/**
 * @file driver_modbus.c
 * @brief Schneider Modicon M340/M580 Fieldbus Driver & High-Fidelity Process Simulator
 */

#include "../../include/drivers/driver_modbus.h"
#include "../../include/renderer/bytecode.h"
#include <string.h>

/* --- Schneider Process Metadata Registry (ROM Flash, 0 Bytes RAM) --- */
static const hmi_tag_meta_t s_schneider_meta[SCHNEIDER_TAG_COUNT] = {
    { SCHNEIDER_TAG_TANK_LEVEL,    TAG_TYPE_ANALOG,  "TANK 01 LEVEL",     "%",   0, 100,  80, 95 },
    { SCHNEIDER_TAG_PUMP_RPM,      TAG_TYPE_ANALOG,  "FEED PUMP A",       "RPM", 0, 3600, 3100, 3500 },
    { SCHNEIDER_TAG_VALVE_POS,     TAG_TYPE_ANALOG,  "VALVE V-101",       "%",   0, 100,  85, 95 },
    { SCHNEIDER_TAG_REACTOR_PRESS, TAG_TYPE_ANALOG,  "LINE PRESSURE",     "kPa", 0, 250,  170, 220 },
    { SCHNEIDER_TAG_COOLANT_TEMP,  TAG_TYPE_ANALOG,  "REACTOR TEMP",      "C",   0, 120,  68, 82 },
    { SCHNEIDER_TAG_PUMP_RUNNING,  TAG_TYPE_DIGITAL, "PUMP A STATUS",     "",    0, 1,    0, 0 },
    { SCHNEIDER_TAG_PUMP_B_RUN,    TAG_TYPE_DIGITAL, "PUMP B COOLANT",    "",    0, 1,    0, 0 },
    { SCHNEIDER_TAG_ESTOP_ACTIVE,  TAG_TYPE_DIGITAL, "SAFETY E-STOP",     "",    0, 1,    0, 0 },
};

/* --- The Plug-and-Play UI Bytecode Manifest (Only 48 Bytes in Flash!) ---
 * 12 widgets laid out across 3 spacious Apple-style cards:
 *   Grid X: 0..15, Grid Y: 0..15, Grid W: 1..16, Grid H: 1..16
 */
static const uint32_t s_schneider_manifest[] = {
    /* --- Card 1: Tank 01 Level (Left Column) --- */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL, WIDGET_CONTAINER_CARD, 0, 2, 5, 11),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL, WIDGET_HERO_DIGIT,      1, 4, 3, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL, WIDGET_ANALOG_BAR,      1, 10, 4, 1),

    /* --- Card 2: Feed Pump A & Motor (Middle Column) --- */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RPM,     WIDGET_CONTAINER_CARD, 5, 2, 5, 11),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RPM,     WIDGET_HERO_DIGIT,      6, 4, 3, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RUNNING, WIDGET_STATUS_PILL,     6, 9, 3, 1),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RUNNING, WIDGET_ACTION_BUTTON,   6, 11, 3, 1),

    /* --- Card 3: Line Pressure & Thermals (Right Column) --- */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_REACTOR_PRESS, WIDGET_CONTAINER_CARD, 10, 2, 5, 11),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_REACTOR_PRESS, WIDGET_HERO_DIGIT,      11, 4, 3, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_REACTOR_PRESS, WIDGET_ANALOG_BAR,      11, 9, 4, 1),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_COOLANT_TEMP,  WIDGET_ANALOG_BAR,      11, 11, 4, 1),
};

#define SCHNEIDER_MANIFEST_COUNT ((uint8_t)(sizeof(s_schneider_manifest) / sizeof(s_schneider_manifest[0])))

/* --- Internal Simulation State --- */
static struct {
    int16_t tank_level_raw;   /* 842 = 84.2% */
    int16_t pump_rpm;         /* 1450 RPM */
    int16_t valve_pos;        /* 65% */
    int16_t pressure_kpa;     /* 101 kPa */
    int16_t temp_c;           /* 48 C */
    bool    pump_a_on;
    bool    pump_b_on;
    bool    estop_pressed;
    uint32_t last_sim_tick_ms;
} s_sim = {
    .tank_level_raw = 842,
    .pump_rpm = 1450,
    .valve_pos = 65,
    .pressure_kpa = 101,
    .temp_c = 48,
    .pump_a_on = true,
    .pump_b_on = false,
    .estop_pressed = false,
    .last_sim_tick_ms = 0
};

static bool modbus_init(void)
{
    s_sim.tank_level_raw = 842;
    s_sim.pump_rpm = 1450;
    s_sim.valve_pos = 65;
    s_sim.pressure_kpa = 101;
    s_sim.temp_c = 48;
    s_sim.pump_a_on = true;
    s_sim.pump_b_on = false;
    s_sim.estop_pressed = false;
    return true;
}

static bool modbus_connect(const char *endpoint)
{
    (void)endpoint;
    return true;
}

static void modbus_disconnect(void)
{
}

static bool modbus_poll_cyclic(hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!tags) return false;

    /* Decimate physics tick to 10 Hz (every 100 ms) inside the 1000 Hz loop */
    if ((now_ms - s_sim.last_sim_tick_ms) >= 100) {
        s_sim.last_sim_tick_ms = now_ms;

        if (s_sim.estop_pressed) {
            s_sim.pump_rpm = 0;
            s_sim.pump_a_on = false;
            s_sim.pressure_kpa = 10;
        } else if (s_sim.pump_a_on) {
            /* Realistic motor RPM jitter (± 15 RPM) */
            int jitter = (int)(now_ms % 31) - 15;
            s_sim.pump_rpm = (int16_t)(1450 + jitter);

            /* Gentle fluid level fluctuation */
            s_sim.tank_level_raw = (int16_t)(842 + (int)(now_ms / 1000 % 20) - 10);
            s_sim.pressure_kpa = (int16_t)(101 + (int)(now_ms / 500 % 10) - 5);
            s_sim.temp_c = (int16_t)(48 + (int)(now_ms / 2000 % 6));
        } else {
            /* Spin down */
            if (s_sim.pump_rpm > 50) s_sim.pump_rpm -= 50;
            else s_sim.pump_rpm = 0;
            s_sim.pressure_kpa = 15;
        }

        /* Publish to Tag Table */
        hmi_tag_write_analog(tags, SCHNEIDER_TAG_TANK_LEVEL, s_sim.tank_level_raw, QUALITY_GOOD);
        hmi_tag_write_analog(tags, SCHNEIDER_TAG_PUMP_RPM, s_sim.pump_rpm, QUALITY_GOOD);
        hmi_tag_write_analog(tags, SCHNEIDER_TAG_VALVE_POS, s_sim.valve_pos, QUALITY_GOOD);
        hmi_tag_write_analog(tags, SCHNEIDER_TAG_REACTOR_PRESS, s_sim.pressure_kpa, QUALITY_GOOD);
        hmi_tag_write_analog(tags, SCHNEIDER_TAG_COOLANT_TEMP, s_sim.temp_c, QUALITY_GOOD);
        hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_RUNNING, s_sim.pump_a_on, QUALITY_GOOD);
        hmi_tag_write_digital(tags, SCHNEIDER_TAG_PUMP_B_RUN, s_sim.pump_b_on, QUALITY_GOOD);
        hmi_tag_write_digital(tags, SCHNEIDER_TAG_ESTOP_ACTIVE, s_sim.estop_pressed, QUALITY_GOOD);
    }

    return true;
}

static bool modbus_write_coil(uint16_t coil_addr, bool state)
{
    if (coil_addr == 1) {
        s_sim.pump_a_on = state;
        return true;
    } else if (coil_addr == 2) {
        s_sim.pump_b_on = state;
        return true;
    } else if (coil_addr == 5) {
        s_sim.estop_pressed = state;
        return true;
    }
    return false;
}

static bool modbus_write_register(uint16_t reg_addr, int16_t value)
{
    if (reg_addr == 40003) {
        s_sim.valve_pos = value;
        return true;
    }
    return false;
}

static const hmi_plc_driver_t s_schneider_driver = {
    .driver_name     = "Schneider Modicon M340/M580",
    .default_port    = 502,
    .init            = modbus_init,
    .connect         = modbus_connect,
    .disconnect      = modbus_disconnect,
    .poll_cyclic     = modbus_poll_cyclic,
    .write_coil      = modbus_write_coil,
    .write_register  = modbus_write_register,
    .ui_bytecode     = s_schneider_manifest,
    .widget_count    = SCHNEIDER_MANIFEST_COUNT,
    .tag_metadata    = s_schneider_meta,
    .tag_count       = SCHNEIDER_TAG_COUNT
};

const hmi_plc_driver_t* hmi_driver_modbus_get(void)
{
    return &s_schneider_driver;
}
