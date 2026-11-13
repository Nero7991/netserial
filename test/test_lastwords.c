#include "ns_lastwords.h"
#include "ns_test.h"
#include <string.h>

// Concatenate the replay segments into out; returns the total length.
static uint32_t replay(const ns_lw_t *lw, char *out)
{
    const uint8_t *p1, *p2;
    uint32_t n1, n2;
    ns_lw_segments(lw, &p1, &n1, &p2, &n2);
    memcpy(out, p1, n1);
    memcpy(out + n1, p2, n2);
    out[n1 + n2] = 0;
    return n1 + n2;
}

static void lw_fresh_is_valid_and_empty(void)
{
    uint8_t buf[32];
    ns_lw_t lw;
    char out[64];
    ns_lw_init(&lw, buf, sizeof buf);
    CHECK(ns_lw_valid(&lw, buf, sizeof buf));
    CHECK_EQ(replay(&lw, out), 0);
}

static void lw_garbage_is_invalid(void)
{
    uint8_t buf[32];
    ns_lw_t lw;
    memset(&lw, 0xA5, sizeof lw);   // what uninitialised RAM looks like after a power-on
    CHECK(!ns_lw_valid(&lw, buf, sizeof buf));
    ns_lw_init(&lw, buf, sizeof buf);
    CHECK(!ns_lw_valid(&lw, buf, 64));   // size changed between builds
    uint8_t other[32];
    CHECK(!ns_lw_valid(&lw, other, sizeof other));   // buffer moved between builds
    lw.head += 1;   // header torn mid-update
    CHECK(!ns_lw_valid(&lw, buf, sizeof buf));
}

static void lw_no_wrap_replays_everything(void)
{
    uint8_t buf[32];
    ns_lw_t lw;
    char out[64];
    ns_lw_init(&lw, buf, sizeof buf);
    ns_lw_write(&lw, "one\n", 4);
    ns_lw_write(&lw, "two\n", 4);
    CHECK(ns_lw_valid(&lw, buf, sizeof buf));
    CHECK_EQ(replay(&lw, out), 8);
    CHECK(strcmp(out, "one\ntwo\n") == 0);
}

static void lw_wrap_keeps_newest_whole_lines(void)
{
    uint8_t buf[16];
    ns_lw_t lw;
    char out[64];
    ns_lw_init(&lw, buf, sizeof buf);
    ns_lw_write(&lw, "aaaaaa\n", 7);
    ns_lw_write(&lw, "bbbbbb\n", 7);
    ns_lw_write(&lw, "cccc\n", 5);   // 19 bytes into 16: the start of "aaaaaa" is overwritten
    CHECK(ns_lw_valid(&lw, buf, sizeof buf));
    uint32_t n = replay(&lw, out);
    CHECK(strcmp(out, "bbbbbb\ncccc\n") == 0);   // the partial first line is skipped
    CHECK_EQ(n, 12);
}

static void lw_line_longer_than_buffer(void)
{
    uint8_t buf[8];
    ns_lw_t lw;
    char out[64];
    ns_lw_init(&lw, buf, sizeof buf);
    ns_lw_write(&lw, "0123456789ABCDEF\n", 17);   // only the tail fits
    CHECK(ns_lw_valid(&lw, buf, sizeof buf));
    uint32_t n = replay(&lw, out);
    CHECK_EQ(n, 0);   // no complete line survives: nothing misleading is replayed
    ns_lw_write(&lw, "xy\n", 3);
    n = replay(&lw, out);
    CHECK(strcmp(out, "xy\n") == 0);
}

static void lw_exact_fill_and_trailing_partial(void)
{
    uint8_t buf[8];
    ns_lw_t lw;
    char out[64];
    ns_lw_init(&lw, buf, sizeof buf);
    ns_lw_write(&lw, "abc\ndef\n", 8);   // exactly full, not wrapped
    CHECK_EQ(replay(&lw, out), 8);
    CHECK(strcmp(out, "abc\ndef\n") == 0);
    ns_lw_write(&lw, "gh", 2);   // a line cut off by the crash: kept, it is the last word
    replay(&lw, out);
    CHECK(strcmp(out, "def\ngh") == 0);
}

void suite_lastwords(void)
{
    lw_fresh_is_valid_and_empty();
    lw_garbage_is_invalid();
    lw_no_wrap_replays_everything();
    lw_wrap_keeps_newest_whole_lines();
    lw_line_longer_than_buffer();
    lw_exact_fill_and_trailing_partial();
}
