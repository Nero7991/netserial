#include "ns_line.h"
#include "ns_ring.h"
#include "ns_test.h"
#include <string.h>

static void ring_write_peek_ack(void)
{
    uint8_t buf[16], out[16];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    CHECK(ns_ring_write(&r, "abc", 3));
    CHECK_EQ(ns_ring_peek(&r, 0, out, sizeof out), 3);
    CHECK(memcmp(out, "abc", 3) == 0);
    ns_ring_ack(&r, 2);
    CHECK_EQ(ns_ring_used(&r), 1);
    CHECK_EQ(ns_ring_peek(&r, 2, out, sizeof out), 1);
    CHECK_EQ(out[0], 'c');
}

static void ring_full_keeps_oldest(void)
{
    uint8_t buf[8], out[8];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    CHECK(ns_ring_write(&r, "123456", 6));
    CHECK(!ns_ring_write(&r, "789", 3));
    CHECK_EQ(r.dropped, 3);
    CHECK_EQ(ns_ring_peek(&r, 0, out, sizeof out), 6);
    CHECK(memcmp(out, "123456", 6) == 0);
}

static void ring_write_larger_than_ring_drops(void)
{
    uint8_t buf[8];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    CHECK(!ns_ring_write(&r, "123456789", 9));
    CHECK_EQ(ns_ring_used(&r), 0);
    CHECK_EQ(r.dropped, 9);
}

static void ring_wraps_index(void)
{
    uint8_t buf[8], out[8];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    for (int i = 0; i < 20; i++) {
        char m[5];
        snprintf(m, sizeof m, "%c%c%c%c", 'a' + i, 'b' + i, 'c' + i, 'd' + i);
        uint32_t off = r.head_off;
        CHECK(ns_ring_write(&r, m, 4));
        CHECK_EQ(ns_ring_peek(&r, off, out, sizeof out), 4);
        CHECK(memcmp(out, m, 4) == 0);
        ns_ring_ack(&r, off + 4);
        CHECK_EQ(ns_ring_used(&r), 0);
    }
}

static void ring_offsets_wrap(void)
{
    uint8_t buf[64], in[32], out[32];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    r.head_off = r.tail_off = 0xFFFFFFF0u;
    for (int i = 0; i < 32; i++) in[i] = (uint8_t)i;
    CHECK(ns_ring_write(&r, in, 32));
    CHECK_EQ(ns_ring_used(&r), 32);
    CHECK_EQ(ns_ring_peek(&r, 0xFFFFFFF0u, out, sizeof out), 32);
    CHECK(memcmp(in, out, 32) == 0);
    ns_ring_ack(&r, 0x00000010u);
    CHECK_EQ(ns_ring_used(&r), 0);
}

static void ring_ack_out_of_range_noop(void)
{
    uint8_t buf[16];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    ns_ring_write(&r, "abcd", 4);
    ns_ring_ack(&r, 9);            // beyond head
    CHECK_EQ(ns_ring_used(&r), 4);
    ns_ring_ack(&r, 2);
    ns_ring_ack(&r, 1);            // before tail
    CHECK_EQ(ns_ring_used(&r), 2);
}

void suite_ring(void)
{
    ring_write_peek_ack();
    ring_full_keeps_oldest();
    ring_write_larger_than_ring_drops();
    ring_wraps_index();
    ring_offsets_wrap();
    ring_ack_out_of_range_noop();
}

static void ring_init_rejects_non_pow2(void)
{
    uint8_t buf[12];
    ns_ring_t r;
    CHECK(!ns_ring_init(&r, buf, 12));
    CHECK(ns_ring_init(&r, buf, 8));
}

static void ring_offsets_wrap_large_ring(void)
{
    // A 4096-byte ring (power of two) across the 2^32 wrap with a write straddling it.
    static uint8_t buf[4096], in[3000], out[3000];
    ns_ring_t r;
    ns_ring_init(&r, buf, sizeof buf);
    r.head_off = r.tail_off = 0xFFFFFC00u;
    for (int i = 0; i < 3000; i++) in[i] = (uint8_t)(i * 7);
    CHECK(ns_ring_write(&r, in, 3000));
    CHECK_EQ(ns_ring_peek(&r, 0xFFFFFC00u, out, sizeof out), 3000);
    CHECK(memcmp(in, out, 3000) == 0);
}

static void line_fit_keeps_newline(void)
{
    char line[8];
    // vsnprintf reported 20 chars for "0123456789abcdefghi\n" into an 8-byte buffer: 7 kept.
    memcpy(line, "0123456", 8);
    CHECK_EQ(ns_line_fit(line, 20, sizeof line), 7);
    CHECK_EQ(line[6], '\n');
    memcpy(line, "ab\n", 4);
    CHECK_EQ(ns_line_fit(line, 3, sizeof line), 3);
    CHECK_EQ(ns_line_fit(line, -1, sizeof line), 0);
}

void suite_ring_fixes(void)
{
    ring_init_rejects_non_pow2();
    ring_offsets_wrap_large_ring();
    line_fit_keeps_newline();
}
