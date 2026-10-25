/**
 * @file modbus_proto.c
 * @brief Pure Modbus TCP (MBAP) Frame Encoder and Decoder
 * @standards Modbus Application Protocol v1.1b3, Modbus Messaging on TCP/IP v1.0b
 */

#include "../../include/drivers/modbus_proto.h"
#include <string.h>

size_t mb_encode_request(const mb_request_t *req, uint8_t *out, size_t cap)
{
    if (!req || !out || cap < 12) return 0;

    if (req->fc != MB_FC_READ_COILS &&
        req->fc != MB_FC_READ_HOLDING &&
        req->fc != MB_FC_WRITE_SINGLE_COIL &&
        req->fc != MB_FC_WRITE_SINGLE_REG) {
        return 0;
    }

    if (req->fc == MB_FC_READ_COILS) {
        if (req->qty_or_value < 1 || req->qty_or_value > MB_MAX_READ_COILS) return 0;
        if ((uint32_t)req->addr + req->qty_or_value > 65536u) return 0;
    } else if (req->fc == MB_FC_READ_HOLDING) {
        if (req->qty_or_value < 1 || req->qty_or_value > MB_MAX_READ_REGS) return 0;
        if ((uint32_t)req->addr + req->qty_or_value > 65536u) return 0;
    }

    /* MBAP Header */
    out[0] = (uint8_t)(req->tid >> 8);
    out[1] = (uint8_t)(req->tid & 0xFF);
    out[2] = 0; /* Protocol ID High */
    out[3] = 0; /* Protocol ID Low */
    out[4] = 0; /* Length High */
    out[5] = 6; /* Length Low: 1 unit id + 5 PDU bytes */
    out[6] = req->unit;

    /* PDU */
    out[7] = req->fc;
    out[8] = (uint8_t)(req->addr >> 8);
    out[9] = (uint8_t)(req->addr & 0xFF);

    uint16_t val;
    if (req->fc == MB_FC_WRITE_SINGLE_COIL) {
        val = req->qty_or_value ? 0xFF00u : 0x0000u;
    } else {
        val = req->qty_or_value;
    }
    out[10] = (uint8_t)(val >> 8);
    out[11] = (uint8_t)(val & 0xFF);

    return 12;
}

int32_t mb_adu_length(const uint8_t *buf, size_t len)
{
    if (!buf || len < MB_MBAP_LEN) return 0;

    /* Protocol ID must be 0 for Modbus TCP */
    if (buf[2] != 0 || buf[3] != 0) return -1;

    uint16_t pdu_len = (uint16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    if (pdu_len < 2 || pdu_len > 254) return -1;

    return (int32_t)(6 + pdu_len);
}

mb_status_t mb_decode_response(const mb_request_t *req, const uint8_t *adu, size_t len, mb_response_t *out)
{
    if (!req || !adu || !out) return MB_ERR_ARG;
    if (len < 8) return MB_ERR_LEN;

    uint16_t tid = (uint16_t)(((uint16_t)adu[0] << 8) | adu[1]);
    if (tid != req->tid) return MB_ERR_TID;

    if (adu[2] != 0 || adu[3] != 0) return MB_ERR_PROTO;

    uint16_t mbap_len = (uint16_t)(((uint16_t)adu[4] << 8) | adu[5]);
    if ((size_t)(mbap_len + 6) != len) return MB_ERR_PROTO;

    if (adu[6] != req->unit) return MB_ERR_UNIT;

    uint8_t fc = adu[7];
    memset(out, 0, sizeof(*out));

    if (fc == (req->fc | MB_EXCEPTION_FLAG)) {
        if (len < 9) return MB_ERR_LEN;
        out->fc = req->fc;
        out->exception_code = adu[8];
        return MB_RESP_EXCEPTION;
    }

    if (fc != req->fc) return MB_ERR_FC;
    out->fc = fc;

    if (fc == MB_FC_READ_COILS) {
        if (len < 9) return MB_ERR_LEN;
        uint8_t byte_count = adu[8];
        uint16_t expected_bytes = (uint16_t)((req->qty_or_value + 7) / 8);
        if (byte_count != expected_bytes || len != (size_t)(9 + byte_count)) return MB_ERR_LEN;

        out->count = req->qty_or_value;
        if (byte_count > sizeof(out->coil_bytes)) byte_count = sizeof(out->coil_bytes);
        memcpy(out->coil_bytes, &adu[9], byte_count);
        return MB_RESP_OK;
    }

    if (fc == MB_FC_READ_HOLDING) {
        if (len < 9) return MB_ERR_LEN;
        uint8_t byte_count = adu[8];
        uint16_t expected_bytes = (uint16_t)(req->qty_or_value * 2);
        if (byte_count != expected_bytes || len != (size_t)(9 + byte_count)) return MB_ERR_LEN;

        out->count = req->qty_or_value;
        for (uint16_t i = 0; i < req->qty_or_value; i++) {
            out->regs[i] = (uint16_t)(((uint16_t)adu[9 + 2 * i] << 8) | adu[9 + 2 * i + 1]);
        }
        return MB_RESP_OK;
    }

    if (fc == MB_FC_WRITE_SINGLE_COIL || fc == MB_FC_WRITE_SINGLE_REG) {
        if (len != 12) return MB_ERR_LEN;
        uint16_t echo_addr = (uint16_t)(((uint16_t)adu[8] << 8) | adu[9]);
        uint16_t echo_val  = (uint16_t)(((uint16_t)adu[10] << 8) | adu[11]);

        uint16_t expected_val = (fc == MB_FC_WRITE_SINGLE_COIL) ? (req->qty_or_value ? 0xFF00u : 0x0000u)
                                                                 : req->qty_or_value;
        if (echo_addr != req->addr || echo_val != expected_val) {
            return MB_ERR_ECHO;
        }
        out->echo_addr  = echo_addr;
        out->echo_value = echo_val;
        return MB_RESP_OK;
    }

    return MB_ERR_ARG;
}

bool mb_coil_get(const mb_response_t *rsp, uint16_t idx)
{
    if (!rsp || idx >= rsp->count) return false;
    return (rsp->coil_bytes[idx / 8] & (1 << (idx % 8))) != 0;
}

const char* mb_status_name(mb_status_t st)
{
    switch (st) {
        case MB_RESP_OK:        return "OK";
        case MB_RESP_EXCEPTION: return "EXCEPTION";
        case MB_ERR_TID:   return "ERR_TID";
        case MB_ERR_PROTO: return "ERR_PROTO";
        case MB_ERR_UNIT:  return "ERR_UNIT";
        case MB_ERR_FC:    return "ERR_FC";
        case MB_ERR_LEN:   return "ERR_LEN";
        case MB_ERR_ECHO:  return "ERR_ECHO";
        case MB_ERR_ARG:   return "ERR_ARG";
        default:           return "UNKNOWN";
    }
}
