#include "ns_frame.h"
#include "ns_ring.h"
#include "ns_test.h"
#include "ns_tx.h"
#include <string.h>

static uint8_t s_buf[256];
static ns_ring_t s_r;
static ns_tx_t s_t;
static uint8_t s_out[NS_FRAME_MAX];

static void setup(bool ack_mode)
{
    ns_ring_init(&s_r, s_buf, sizeof s_buf);
    ns_tx_init(&s_t, &s_r, 0xB00Bu, ack_mode, 2000);
}

static ns_hdr_t decode(uint32_t n)
{
    ns_hdr_t h = {0};
    const uint8_t *p;
    CHECK(ns_frame_decode(s_out, n, &h, &p));
    return h;
}

static void tx_sends_data_then_waits_for_ack(void)
{
    setup(true);
    ns_ring_write(&s_r, "0123456789", 10);
    uint32_t n = ns_tx_next(&s_t, 100, s_out, sizeof s_out);
    ns_hdr_t h = decode(n);
    CHECK_EQ(h.type, NS_DATA);
    CHECK_EQ(h.seq, 0);
    CHECK_EQ(h.len, 10);
    CHECK_EQ(h.boot_id, 0xB00Bu);
    CHECK_EQ(h.dev_ms, 100);
    ns_tx_sent(&s_t, n, true);
    CHECK_EQ(ns_tx_next(&s_t, 200, s_out, sizeof s_out), 0);
    CHECK_EQ(ns_ring_used(&s_r), 10);
}

static void tx_ack_frees_ring(void)
{
    setup(true);
    ns_ring_write(&s_r, "0123456789", 10);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    ns_tx_on_ack(&s_t, 10, 50);
    CHECK_EQ(ns_ring_used(&s_r), 0);
    ns_ring_write(&s_r, "ab", 2);
    ns_hdr_t h = decode(ns_tx_next(&s_t, 60, s_out, sizeof s_out));
    CHECK_EQ(h.seq, 10);
    CHECK_EQ(h.len, 2);
}

static void tx_resends_after_timeout(void)
{
    setup(true);
    ns_ring_write(&s_r, "0123456789", 10);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    CHECK_EQ(ns_tx_next(&s_t, 1999, s_out, sizeof s_out), 0);
    uint32_t n = ns_tx_next(&s_t, 2001, s_out, sizeof s_out);
    ns_hdr_t h = decode(n);
    CHECK_EQ(h.seq, 0);
    CHECK_EQ(h.len, 10);
}

static void tx_send_failure_keeps_bytes(void)
{
    setup(true);
    ns_ring_write(&s_r, "abc", 3);
    uint32_t n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    ns_tx_sent(&s_t, n, false);
    ns_hdr_t h = decode(ns_tx_next(&s_t, 10, s_out, sizeof s_out));
    CHECK_EQ(h.seq, 0);
    CHECK_EQ(h.len, 3);
    CHECK_EQ(ns_ring_used(&s_r), 3);
}

static void tx_no_ack_mode_commits_on_send(void)
{
    setup(false);
    ns_ring_write(&s_r, "abc", 3);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    CHECK_EQ(ns_ring_used(&s_r), 0);
    CHECK_EQ(ns_tx_next(&s_t, 5000, s_out, sizeof s_out), 0);
}

static void tx_reports_dropped_once(void)
{
    setup(true);
    static uint8_t big[300];
    ns_ring_write(&s_r, big, sizeof big);   // > ring: dropped 300
    uint32_t n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    ns_hdr_t h = {0};
    const uint8_t *p = NULL;
    CHECK(ns_frame_decode(s_out, n, &h, &p));
    CHECK_EQ(h.type, NS_DROPPED);
    CHECK_EQ(h.len, 4);
    CHECK(p && p[0] == 44 && p[1] == 1);   // 300 little-endian
    ns_tx_sent(&s_t, n, true);
    CHECK_EQ(ns_tx_next(&s_t, 10, s_out, sizeof s_out), 0);
}

static void tx_splits_at_frame_max(void)
{
    static uint8_t buf[4096], big[3000];
    ns_ring_init(&s_r, buf, sizeof buf);
    ns_tx_init(&s_t, &s_r, 1, true, 2000);
    ns_ring_write(&s_r, big, sizeof big);
    uint32_t n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    CHECK_EQ(n, NS_FRAME_MAX);
    ns_tx_sent(&s_t, n, true);
    ns_hdr_t h = decode(ns_tx_next(&s_t, 0, s_out, sizeof s_out));
    CHECK_EQ(h.seq, NS_FRAME_MAX - NS_FRAME_HDR);
}

static void tx_ack_out_of_range_ignored(void)
{
    setup(true);
    ns_ring_write(&s_r, "0123456789", 10);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    ns_tx_on_ack(&s_t, 50, 10);                 // beyond what was sent
    CHECK_EQ(ns_ring_used(&s_r), 10);
    ns_tx_on_ack(&s_t, 4, 10);
    ns_tx_on_ack(&s_t, 2, 20);                  // before tail now
    CHECK_EQ(ns_ring_used(&s_r), 6);
    ns_tx_on_ack(&s_t, 0xFFFFFF00u, 30);        // far in the past
    CHECK_EQ(ns_ring_used(&s_r), 6);
}

void suite_tx(void)
{
    tx_sends_data_then_waits_for_ack();
    tx_ack_frees_ring();
    tx_resends_after_timeout();
    tx_send_failure_keeps_bytes();
    tx_no_ack_mode_commits_on_send();
    tx_reports_dropped_once();
    tx_splits_at_frame_max();
    tx_ack_out_of_range_ignored();
}

static void tx_resend_gives_up_without_host(void)
{
    setup(true);
    ns_ring_write(&s_r, "abc", 3);
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);
    uint32_t now = 0;
    int resends = 0;
    for (int i = 0; i < 20; i++) {
        now += 2001;
        uint32_t n = ns_tx_next(&s_t, now, s_out, sizeof s_out);
        if (n) { resends++; ns_tx_sent(&s_t, n, true); }
    }
    CHECK_EQ(resends, NS_TX_MAX_RESENDS);
    ns_tx_rewind(&s_t, now);                   // a HELLO: start again from the tail
    uint32_t n = ns_tx_next(&s_t, now, s_out, sizeof s_out);
    CHECK_EQ(decode(n).type, NS_SYNC);         // the reply to HELLO comes first
    ns_tx_sent(&s_t, n, true);
    ns_hdr_t h = decode(ns_tx_next(&s_t, now, s_out, sizeof s_out));
    CHECK_EQ(h.seq, 0);
    CHECK_EQ(h.len, 3);
}

static void tx_dropped_resent_on_resend_cycle(void)
{
    setup(true);
    static uint8_t big[300];
    ns_ring_write(&s_r, "abc", 3);
    ns_ring_write(&s_r, big, sizeof big);      // dropped 300
    uint32_t n = ns_tx_next(&s_t, 0, s_out, sizeof s_out);
    CHECK_EQ(decode(n).type, NS_DROPPED);
    ns_tx_sent(&s_t, n, true);                 // "sent", but suppose the datagram was lost
    ns_tx_sent(&s_t, ns_tx_next(&s_t, 0, s_out, sizeof s_out), true);   // DATA abc
    n = ns_tx_next(&s_t, 2001, s_out, sizeof s_out);                     // no ACK: resend cycle
    CHECK_EQ(decode(n).type, NS_DROPPED);
}

void suite_tx_fixes(void)
{
    tx_resend_gives_up_without_host();
    tx_dropped_resent_on_resend_cycle();
}
