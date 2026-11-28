// ns_lastwords: a small flight recorder for the last output before a crash. Every line also goes
// into this circular buffer (overwriting the oldest), independent of the ACK ring, which keeps the
// oldest output when full. Header and buffer live in RAM that survives a software, panic or
// watchdog reset (.noinit on ESP-IDF), so the next boot can replay the newest whole lines.
// Not thread-safe: the caller serializes it with the ring.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t size;    // buffer size this header was built for
    uintptr_t addr;   // buffer address, so a buffer that moved between builds reads as invalid
    uint32_t head;    // total bytes ever written; next write index is head % size
    uint32_t check;   // ties the fields together so a torn or random header reads as invalid
} ns_lw_t;

void ns_lw_init(ns_lw_t *lw, uint8_t *buf, uint32_t size);
bool ns_lw_valid(const ns_lw_t *lw, const uint8_t *buf, uint32_t size);
void ns_lw_write(ns_lw_t *lw, const void *p, uint32_t n);
// The newest content in order, as up to two segments. When the buffer has wrapped, the first
// (partial) line is skipped so the replay starts on a line boundary.
void ns_lw_segments(const ns_lw_t *lw, const uint8_t **p1, uint32_t *n1, const uint8_t **p2,
                    uint32_t *n2);
// Keep at most max bytes of the two segments: drop the oldest, then start after the next
// newline so the replay begins on a whole line (a single line longer than max keeps its tail).
void ns_lw_trim(const uint8_t **p1, uint32_t *n1, const uint8_t **p2, uint32_t *n2, uint32_t max);
