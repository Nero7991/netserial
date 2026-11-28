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

// The replay must fit the ring it is written into (Codex): drop the oldest bytes, then start
// on a whole line.
static uint32_t trim_join(const char *a, const char *b, uint32_t max, char *out)
{
    const uint8_t *p1 = (const uint8_t *)a, *p2 = (const uint8_t *)b;
    uint32_t n1 = (uint32_t)strlen(a), n2 = (uint32_t)strlen(b);
    ns_lw_trim(&p1, &n1, &p2, &n2, max);
    memcpy(out, p1, n1);
    memcpy(out + n1, p2, n2);
    out[n1 + n2] = 0;
    return n1 + n2;
}

static void lw_trim_fits_newest_whole_lines(void)
{
    char out[64];
    CHECK_EQ(trim_join("aa\nbb\n", "cc\ndd\n", 12, out), 12);   // fits: unchanged
    CHECK(strcmp(out, "aa\nbb\ncc\ndd\n") == 0);
    CHECK_EQ(trim_join("aa\nbb\n", "cc\ndd\n", 7, out), 6);    // cut inside the first segment
    CHECK(strcmp(out, "cc\ndd\n") == 0);
    CHECK_EQ(trim_join("aa\nbb\n", "cc\ndd\n", 6, out), 6);    // cut exactly at the boundary
    CHECK(strcmp(out, "cc\ndd\n") == 0);
    CHECK_EQ(trim_join("aa\nbb", "\ncc\n", 4, out), 3);         // boundary inside a line
    CHECK(strcmp(out, "cc\n") == 0);
    CHECK_EQ(trim_join("aa\nbb\n", "cc\ndd\n", 4, out), 3);    // cut inside the second
    CHECK(strcmp(out, "dd\n") == 0);
    CHECK_EQ(trim_join("abcdefgh", "", 3, out), 3);              // one long line: keep its tail
    CHECK(strcmp(out, "fgh") == 0);
    CHECK_EQ(trim_join("aa\n", "bb\n", 0, out), 0);
}

void suite_lastwords(void)
{
    lw_fresh_is_valid_and_empty();
    lw_garbage_is_invalid();
    lw_no_wrap_replays_everything();
    lw_wrap_keeps_newest_whole_lines();
    lw_line_longer_than_buffer();
    lw_exact_fill_and_trailing_partial();
    lw_trim_fits_newest_whole_lines();
}
