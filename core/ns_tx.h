// ns_tx: decides the next datagram to send from a ring. In ACK mode bytes leave the ring only when
// the host acknowledges them, and unacknowledged bytes are resent from the tail after resend_ms
// without ACK progress. Without ACK mode a successful send commits the bytes. Not thread-safe:
// the caller serializes it with the ring.
#pragma once
#include "ns_ring.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    ns_ring_t *ring;
    uint32_t boot_id;
    bool ack_mode;
    uint32_t resend_ms;
    uint32_t send_off;           // next stream offset to transmit
    uint32_t last_progress_ms;   // last ACK progress, or when bytes first went out unacknowledged
    uint32_t reported_dropped;   // ring->dropped value the host has been told about
    uint8_t pend_type;           // frame built by the last ns_tx_next (0 = none)
    uint32_t pend_len;           // its DATA payload length, or its DROPPED count
    uint32_t pend_ms;            // now_ms when it was built
} ns_tx_t;

void ns_tx_init(ns_tx_t *t, ns_ring_t *r, uint32_t boot_id, bool ack_mode, uint32_t resend_ms);
uint32_t ns_tx_next(ns_tx_t *t, uint32_t now_ms, uint8_t *out, uint32_t cap);
void ns_tx_sent(ns_tx_t *t, uint32_t frame_len, bool ok);
void ns_tx_on_ack(ns_tx_t *t, uint32_t off, uint32_t now_ms);
