// ns_tx: decides the next datagram to send from a ring. In ACK mode bytes leave the ring only when
// the host acknowledges them, and unacknowledged bytes are resent from the tail after resend_ms
// without ACK progress. Without ACK mode a successful send commits the bytes. Not thread-safe:
// the caller serializes it with the ring.
#pragma once
#include "ns_ring.h"
#include <stdbool.h>
#include <stdint.h>

// Resend cycles without ACK progress before giving up until the next HELLO (ns_tx_rewind) or ACK,
// so a device whose host went away does not keep the radio busy resending its ring forever.
#define NS_TX_MAX_RESENDS 3

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
    uint8_t resends;             // resend cycles since the last ACK progress
    bool resend_dropped;         // send DROPPED again: the last one may have been lost
    bool sync_pending;           // a HELLO arrived: tell the host the oldest held offset (SYNC)
} ns_tx_t;

void ns_tx_init(ns_tx_t *t, ns_ring_t *r, uint32_t boot_id, bool ack_mode, uint32_t resend_ms);
uint32_t ns_tx_next(ns_tx_t *t, uint32_t now_ms, uint8_t *out, uint32_t cap);
void ns_tx_sent(ns_tx_t *t, uint32_t frame_len, bool ok);
// A host said HELLO: resend everything held, from the tail, and the drop count.
void ns_tx_rewind(ns_tx_t *t, uint32_t now_ms);
void ns_tx_on_ack(ns_tx_t *t, uint32_t off, uint32_t now_ms);
// Bytes are out and an ACK for them is expected soon (the caller should listen for it rather than
// wait for the next flush). False once the resends gave up: nobody is listening.
bool ns_tx_awaiting_ack(const ns_tx_t *t);
