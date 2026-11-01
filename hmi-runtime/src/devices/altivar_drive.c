/**
 * @file altivar_drive.c
 * @brief Driver module: Schneider Altivar drives (ATV320 / ATV340 / ATV630 / ATV930) over Modbus TCP
 *
 * A product module that uses the drive's communication parameters (Modbus address = parameter
 * logical address, 0-based). STATUS OF THE ADDRESSES:
 *   verified in public Schneider material:  3201 ETA, 3202 RFR, 8501 CMD, 8502 LFR
 *   NOT verified (check NVE41316 / the ATV programming manual before use on a real drive):
 *     3203 FRH, 3204 LCR (a third-party guide reads 3204 as ETA - conflicting), 7121 LFT, 9001 ACC, 9002 DEC
 *   Bit meanings follow the CiA402 / DS402 standard; "network control" bit 9 is assumed.
 *
 *   3201 ETA  status word (CiA402 / Drivecom)        read
 *   3202 RFR  output frequency, 0.1 Hz (signed)      read
 *   3203 FRH  frequency reference, 0.1 Hz            read
 *   3204 LCR  motor current, 0.1 A                   read
 *   7121 LFT  last fault code                        read
 *   8501 CMD  control word                           read / write
 *   8502 LFR  frequency reference via network, 0.1 Hz  read / write
 *   9001 ACC  acceleration ramp, 0.1 s               read / write (Maintenance)
 *   9002 DEC  deceleration ramp, 0.1 s               read / write (Maintenance)
 *
 * Control word sequences (CiA402 state machine, I/O profile "not separate"):
 *   run forward  0x0006 -> 0x0007 -> 0x000F      (shutdown, switch on, enable operation)
 *   run reverse  0x0006 -> 0x0007 -> 0x080F      (bit 11 = reverse)
 *   stop         0x0007                          (disable operation: ramp stop)
 *   fault reset  0x0080, then 0x0000             (bit 7 rising edge)
 * Status word: running = (ETA & 0x006F) == 0x0027, fault = bit 3, remote (network has control) = bit 9,
 * reverse = bit 15.
 *
 * On the drive: set the command and reference channel to the embedded Ethernet / Modbus TCP
 * (e.g. [Ref Freq 1 Config] FR1 and [Cmd channel 1] CD1, or the drive's "Modbus TCP" macro);
 * otherwise "Network control" shows Off and the drive ignores the commands (reads still work).
 * Check the exact parameter names in your drive's programming manual.
 */

#include "devices/device_profile.h"
#include "core/security.h"
#include "renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum {
    AV_ETA = 0, AV_FREQ, AV_FREQ_REF, AV_CURRENT, AV_LAST_FAULT, AV_CMD, AV_SPEED_SP, AV_ACCEL, AV_DECEL,
    AV_RUN, AV_FAULT, AV_REMOTE, AV_REVERSE, AV_COMM
};

/* ETA and CMD are raw words: read for the status bits, not shown (no meta) */
static const hmi_tag_meta_t s_meta[] = {
    { AV_FREQ,       TAG_TYPE_ANALOG,  "Output frequency", "Hz", 0, 600, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_FREQ_REF,   TAG_TYPE_ANALOG,  "Frequency ref",    "Hz", 0, 600, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_CURRENT,    TAG_TYPE_ANALOG,  "Motor current",    "A",  0, 200, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_LAST_FAULT, TAG_TYPE_ANALOG,  "Last fault code",  "",   0, 999, 0, 0, 0, 0, NULL, NULL, NULL },
    { AV_SPEED_SP,   TAG_TYPE_ANALOG,  "Speed reference",  "Hz", 0, 600, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_ACCEL,      TAG_TYPE_ANALOG,  "Acceleration",     "s",  0, 999, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_DECEL,      TAG_TYPE_ANALOG,  "Deceleration",     "s",  0, 999, 0, 0, 1, 0, NULL, NULL, NULL },
    { AV_RUN,        TAG_TYPE_DIGITAL, "Motor",            "",   0, 1, 0, 0, 0, 0, NULL, NULL, NULL },
    { AV_FAULT,      TAG_TYPE_DIGITAL, "Drive fault",      "",   0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "Read the fault code on the drive display", "Press Reset fault once the cause is cleared", NULL },
    { AV_REMOTE,     TAG_TYPE_DIGITAL, "Network control",  "",   0, 1, 0, 0, 0, 0, NULL, NULL, NULL },
    { AV_REVERSE,    TAG_TYPE_DIGITAL, "Reverse",          "",   0, 1, 0, 0, 0, 0, NULL, NULL, NULL },
    { AV_COMM,       TAG_TYPE_DIGITAL, "Drive comm loss",  "",   0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "Drive may still be running: check it at the machine", "Then check cable, power, IP address, Modbus TCP", NULL },
};

static const uint8_t s_status[] = { AV_ETA, AV_FREQ, AV_FREQ_REF, AV_CURRENT };   /* 3201..3204 */
static const uint8_t s_cmd[]    = { AV_CMD, AV_SPEED_SP };                         /* 8501..8502 */
static const uint8_t s_ramps[]  = { AV_ACCEL, AV_DECEL };                          /* 9001..9002 */
static const uint8_t s_fault[]  = { AV_LAST_FAULT };                               /* 7121 */
static const hmi_block_map_t s_more[] = {
    { 8501, N(s_cmd), s_cmd, HMI_FMT_INT16 },
    { 9001, N(s_ramps), s_ramps, HMI_FMT_INT16 },
    { 7121, N(s_fault), s_fault, HMI_FMT_INT16 },
};
static const hmi_bit_map_t s_bits[] = {
    { AV_ETA, AV_RUN,     0x006F, 0x0027 },
    { AV_ETA, AV_FAULT,   0x0008, 0x0008 },
    { AV_ETA, AV_REMOTE,  0x0200, 0x0200 },
    { AV_ETA, AV_REVERSE, 0x8000, 0x8000 },
};

#define CMD 8501u
static const int16_t s_run_fwd[] = { 0x0006, 0x0007, 0x000F };
static const int16_t s_run_rev[] = { 0x0006, 0x0007, 0x080F };
static const int16_t s_stop[]    = { 0x0007 };
static const int16_t s_reset[]   = { 0x0080, 0x0000 };

static const hmi_device_action_t s_actions[] = {
    HMI_A_RUN_WORD  (AV_RUN, CMD, AV_FAULT, s_run_fwd, "Run forward", "Drive", false),
    HMI_A_RUN_WORD  (AV_RUN, CMD, AV_FAULT, s_run_rev, "Run reverse", "Drive", true),
    HMI_A_STOP_WORD (AV_RUN, CMD, s_stop, "Stop", "Drive", false),
    HMI_A_SET       (AV_SPEED_SP, 8502, HMI_CMD_SETPOINT, "Set speed", "Speed reference", 0, 500),
    HMI_A_SET       (AV_ACCEL, 9001, HMI_CMD_TUNE, "Set ramp", "Acceleration", 1, 999),
    HMI_A_SET       (AV_DECEL, 9002, HMI_CMD_TUNE, "Set ramp", "Deceleration", 1, 999),
    HMI_A_PULSE_WORD(AV_FAULT, CMD, s_reset, "Reset fault", "Drive fault", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(AV_FREQ,     WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(AV_FREQ,     WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(AV_FREQ_REF, WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(AV_FREQ,     WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(AV_CURRENT,  WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(AV_CURRENT,  WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(AV_RUN,      WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(AV_RUN,      WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(AV_RUN,      WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(AV_SPEED_SP, WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(AV_SPEED_SP, WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(AV_REMOTE,   WIDGET_STATUS_PILL,    11, 7, 5, 2),
    HMI_TOKEN_PACK(AV_ACCEL,    WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(AV_SPEED_SP, WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several devices): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(AV_FREQ,     WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(AV_FREQ,     WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(AV_RUN,      WIDGET_STATUS_PILL,    10, 4, 6, 3),
    HMI_TOKEN_PACK(AV_CURRENT,  WIDGET_ANALOG_BAR,     10, 7, 6, 3),
    HMI_TOKEN_PACK(AV_RUN,      WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(AV_SPEED_SP, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_altivar = {
    "altivar", "Altivar drive", "schneider", "atv", true, true,
    s_meta, N(s_meta),
    { 3201, N(s_status), s_status, HMI_FMT_INT16 },
    { 0, 0, NULL, HMI_FMT_INT16 },                       /* Altivars have no coils */
    s_actions, N(s_actions),
    AV_COMM, HMI_REL_NONE, 0xFFFFu, AV_FREQ,
    s_full, N(s_full),
    s_compact, N(s_compact),
    s_more, N(s_more),
    s_bits, N(s_bits),
    NULL,                                                /* fixed product map */
    false, 0,                                       /* drive: use its own communication-loss fault instead */
};
