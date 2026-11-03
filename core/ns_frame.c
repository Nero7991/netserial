#include "ns_frame.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t ns_frame_encode(const ns_hdr_t *h, const uint8_t *payload, uint8_t *out, uint32_t cap)
{
    uint32_t total = NS_FRAME_HDR + (uint32_t)h->len;
    if (total > NS_FRAME_MAX || total > cap) return 0;
    out[0] = 'N';
    out[1] = 'S';
    out[2] = NS_FRAME_VER;
    out[3] = h->type;
    put32(out + 4, h->boot_id);
    put32(out + 8, h->seq);
    put32(out + 12, h->dev_ms);
    out[16] = (uint8_t)h->len;
    out[17] = (uint8_t)(h->len >> 8);
    if (h->len) memcpy(out + NS_FRAME_HDR, payload, h->len);
    return total;
}

bool ns_frame_decode(const uint8_t *in, uint32_t n, ns_hdr_t *h, const uint8_t **payload)
{
    if (n < NS_FRAME_HDR || in[0] != 'N' || in[1] != 'S' || in[2] != NS_FRAME_VER) return false;
    h->type = in[3];
    h->boot_id = get32(in + 4);
    h->seq = get32(in + 8);
    h->dev_ms = get32(in + 12);
    h->len = (uint16_t)(in[16] | in[17] << 8);
    if (n != NS_FRAME_HDR + (uint32_t)h->len) return false;
    *payload = in + NS_FRAME_HDR;
    return true;
}
