// netserial ESP-IDF glue: the esp_log hook copies each formatted line into the ring under a spinlock
// and returns (it never blocks, allocates or touches lwIP, so a log call from the tcpip or Wi-Fi task
// cannot recurse into the stack). A low-priority task owns the socket: it reads HELLO/ACK from the
// host and sends what ns_tx decides, at CONFIG_NETSERIAL_FLUSH_MS or at once after an error line.
#include "netserial.h"
#include "ns_frame.h"
#include "ns_ring.h"
#include "ns_tx.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_BYTES 192   // longer lines are truncated, as a line on a 192-column console would be

static uint8_t s_buf[CONFIG_NETSERIAL_RING_SIZE];
static ns_ring_t s_ring;
static ns_tx_t s_tx;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static netserial_filter_t s_filter;
static TaskHandle_t s_task;
static bool s_started;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int ns_vprintf(const char *fmt, va_list ap)
{
    char line[LINE_MAX_BYTES];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (n > 0 && (!s_filter || s_filter(line, n))) {
        taskENTER_CRITICAL(&s_mux);
        ns_ring_write(&s_ring, line, (uint32_t)n);
        taskEXIT_CRITICAL(&s_mux);
        if (line[0] == 'E' && line[1] == ' ' && s_task) xTaskNotifyGive(s_task);   // send errors now
    }
    return s_prev ? s_prev(fmt, ap) : n;
}

// Read every pending HELLO/ACK. HELLO adopts the sender as the peer and resends from the oldest held
// byte; an ACK counts only from that peer and for this boot.
static void drain_rx(int fd, struct sockaddr_in *peer, bool *have_peer)
{
    uint8_t in[64];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(fd, in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        ns_hdr_t h;
        const uint8_t *p;
        if (!ns_frame_decode(in, (uint32_t)n, &h, &p)) continue;
        if (h.type == NS_HELLO) {
            *peer = from;
            *have_peer = true;
            taskENTER_CRITICAL(&s_mux);
            s_tx.send_off = s_ring.tail_off;
            taskEXIT_CRITICAL(&s_mux);
        } else if (h.type == NS_ACK && *have_peer && h.boot_id == s_tx.boot_id &&
                   from.sin_addr.s_addr == peer->sin_addr.s_addr && from.sin_port == peer->sin_port) {
            taskENTER_CRITICAL(&s_mux);
            ns_tx_on_ack(&s_tx, h.seq, now_ms());
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

static void ns_task(void *arg)
{
    (void)arg;
    static uint8_t out[NS_FRAME_MAX];
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(CONFIG_NETSERIAL_PORT) };
    bool have_peer = CONFIG_NETSERIAL_DEFAULT_HOST[0] && inet_aton(CONFIG_NETSERIAL_DEFAULT_HOST,
                                                                   &peer.sin_addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(CONFIG_NETSERIAL_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&me, sizeof me) != 0) {
        // Nothing can be sent; keep the hook (output is held) and stop here rather than spin.
        vTaskDelete(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_NETSERIAL_FLUSH_MS));
        drain_rx(fd, &peer, &have_peer);
        while (have_peer) {
            taskENTER_CRITICAL(&s_mux);
            uint32_t n = ns_tx_next(&s_tx, now_ms(), out, sizeof out);
            taskEXIT_CRITICAL(&s_mux);
            if (n == 0) break;
            bool ok = sendto(fd, out, n, 0, (struct sockaddr *)&peer, sizeof peer) == (int)n;
            taskENTER_CRITICAL(&s_mux);
            ns_tx_sent(&s_tx, n, ok);
            taskEXIT_CRITICAL(&s_mux);
            if (!ok) break;   // link down: everything stays in the ring for the next pass
        }
    }
}

esp_err_t netserial_start(netserial_filter_t filter)
{
    if (s_started) return ESP_OK;
    ns_ring_init(&s_ring, s_buf, sizeof s_buf);
    ns_tx_init(&s_tx, &s_ring, esp_random(), CONFIG_NETSERIAL_ACK_MODE, CONFIG_NETSERIAL_RESEND_MS);
    s_filter = filter;
    if (xTaskCreate(ns_task, "netserial", 3072, NULL, 2, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    s_started = true;
    s_prev = esp_log_set_vprintf(ns_vprintf);
    return ESP_OK;
}

void netserial_stats(uint32_t *used, uint32_t *dropped)
{
    taskENTER_CRITICAL(&s_mux);
    if (used) *used = ns_ring_used(&s_ring);
    if (dropped) *dropped = s_ring.dropped;
    taskEXIT_CRITICAL(&s_mux);
}
