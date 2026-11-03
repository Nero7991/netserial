// ns_ring: byte ring addressed by stream offsets. head_off is the offset of the next byte written,
// tail_off the oldest byte still held (unacknowledged). Offsets are u32 and wrap; every comparison
// is wrap-safe. A write that does not fit is dropped whole and counted: the oldest output is kept.
// size must be a power of two: the index is off & (size - 1), which stays continuous when the u32
// offset wraps. Not thread-safe: the caller serializes access (the ESP-IDF glue holds a spinlock).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t size;
    uint32_t head_off;
    uint32_t tail_off;
    uint32_t dropped;   // bytes refused because the ring was full, since init
} ns_ring_t;

bool ns_ring_init(ns_ring_t *r, uint8_t *buf, uint32_t size);   // false: size not a power of two
bool ns_ring_write(ns_ring_t *r, const void *p, uint32_t n);
uint32_t ns_ring_peek(const ns_ring_t *r, uint32_t from_off, uint8_t *out, uint32_t max);
void ns_ring_ack(ns_ring_t *r, uint32_t off);
uint32_t ns_ring_used(const ns_ring_t *r);

// a - b as a signed distance, valid while the two offsets are within 2^31 of each other.
static inline int32_t ns_off_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
