# ADR-0005: Re-check every device's identity after each connect

**Status:** accepted (Build-19)

## Context
A device is attached once, by its address and its FC43 identity. If that address later answers
from a different device (a replaced unit, an IP conflict, a rogue device), the HMI would read
wrong values into the old device's screen and could send it commands meant for another machine.

## Decision
After every TCP connect, before anything is read or written, ask for the device identity
(FC43 / MEI 0x0E). Compare its vendor and product with the ones seen when it was attached.
- **Match:** normal polling.
- **Different, refused or malformed:** drop the link; raise the comm-loss alarm; journal
  "other device answers, blocked" once; retry after the normal backoff.
- **A device attached without an identity** (listed with `--plc` and silent to FC43) cannot be
  checked, and is not.

## Consequences
- Seen in a live run: 8 devices whose addresses started answering as different types were all
  blocked. After the usual 10 s offline removal, discovery attached the new devices with their
  own drivers.
- One extra request per connect.
- It does not stop a device that copies another device's identity. Modbus has no device
  authentication, so network segmentation is still required.
