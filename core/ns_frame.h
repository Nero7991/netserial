// ns_frame: the UDP wire format. 18-byte little-endian header, then `len` payload bytes:
//   magic "NS" | ver u8 | type u8 | boot_id u32 | seq u32 | dev_ms u32 | len u16
// DATA: seq = stream offset of the first payload byte. DROPPED: payload u32 total bytes dropped.
// HELLO (host -> device): no payload. ACK (host -> device): seq = next offset the host expects,
// i.e. every byte below seq has arrived.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NS_FRAME_HDR 18
#define NS_FRAME_MAX 1400
#define NS_FRAME_VER 1

// SYNC (device -> host, reply to HELLO): no payload, seq = oldest offset the device still holds.
// A host whose next expected offset is below it has missed those bytes for good (another host
// ACKed them): it reports a GAP and continues from seq instead of waiting for them.
// BYE (device -> host): a HELLO from another host took the stream. Payload: the new host's IPv4
// address (4 bytes) and UDP port (2 bytes), network byte order.
enum { NS_DATA = 1, NS_DROPPED = 2, NS_HELLO = 3, NS_ACK = 4, NS_SYNC = 5, NS_BYE = 6 };

// BYE payload for a host at ip_host / port_host (both in host byte order).
void ns_bye_payload(uint32_t ip_host, uint16_t port_host, uint8_t out[6]);

typedef struct {
    uint8_t type;
    uint32_t boot_id, seq, dev_ms;
    uint16_t len;
} ns_hdr_t;

// Write just the 18-byte header (for a payload the caller already placed at out + NS_FRAME_HDR).
void ns_frame_put_hdr(const ns_hdr_t *h, uint8_t *out);
uint32_t ns_frame_encode(const ns_hdr_t *h, const uint8_t *payload, uint8_t *out, uint32_t cap);
bool ns_frame_decode(const uint8_t *in, uint32_t n, ns_hdr_t *h, const uint8_t **payload);
