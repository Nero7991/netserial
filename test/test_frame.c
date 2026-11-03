#include "ns_frame.h"
#include "ns_test.h"
#include <string.h>

static void frame_roundtrip(void)
{
    uint8_t out[NS_FRAME_MAX];
    ns_hdr_t h = { .type = NS_DATA, .boot_id = 0xA1B2C3D4u, .seq = 1234, .dev_ms = 777, .len = 5 };
    uint32_t n = ns_frame_encode(&h, (const uint8_t *)"hello", out, sizeof out);
    CHECK_EQ(n, NS_FRAME_HDR + 5);
    ns_hdr_t d;
    const uint8_t *p = NULL;
    CHECK(ns_frame_decode(out, n, &d, &p));
    CHECK_EQ(d.type, NS_DATA);
    CHECK_EQ(d.boot_id, 0xA1B2C3D4u);
    CHECK_EQ(d.seq, 1234);
    CHECK_EQ(d.dev_ms, 777);
    CHECK_EQ(d.len, 5);
    CHECK(p && memcmp(p, "hello", 5) == 0);
}

static void frame_rejects_bad_magic(void)
{
    uint8_t out[NS_FRAME_MAX];
    ns_hdr_t h = { .type = NS_HELLO };
    uint32_t n = ns_frame_encode(&h, NULL, out, sizeof out);
    out[0] = 'X';
    ns_hdr_t d;
    const uint8_t *p;
    CHECK(!ns_frame_decode(out, n, &d, &p));
}

static void frame_rejects_len_mismatch(void)
{
    uint8_t out[NS_FRAME_MAX];
    ns_hdr_t h = { .type = NS_DATA, .len = 4 };
    uint32_t n = ns_frame_encode(&h, (const uint8_t *)"abcd", out, sizeof out);
    ns_hdr_t d;
    const uint8_t *p;
    CHECK(!ns_frame_decode(out, n - 1, &d, &p));
    CHECK(!ns_frame_decode(out, NS_FRAME_HDR - 1, &d, &p));
}

static void frame_encode_too_big_returns_0(void)
{
    static uint8_t payload[1383], out[2048];
    ns_hdr_t h = { .type = NS_DATA, .len = sizeof payload };
    CHECK_EQ(ns_frame_encode(&h, payload, out, sizeof out), 0);   // 18 + 1383 > NS_FRAME_MAX
    h.len = 1382;
    CHECK_EQ(ns_frame_encode(&h, payload, out, sizeof out), NS_FRAME_MAX);
    CHECK_EQ(ns_frame_encode(&h, payload, out, 1399), 0);         // fits the limit, not the buffer
}

static void frame_little_endian(void)
{
    uint8_t out[NS_FRAME_MAX];
    ns_hdr_t h = { .type = NS_ACK, .seq = 0x01020304u };
    ns_frame_encode(&h, NULL, out, sizeof out);
    CHECK_EQ(out[0], 'N');
    CHECK_EQ(out[1], 'S');
    CHECK_EQ(out[2], 1);
    CHECK_EQ(out[3], NS_ACK);
    CHECK_EQ(out[8], 0x04);
    CHECK_EQ(out[9], 0x03);
    CHECK_EQ(out[10], 0x02);
    CHECK_EQ(out[11], 0x01);
}

void suite_frame(void)
{
    frame_roundtrip();
    frame_rejects_bad_magic();
    frame_rejects_len_mismatch();
    frame_encode_too_big_returns_0();
    frame_little_endian();
}
