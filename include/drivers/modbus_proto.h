/**
 * @file modbus_proto.h
 * @brief Pure Modbus TCP (MBAP) frame encoder / decoder - no sockets, no heap
 * @standards Modbus Application Protocol v1.1b3, Modbus Messaging on TCP/IP v1.0b
 *
 * ADU = MBAP header (7 bytes) + PDU.
 *   MBAP: transaction id (2) | protocol id = 0 (2) | length (2) = unit(1)+PDU | unit id (1)
 * All multi-byte fields are big-endian.
 */

#ifndef INDUSTRIAL_HMI_MODBUS_PROTO_H
#define INDUSTRIAL_HMI_MODBUS_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MB_MBAP_LEN              7
#define MB_MAX_ADU               260   /* 7 + 253 */
#define MB_MAX_PDU               253

#define MB_FC_READ_COILS         0x01
#define MB_FC_READ_HOLDING       0x03
#define MB_FC_WRITE_SINGLE_COIL  0x05
#define MB_FC_WRITE_SINGLE_REG   0x06
#define MB_EXCEPTION_FLAG        0x80

#define MB_MAX_READ_REGS         125
#define MB_MAX_READ_COILS        2000

/* Exception codes */
#define MB_EX_ILLEGAL_FUNCTION   0x01
#define MB_EX_ILLEGAL_ADDRESS    0x02
#define MB_EX_ILLEGAL_VALUE      0x03
#define MB_EX_DEVICE_FAILURE     0x04

typedef enum {
    MB_RESP_OK        = 0,
    MB_RESP_EXCEPTION = 1,   /* well-formed exception response; see exception_code */
    MB_ERR_TID        = -1,  /* transaction id mismatch */
    MB_ERR_PROTO    = -2,  /* protocol id != 0 or MBAP length inconsistent with len */
    MB_ERR_UNIT     = -3,  /* unit id mismatch */
    MB_ERR_FC       = -4,  /* function code mismatch */
    MB_ERR_LEN      = -5,  /* PDU length / byte count wrong for the request */
    MB_ERR_ECHO     = -6,  /* FC05/FC06 echo does not match the request */
    MB_ERR_ARG      = -7   /* bad arguments (NULL, unsupported fc) */
} mb_status_t;

typedef struct {
    uint16_t tid;
    uint8_t  unit;
    uint8_t  fc;             /* MB_FC_* */
    uint16_t addr;           /* 0-based PDU address */
    uint16_t qty_or_value;   /* FC01/03: quantity; FC05: 0 = OFF, non-zero = ON; FC06: register value */
} mb_request_t;

typedef struct {
    uint8_t  fc;                         /* function code from the response (without 0x80) */
    uint8_t  exception_code;             /* valid when status == MB_EXCEPTION */
    uint16_t count;                      /* registers or coils decoded */
    uint16_t regs[MB_MAX_READ_REGS];     /* FC03 */
    uint8_t  coil_bytes[(MB_MAX_READ_COILS + 7) / 8]; /* FC01, packed LSB-first */
    uint16_t echo_addr;                  /* FC05/06 */
    uint16_t echo_value;                 /* FC05/06 */
} mb_response_t;

/* Encode a request ADU. Returns its length (12 for all four supported function
 * codes), or 0 if req/out is NULL, cap is too small, the fc is unsupported, or a
 * quantity is out of range (FC01: 1..2000, FC03: 1..125, addr + qty <= 65536). */
size_t mb_encode_request(const mb_request_t *req, uint8_t *out, size_t cap);

/* Given the bytes received so far: returns the total ADU length once the MBAP
 * header is available and plausible (protocol id 0, length field 2..254 ->
 * 6 + length), 0 if fewer than 7 bytes are available, -1 if the header is invalid. */
int32_t mb_adu_length(const uint8_t *buf, size_t len);

/* Decode one complete response ADU (exactly len bytes) against the request it answers. */
mb_status_t mb_decode_response(const mb_request_t *req, const uint8_t *adu, size_t len, mb_response_t *out);

/* Bit idx of an FC01 response (false if idx >= count). */
bool mb_coil_get(const mb_response_t *rsp, uint16_t idx);

const char* mb_status_name(mb_status_t st);

/* --- FC43 / MEI 0x0E: Read Device Identification (basic objects) --- */
#define MB_FC_ENCAPSULATED       0x2B
#define MB_MEI_DEVICE_ID         0x0E

typedef struct {
    char vendor[32];      /* object 0x00 VendorName  */
    char product[32];     /* object 0x01 ProductCode */
    char revision[16];    /* object 0x02 MajorMinorRevision */
} mb_device_id_t;

/* Request for the basic identification objects. Returns 11 (bytes written) or 0. */
size_t mb_encode_devid_request(uint16_t tid, uint8_t unit, uint8_t *out, size_t cap);

/* Decode a complete FC43/0x0E response ADU. MB_RESP_EXCEPTION when the device
 * does not support identification. Strings are NUL-terminated, non-printable
 * bytes replaced by '?', and truncated to fit. */
mb_status_t mb_decode_devid(const uint8_t *adu, size_t len, uint16_t tid, uint8_t unit, mb_device_id_t *out);

#endif /* INDUSTRIAL_HMI_MODBUS_PROTO_H */
