/**
 * @file em6400_meter.c
 * @brief Driver module: PowerLogic EM6400NG / PM2xxx energy meter (Modbus RTU behind a PAS600 / Link150
 *        gateway, or a meter with Ethernet)
 *
 * A product module (read only, no controls). Values are IEEE-754 floats, two registers each,
 * high word first. ADDRESSES ARE NOT VERIFIED: they were written from memory of the common
 * PowerLogic layout (3000 current, 3020 voltage, 3060 power, 3110 frequency, 2700 energy), and
 * the web searches did not return Schneider's EM6400NG register list. Before use, check every
 * address against Schneider's EM6400 register list (search "EM6400 register list" on se.com) and
 * against the meter's own display. Some register lists are 1-based: then subtract 1.
 *
 * Meters are not PLCs, so discovery never attaches one on its own: list it with
 *   --plc <gateway-ip>/u<meter address>@em6400       e.g. --plc 192.168.1.50/u1@em6400
 * The gateway (PAS600 or similar) is assumed to forward each Modbus TCP request to the RS-485
 * meter whose address is the unit id. Confirm that on your gateway's configuration page.
 */

#include "devices/device_profile.h"
#include "core/security.h"
#include "renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { EM_IA = 0, EM_IB, EM_IC, EM_VAB, EM_VBC, EM_VCA, EM_VLL, EM_KW, EM_HZ, EM_KWH, EM_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { EM_IA,   TAG_TYPE_ANALOG,  "Current A",       "A",   0, 10000, 0, 0, 2, 0, NULL, NULL, NULL },
    { EM_IB,   TAG_TYPE_ANALOG,  "Current B",       "A",   0, 10000, 0, 0, 2, 0, NULL, NULL, NULL },
    { EM_IC,   TAG_TYPE_ANALOG,  "Current C",       "A",   0, 10000, 0, 0, 2, 0, NULL, NULL, NULL },
    { EM_VAB,  TAG_TYPE_ANALOG,  "Voltage A-B",     "V",   0, 5000, 4400, 4600, 1, 0,
      "Check the supply transformer tap", "Report sustained over-voltage to maintenance", NULL },
    { EM_VBC,  TAG_TYPE_ANALOG,  "Voltage B-C",     "V",   0, 5000, 4400, 4600, 1, 0,
      "Check the supply transformer tap", "Report sustained over-voltage to maintenance", NULL },
    { EM_VCA,  TAG_TYPE_ANALOG,  "Voltage C-A",     "V",   0, 5000, 4400, 4600, 1, 0,
      "Check the supply transformer tap", "Report sustained over-voltage to maintenance", NULL },
    { EM_VLL,  TAG_TYPE_ANALOG,  "Voltage L-L avg", "V",   0, 5000, 4400, 4600, 1, 0,
      "Check the supply transformer tap", "Report sustained over-voltage to maintenance", NULL },
    { EM_KW,   TAG_TYPE_ANALOG,  "Active power",    "kW",  0, 5000, 0, 0, 2, 0, NULL, NULL, NULL },
    { EM_HZ,   TAG_TYPE_ANALOG,  "Frequency",       "Hz",  4500, 5500, 5100, 5200, 2, 0,
      "Check the supply or the generator governor", "Shed load if the frequency keeps rising", NULL },
    { EM_KWH,  TAG_TYPE_ANALOG,  "Energy",          "kWh", 0, 32000, 0, 0, 0, 0, NULL, NULL, NULL },
    { EM_COMM, TAG_TYPE_DIGITAL, "Meter comm loss", "",    0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "Check the RS-485 wiring and the gateway", "Verify the meter address (unit id) and baud rate", NULL },
};

/* FLOAT32 blocks: one rel tag per register pair */
static const uint8_t s_current[] = { EM_IA, EM_IB, EM_IC };                 /* 3000..3005 */
static const uint8_t s_voltage[] = { EM_VAB, EM_VBC, EM_VCA, EM_VLL };      /* 3020..3027 */
static const uint8_t s_power[]   = { EM_KW };                               /* 3060..3061 */
static const uint8_t s_freq[]    = { EM_HZ };                               /* 3110..3111 */
static const uint8_t s_energy[]  = { EM_KWH };                              /* 2700..2701 */
static const hmi_block_map_t s_more[] = {
    { 3019, 2 * N(s_voltage), s_voltage, HMI_FMT_FLOAT32 },
    { 3059, 2 * N(s_power), s_power, HMI_FMT_FLOAT32 },
    { 3109, 2 * N(s_freq), s_freq, HMI_FMT_FLOAT32 },
    { 2699, 2 * N(s_energy), s_energy, HMI_FMT_FLOAT32 },
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(EM_KW,  WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(EM_KW,  WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(EM_HZ,  WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(EM_KW,  WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(EM_VLL, WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(EM_VLL, WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(EM_VAB, WIDGET_ANALOG_BAR,     6, 7, 5, 2),
    HMI_TOKEN_PACK(EM_VBC, WIDGET_ANALOG_BAR,     6, 9, 5, 2),
    HMI_TOKEN_PACK(EM_VCA, WIDGET_ANALOG_BAR,     6, 11, 5, 2),
    HMI_TOKEN_PACK(EM_KWH, WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(EM_KWH, WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(EM_IA,  WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(EM_IB,  WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(EM_IC,  WIDGET_ANALOG_BAR,     11, 11, 5, 2),
};

static const uint32_t s_compact[] = {
    HMI_TOKEN_PACK(EM_KW,  WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(EM_KW,  WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(EM_VLL, WIDGET_ANALOG_BAR,     10, 4, 6, 3),
    HMI_TOKEN_PACK(EM_HZ,  WIDGET_ANALOG_BAR,     10, 8, 6, 3),    /* bars need a row of air between them */
    HMI_TOKEN_PACK(EM_IA,  WIDGET_ANALOG_BAR,     0, 11, 8, 3),
    HMI_TOKEN_PACK(EM_KWH, WIDGET_ANALOG_BAR,     8, 11, 8, 3),
};

const hmi_device_profile_t g_profile_em6400 = {
    "em6400", "Energy meter", "schneider", "em64|pm21|pm22", true, false,   /* not a PLC: --plc only */
    s_meta, N(s_meta),
    { 2999, 2 * N(s_current), s_current, HMI_FMT_FLOAT32 },
    { 0, 0, NULL, HMI_FMT_INT16 },
    NULL, 0,
    EM_COMM, HMI_REL_NONE, 0xFFFFu, EM_KW,
    s_full, N(s_full),
    s_compact, N(s_compact),
    s_more, N(s_more),
    NULL, 0,
    NULL,                                                /* fixed product map */
};
