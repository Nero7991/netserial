// Fixes from the 2026-11-13 hardware test (docs/debugging/2026-11-13_netserial-hil-findings.md).
#include "ns_frame.h"
#include "ns_line.h"
#include "ns_ring.h"
#include "ns_test.h"
#include "ns_tx.h"
#include <string.h>

static uint8_t s_buf[256];
static ns_ring_t s_r;
static ns_tx_t s_t;
static uint8_t s_out[NS_FRAME_MAX];

static void setup(void)
{
    ns_ring_init(&s_r, s_buf, sizeof s_buf);
    ns_tx_init(&s_t, &s_r, 0xB00Bu, true, 2000);
}

static ns_hdr_t decode(uint32_t n)
{
    ns_hdr_t h = {0};
    const uint8_t *p;
    CHECK(ns_frame_decode(s_out, n, &h, &p));
    return h;
}

static int is_err(const char *s) { return ns_line_is_error(s, (int)strlen(s)); }

static void line_error_detection(void)
{
    CHECK(is_err("E (123) TAG: boom\n"));
    CHECK(is_err("\033[0;31mE (123) TAG: boom\033[0m\n"));     // CONFIG_LOG_COLORS
    CHECK(!is_err("\033[0;32mI (123) TAG: fine\033[0m\n"));
    CHECK(!is_err("W (1) TAG: warn\n"));
    CHECK(!is_err("Error without the log prefix\n"));
    CHECK(!is_err("E"));
    CHECK(!is_err("\033[0;31m"));                               // escape only
    CHECK(!is_err("\033[0;31"));                                // unterminated escape
    CHECK(!is_err(""));
}

static void ring_half_full_crossing(void)
{
    setup();
    CHECK(!ns_ring_crossed_half(&s_r, 0, 100));
    CHECK(ns_ring_crossed_half(&s_r, 100, 128));                // 256 / 2 reached
    CHECK(!ns_ring_crossed_half(&s_r, 130, 200));               // already above: no repeat
}

static void tx_awaiting_ack(void)
{
    setup();
    CHECK(!ns_tx_awaiting_ack(&s_t));
    ns_ring_write(&s_r, "abc", 3);
    CHECK(!ns_tx_awaiting_ack(&s_t));                           // held but not sent yet
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    CHECK(ns_tx_awaiting_ack(&s_t));
    ns_tx_on_ack(&s_t, 3, 10);
    CHECK(!ns_tx_awaiting_ack(&s_t));
    // A device that gave up on its host is not waiting for an ACK (so it does not poll forever).
    ns_ring_write(&s_r, "def", 3);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 20, s_out, sizeof s_out), true);
    uint32_t now = 20;
    for (int i = 0; i < NS_TX_MAX_RESENDS + 2; i++) {
        now += 2001;
        uint32_t n = ns_tx_next(&s_t, now, s_out, sizeof s_out);
        if (n) ns_tx_sent(&s_t, n, true);
    }
    CHECK(!ns_tx_awaiting_ack(&s_t));
}

static void tx_sync_after_hello(void)
{
    setup();
    ns_ring_write(&s_r, "0123456789", 10);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    ns_tx_on_ack(&s_t, 4, 5);                                   // tail now 4
    ns_tx_rewind(&s_t, 10);                                     // HELLO
    uint32_t n = ns_tx_next(&s_t, 10, s_out, sizeof s_out);
    ns_hdr_t h = decode(n);
    CHECK_EQ(h.type, NS_SYNC);
    CHECK_EQ(h.seq, 4);                                         // oldest byte the device holds
    CHECK_EQ(h.len, 0);
    ns_tx_sent(&s_t, n, true);
    h = decode(ns_tx_next(&s_t, 10, s_out, sizeof s_out));
    CHECK_EQ(h.type, NS_DATA);
    CHECK_EQ(h.seq, 4);
    CHECK_EQ(h.len, 6);
}

static void tx_sync_retried_when_send_fails(void)
{
    setup();
    ns_tx_rewind(&s_t, 0);
    uint32_t n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    CHECK_EQ(decode(n).type, NS_SYNC);                          // even with nothing held
    ns_tx_sent(&s_t, n, false);
    n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    CHECK_EQ(decode(n).type, NS_SYNC);
    ns_tx_sent(&s_t, n, true);
    CHECK_EQ(ns_tx_next(&s_t, 0, s_out, sizeof s_out), 0);
}

static void tx_no_sync_without_hello(void)
{
    setup();
    ns_ring_write(&s_r, "ab", 2);
    CHECK_EQ(decode(ns_tx_next(&s_t, 0, s_out, sizeof s_out)).type, NS_DATA);   // default host
}

static void frame_bye_roundtrip(void)
{
    uint8_t p[6];
    ns_bye_payload(0xC0A861F0u /* 192.168.97.240 */, 40000, p);
    CHECK_EQ(p[0], 192);
    CHECK_EQ(p[3], 240);
    CHECK_EQ(p[4] << 8 | p[5], 40000);
}

void suite_hil_fixes(void)
{
    line_error_detection();
    ring_half_full_crossing();
    tx_awaiting_ack();
    tx_sync_after_hello();
    tx_sync_retried_when_send_fails();
    tx_no_sync_without_hello();
    frame_bye_roundtrip();
}
