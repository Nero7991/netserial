#include "ns_ring.h"
#include <string.h>

bool ns_ring_init(ns_ring_t *r, uint8_t *buf, uint32_t size)
{
    if (size == 0 || (size & (size - 1)) != 0) return false;
    r->buf = buf;
    r->size = size;
    r->head_off = r->tail_off = 0;
    r->dropped = 0;
    return true;
}

uint32_t ns_ring_used(const ns_ring_t *r) { return r->head_off - r->tail_off; }

// Copy n bytes between the ring at stream offset off and a linear buffer, splitting at the end.
static void ring_copy(const ns_ring_t *r, uint32_t off, uint8_t *lin, uint32_t n, bool to_ring)
{
    uint32_t idx = off & (r->size - 1);
    uint32_t first = r->size - idx < n ? r->size - idx : n;
    if (to_ring) {
        memcpy(r->buf + idx, lin, first);
        memcpy(r->buf, lin + first, n - first);
    } else {
        memcpy(lin, r->buf + idx, first);
        memcpy(lin + first, r->buf, n - first);
    }
}

bool ns_ring_write(ns_ring_t *r, const void *p, uint32_t n)
{
    if (n > r->size - ns_ring_used(r)) {
        r->dropped += n;
        return false;
    }
    ring_copy(r, r->head_off, (uint8_t *)(uintptr_t)p, n, true);
    r->head_off += n;
    return true;
}

uint32_t ns_ring_peek(const ns_ring_t *r, uint32_t from_off, uint8_t *out, uint32_t max)
{
    if (ns_off_diff(from_off, r->tail_off) < 0) from_off = r->tail_off;
    if (ns_off_diff(r->head_off, from_off) <= 0) return 0;
    uint32_t n = r->head_off - from_off;
    if (n > max) n = max;
    ring_copy(r, from_off, out, n, false);
    return n;
}

void ns_ring_ack(ns_ring_t *r, uint32_t off)
{
    if (ns_off_diff(off, r->tail_off) >= 0 && ns_off_diff(r->head_off, off) >= 0) r->tail_off = off;
}
