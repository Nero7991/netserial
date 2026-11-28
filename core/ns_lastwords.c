#include "ns_lastwords.h"
#include <stddef.h>
#include <string.h>

#define LW_MAGIC 0x4C415354u   // "LAST"

static uint32_t lw_check(const ns_lw_t *lw)
{
    return (lw->magic ^ 0x9E3779B9u) + lw->size * 31u + (uint32_t)lw->addr * 7u + ~lw->head;
}

static uint8_t *lw_buf(const ns_lw_t *lw) { return (uint8_t *)lw->addr; }

void ns_lw_init(ns_lw_t *lw, uint8_t *buf, uint32_t size)
{
    lw->magic = LW_MAGIC;
    lw->size = size;
    lw->addr = (uintptr_t)buf;
    lw->head = 0;
    lw->check = lw_check(lw);
}

bool ns_lw_valid(const ns_lw_t *lw, const uint8_t *buf, uint32_t size)
{
    return lw->magic == LW_MAGIC && lw->size == size && size != 0 &&
           lw->addr == (uintptr_t)buf && lw->check == lw_check(lw);
}

void ns_lw_write(ns_lw_t *lw, const void *p, uint32_t n)
{
    const uint8_t *src = p;
    uint8_t *buf = lw_buf(lw);
    if (n > lw->size) {   // only the tail can survive
        src += n - lw->size;
        lw->head += n - lw->size;
        n = lw->size;
    }
    uint32_t idx = lw->head % lw->size;
    uint32_t first = lw->size - idx < n ? lw->size - idx : n;
    memcpy(buf + idx, src, first);
    memcpy(buf, src + first, n - first);
    lw->head += n;
    lw->check = lw_check(lw);
}

void ns_lw_segments(const ns_lw_t *lw, const uint8_t **p1, uint32_t *n1, const uint8_t **p2,
                    uint32_t *n2)
{
    const uint8_t *buf = lw_buf(lw);
    *p1 = *p2 = buf;
    *n1 = *n2 = 0;
    if (lw->head <= lw->size) {   // never wrapped: everything, from the start
        *n1 = lw->head;
        return;
    }
    // Wrapped: the oldest byte is at head % size. Skip to just after the first newline.
    uint32_t start = lw->head % lw->size, len = lw->size, skip = 0;
    while (skip < len && buf[(start + skip) % lw->size] != '\n') skip++;
    if (skip == len) return;   // no line boundary at all: a single over-long line, replay nothing
    skip++;
    start = (start + skip) % lw->size;
    len -= skip;
    uint32_t first = lw->size - start < len ? lw->size - start : len;
    *p1 = buf + start;
    *n1 = first;
    *n2 = len - first;
}

static void lw_skip(const uint8_t **p1, uint32_t *n1, const uint8_t **p2, uint32_t *n2, uint32_t k)
{
    uint32_t a = k < *n1 ? k : *n1;
    *p1 += a;
    *n1 -= a;
    k -= a;
    if (k > *n2) k = *n2;
    *p2 += k;
    *n2 -= k;
}

void ns_lw_trim(const uint8_t **p1, uint32_t *n1, const uint8_t **p2, uint32_t *n2, uint32_t max)
{
    uint32_t total = *n1 + *n2;
    if (total <= max) return;
    const uint8_t *p2_start = *p2;
    lw_skip(p1, n1, p2, n2, total - max);
    // Now inside a line, unless the cut landed right after a newline. The byte before the cut is
    // in the first segment unless the cut moved into the second.
    const uint8_t *before = *p2 > p2_start ? *p2 - 1 : *p1 - 1;
    if (*before == '\n') return;
    uint32_t rest = *n1 + *n2;
    for (uint32_t i = 0; i < rest; i++) {
        uint8_t c = i < *n1 ? (*p1)[i] : (*p2)[i - *n1];
        if (c == '\n') {
            if (i + 1 < rest) lw_skip(p1, n1, p2, n2, i + 1);
            return;
        }
    }
}
