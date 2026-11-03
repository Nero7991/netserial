#include "ns_tx.h"
#include "ns_frame.h"

void ns_tx_init(ns_tx_t *t, ns_ring_t *r, uint32_t boot_id, bool ack_mode, uint32_t resend_ms)
{
    *t = (ns_tx_t){ .ring = r, .boot_id = boot_id, .ack_mode = ack_mode, .resend_ms = resend_ms,
                    .send_off = r->tail_off, .reported_dropped = r->dropped };
}

uint32_t ns_tx_next(ns_tx_t *t, uint32_t now_ms, uint8_t *out, uint32_t cap)
{
    ns_ring_t *r = t->ring;
    t->pend_type = 0;
    if (ns_off_diff(t->send_off, r->tail_off) < 0) t->send_off = r->tail_off;
    if (t->ack_mode && t->send_off != r->tail_off && t->resends < NS_TX_MAX_RESENDS &&
        now_ms - t->last_progress_ms >= t->resend_ms) {
        t->send_off = r->tail_off;          // no ACK progress: go back and resend everything held
        t->last_progress_ms = now_ms;
        t->resends++;
        t->resend_dropped = r->dropped != 0;
    }
    ns_hdr_t h = { .boot_id = t->boot_id, .dev_ms = now_ms };
    if (r->dropped != t->reported_dropped || t->resend_dropped) {
        uint8_t p[4] = { (uint8_t)r->dropped, (uint8_t)(r->dropped >> 8), (uint8_t)(r->dropped >> 16),
                         (uint8_t)(r->dropped >> 24) };
        h.type = NS_DROPPED;
        h.seq = r->head_off;
        h.len = 4;
        uint32_t n = ns_frame_encode(&h, p, out, cap);
        if (n) {
            t->pend_type = NS_DROPPED;
            t->pend_len = r->dropped;
        }
        return n;
    }
    if (cap <= NS_FRAME_HDR) return 0;
    uint32_t room = (cap < NS_FRAME_MAX ? cap : NS_FRAME_MAX) - NS_FRAME_HDR;
    uint32_t n = ns_ring_peek(r, t->send_off, out + NS_FRAME_HDR, room);
    if (n == 0) return 0;
    h.type = NS_DATA;
    h.seq = t->send_off;
    h.len = (uint16_t)n;
    ns_frame_put_hdr(&h, out);   // the payload was peeked straight into place
    t->pend_type = NS_DATA;
    t->pend_ms = now_ms;
    t->pend_len = n;
    return NS_FRAME_HDR + n;
}

void ns_tx_sent(ns_tx_t *t, uint32_t frame_len, bool ok)
{
    uint8_t type = t->pend_type;
    t->pend_type = 0;
    if (!ok || frame_len == 0 || type == 0) return;
    if (type == NS_DROPPED) {
        t->reported_dropped = t->pend_len;
        t->resend_dropped = false;
        return;
    }
    // First unacknowledged bytes out: the resend clock starts now, not at some earlier ACK.
    if (t->send_off == t->ring->tail_off) t->last_progress_ms = t->pend_ms;
    t->send_off += t->pend_len;
    if (!t->ack_mode) ns_ring_ack(t->ring, t->send_off);
}

void ns_tx_on_ack(ns_tx_t *t, uint32_t off, uint32_t now_ms)
{
    ns_ring_t *r = t->ring;
    if (ns_off_diff(off, r->tail_off) <= 0 || ns_off_diff(t->send_off, off) < 0) return;
    ns_ring_ack(r, off);
    t->last_progress_ms = now_ms;
    t->resends = 0;
}

void ns_tx_rewind(ns_tx_t *t, uint32_t now_ms)
{
    t->send_off = t->ring->tail_off;
    t->last_progress_ms = now_ms;
    t->resends = 0;
    t->resend_dropped = t->ring->dropped != 0;
}
