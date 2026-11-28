// netserial ESP-IDF glue: the esp_log hook copies each formatted line into the ring under a spinlock
// and returns (it never blocks, allocates or touches lwIP, so a log call from the tcpip or Wi-Fi task
// cannot recurse into the stack). A low-priority task owns the socket: it reads HELLO/ACK from the
// host and sends what ns_tx decides, at CONFIG_NETSERIAL_FLUSH_MS, or at once after an error line or
// when the ring reaches half full. While sent bytes await their ACK it listens on the socket, so the
// ACK frees the ring within a round trip instead of at the next flush.
#include "netserial.h"
#include "ns_line.h"
#include "ns_frame.h"
#include "ns_ring.h"
#include "ns_tx.h"
#include "ns_lastwords.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <sys/select.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_BYTES 192   // longer lines are truncated, as a line on a 192-column console would be
#define LINE_POOL 4          // static line buffers: logging tasks do not carry LINE_MAX_BYTES on their stack
#define ACK_POLL_MS 50       // while awaiting an ACK: how often a hook notification is noticed

_Static_assert((CONFIG_NETSERIAL_RING_SIZE & (CONFIG_NETSERIAL_RING_SIZE - 1)) == 0,
               "CONFIG_NETSERIAL_RING_SIZE must be a power of two");

static uint8_t s_buf[CONFIG_NETSERIAL_RING_SIZE];
static ns_ring_t s_ring;
static ns_tx_t s_tx;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;
static netserial_filter_t s_filter;
static TaskHandle_t s_task;
static bool s_started;
static char s_pool[LINE_POOL][LINE_MAX_BYTES];
static uint8_t s_pool_busy;   // bit i: s_pool[i] is in use (guarded by s_mux)

#if CONFIG_NETSERIAL_LAST_WORDS_SIZE > 0
// Survives software, panic and watchdog resets; random after power-on (ns_lw_valid rejects that).
static __NOINIT_ATTR uint8_t s_lw_buf[CONFIG_NETSERIAL_LAST_WORDS_SIZE];
static __NOINIT_ATTR ns_lw_t s_lw;
// .bss, so false at every boot until replay_last_words() has initialized s_lw: a panic before
// netserial_start() must not write through a random .noinit header (Codex).
static bool s_lw_ready;
#endif

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#if CONFIG_NETSERIAL_LAST_WORDS_SIZE > 0
// All of this path is cache-safe (IRAM_ATTR here, ns_lastwords via linker.lf, data in DRAM):
// with CONFIG_ESP_PANIC_HANDLER_IRAM a panic prints with the flash cache disabled.
// The panic handler does not go through esp_log: it prints with panic_print_* straight to the
// console. These link-time wrappers (CMakeLists: -Wl,--wrap) copy that output into the
// last-words recorder too, so a crash's reason and backtrace are replayed over the network on
// the next boot. The real function runs first: if the recorder faults (a panic with the cache
// disabled), the console still has the output.
void __real_panic_print_char(const char c);
void __real_panic_print_str(const char *str);
void __real_panic_print_hex(int h);
void __real_panic_print_dec(int d);

// panic.c prints its own reason lines through same-file calls, which the link-time wrap does
// not see; the most common reason worth knowing remotely is a stack overflow, so record the
// task name here (overrides FreeRTOS's weak hook, then aborts exactly as it does).
IRAM_ATTR void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    static DRAM_ATTR const char a[] = "***ERROR*** A stack overflow in task ";
    static DRAM_ATTR const char b[] = " has been detected.\n";
    char msg[sizeof a + configMAX_TASK_NAME_LEN + sizeof b];
    size_t n = 0;
    for (size_t i = 0; a[i]; i++) msg[n++] = a[i];
    for (size_t i = 0; name && name[i] && i < configMAX_TASK_NAME_LEN; i++) msg[n++] = name[i];
    for (size_t i = 0; b[i]; i++) msg[n++] = b[i];
    if (s_lw_ready) ns_lw_write(&s_lw, msg, (uint32_t)n);
    msg[n - 1] = '\0';   // esp_system_abort prints its own newline
    esp_system_abort(msg);
}

IRAM_ATTR void __wrap_panic_print_char(const char c)
{
    __real_panic_print_char(c);
    if (s_lw_ready) ns_lw_write(&s_lw, &c, 1);
}

IRAM_ATTR void __wrap_panic_print_str(const char *str)
{
    __real_panic_print_str(str);   // calls the real panic_print_char (same file): no double copy
    if (!s_lw_ready) return;
    size_t n = 0;
    while (str[n]) n++;
    ns_lw_write(&s_lw, str, (uint32_t)n);
}

IRAM_ATTR void __wrap_panic_print_hex(int h)
{
    __real_panic_print_hex(h);
    if (!s_lw_ready) return;
    char b[8];
    for (int i = 0; i < 8; i++) {
        int c = (int)(((unsigned)h >> (28 - 4 * i)) & 0xf);
        b[i] = (char)(c < 10 ? '0' + c : 'a' + c - 10);
    }
    ns_lw_write(&s_lw, b, sizeof b);
}

IRAM_ATTR void __wrap_panic_print_dec(int d)
{
    __real_panic_print_dec(d);
    if (!s_lw_ready) return;
    char b[2] = { d / 10 ? (char)('0' + d / 10 % 10) : ' ', (char)('0' + d % 10) };
    ns_lw_write(&s_lw, b, sizeof b);
}
#endif

// Copy one formatted line into the ring (and the last-words recorder), and wake the sender task for
// an error line or when the ring has just reached half full.
static void ns_put_line(char *line, int n)
{
    n = ns_line_fit(line, n, LINE_MAX_BYTES);
    if (n <= 0 || (s_filter && !s_filter(line, n))) return;
    taskENTER_CRITICAL(&s_mux);
    uint32_t before = ns_ring_used(&s_ring);
    ns_ring_write(&s_ring, line, (uint32_t)n);
    bool half = ns_ring_crossed_half(&s_ring, before, ns_ring_used(&s_ring));
#if CONFIG_NETSERIAL_LAST_WORDS_SIZE > 0
    ns_lw_write(&s_lw, line, (uint32_t)n);   // kept even when the ring is full and dropping
#endif
    taskEXIT_CRITICAL(&s_mux);
    if ((half || ns_line_is_error(line, n)) && s_task) xTaskNotifyGive(s_task);   // send now
}

// Every pooled buffer is in use (logging tasks preempted mid-line): format on this task's stack.
// Kept out of line so the common path does not reserve LINE_MAX_BYTES of stack.
static int __attribute__((noinline)) ns_vprintf_stack(const char *fmt, va_list ap)
{
    char line[LINE_MAX_BYTES];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    ns_put_line(line, n);
    return s_prev ? s_prev(fmt, ap) : n;
}

static int ns_vprintf(const char *fmt, va_list ap)
{
    int slot = -1;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LINE_POOL; i++) {
        if (!(s_pool_busy & (1u << i))) {
            s_pool_busy |= (uint8_t)(1u << i);
            slot = i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (slot < 0) return ns_vprintf_stack(fmt, ap);

    char *line = s_pool[slot];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, LINE_MAX_BYTES, fmt, ap2);
    va_end(ap2);
    int full = n;   // untruncated length, before ns_put_line may cut the line
    ns_put_line(line, n);
    int ret;
    if (s_prev == vprintf && full > 0 && full < LINE_MAX_BYTES) {
        ret = (int)fwrite(line, 1, (size_t)full, stdout);   // already formatted: do not format again
    } else {
        ret = s_prev ? s_prev(fmt, ap) : n;
    }
    taskENTER_CRITICAL(&s_mux);
    s_pool_busy &= (uint8_t)~(1u << slot);
    taskEXIT_CRITICAL(&s_mux);
    return ret;
}

// Tell a host that another host took the stream (it then stops sending HELLO; see host/netserial.py).
static void send_bye(int fd, const struct sockaddr_in *to, const struct sockaddr_in *new_peer)
{
    static uint8_t frame[NS_FRAME_HDR + 6];
    uint8_t p[6];
    ns_bye_payload(ntohl(new_peer->sin_addr.s_addr), ntohs(new_peer->sin_port), p);
    ns_hdr_t h = { .type = NS_BYE, .boot_id = s_tx.boot_id, .dev_ms = now_ms(), .len = sizeof p };
    uint32_t n = ns_frame_encode(&h, p, frame, sizeof frame);
    if (n) sendto(fd, frame, n, 0, (const struct sockaddr *)to, sizeof *to);
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// Read every pending HELLO/ACK. HELLO adopts the sender as the peer (telling the previous peer with
// BYE) and resends from the oldest held byte, after a SYNC; an ACK counts only from that peer and for
// this boot. An ACK from anyone else gets a BYE: that host missed the first one. Returns true if a
// HELLO arrived (its SYNC reply should go out now).
static bool drain_rx(int fd, struct sockaddr_in *peer, bool *have_peer)
{
    uint8_t in[64];
    bool hello = false;
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(fd, in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) return hello;
        ns_hdr_t h;
        const uint8_t *p;
        if (!ns_frame_decode(in, (uint32_t)n, &h, &p)) continue;
        if (h.type == NS_HELLO) {
            if (*have_peer && !same_addr(peer, &from)) send_bye(fd, peer, &from);
            *peer = from;
            *have_peer = true;
            hello = true;
            taskENTER_CRITICAL(&s_mux);
            ns_tx_rewind(&s_tx, now_ms());
            taskEXIT_CRITICAL(&s_mux);
        } else if (h.type == NS_ACK && *have_peer && !same_addr(peer, &from)) {
            send_bye(fd, &from, peer);
        } else if (h.type == NS_ACK && *have_peer && h.boot_id == s_tx.boot_id) {
            taskENTER_CRITICAL(&s_mux);
            ns_tx_on_ack(&s_tx, h.seq, now_ms());
            taskEXIT_CRITICAL(&s_mux);
        }
    }
}

// Wait up to ms for a datagram on fd.
static void wait_readable(int fd, uint32_t ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    select(fd + 1, &rf, NULL, NULL, &tv);
}

static void send_pending(int fd, const struct sockaddr_in *peer)
{
    static uint8_t out[NS_FRAME_MAX];
    for (;;) {
        taskENTER_CRITICAL(&s_mux);
        uint32_t n = ns_tx_next(&s_tx, now_ms(), out, sizeof out);
        taskEXIT_CRITICAL(&s_mux);
        if (n == 0) return;
        bool ok = sendto(fd, out, n, 0, (const struct sockaddr *)peer, sizeof *peer) == (int)n;
        taskENTER_CRITICAL(&s_mux);
        ns_tx_sent(&s_tx, n, ok);
        taskEXIT_CRITICAL(&s_mux);
        if (!ok) return;   // link down: everything stays in the ring for the next pass
    }
}

static void ns_task(void *arg)
{
    (void)arg;
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(CONFIG_NETSERIAL_PORT) };
    bool have_peer = CONFIG_NETSERIAL_DEFAULT_HOST[0] && inet_aton(CONFIG_NETSERIAL_DEFAULT_HOST,
                                                                   &peer.sin_addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(CONFIG_NETSERIAL_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&me, sizeof me) != 0) {
        // Nothing can be sent (for example the port is taken). Keep the hook, which then only fills
        // the ring, and stop here rather than spin. Clear s_task first: the hook notifies it.
        s_task = NULL;
        if (fd >= 0) close(fd);
        vTaskDelete(NULL);
    }
    uint32_t next_flush = now_ms() + CONFIG_NETSERIAL_FLUSH_MS;
    for (;;) {
        taskENTER_CRITICAL(&s_mux);
        bool awaiting = have_peer && ns_tx_awaiting_ack(&s_tx);
        taskEXIT_CRITICAL(&s_mux);
        int32_t left = ns_off_diff(next_flush, now_ms());
        uint32_t wait = left > 0 ? (uint32_t)left : 0;
        bool woken;
        if (awaiting) {
            // Listen for the ACK; a notification (error line, half full) is noticed within ACK_POLL_MS.
            wait_readable(fd, wait < ACK_POLL_MS ? wait : ACK_POLL_MS);
            woken = ulTaskNotifyTake(pdTRUE, 0) > 0;
        } else {
            woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)) > 0;
        }
        woken |= drain_rx(fd, &peer, &have_peer);
        // Send on the flush interval or when woken; an ACK alone only frees the ring (the radio
        // batches output at the flush interval).
        if (!woken && ns_off_diff(next_flush, now_ms()) > 0) continue;
        next_flush = now_ms() + CONFIG_NETSERIAL_FLUSH_MS;
        if (have_peer) send_pending(fd, &peer);
    }
}

#if CONFIG_NETSERIAL_LAST_WORDS_SIZE > 0
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "other";
    }
}

// Put the previous boot's newest lines (if its RAM survived) at the start of this boot's stream,
// then start recording afresh. Runs before the log hook is installed: no locking needed.
static void replay_last_words(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool keep = rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT && rr != ESP_RST_UNKNOWN &&
                ns_lw_valid(&s_lw, s_lw_buf, sizeof s_lw_buf);
    if (keep) {
        const uint8_t *p1, *p2;
        uint32_t n1, n2;
        ns_lw_segments(&s_lw, &p1, &n1, &p2, &n2);
        // The ring drops a write that does not fit whole, and nothing drains it yet: keep the
        // newest lines that fit after the markers (Codex).
        uint32_t room = s_ring.size - ns_ring_used(&s_ring);
        uint32_t markers = 112 + 1 + 48;
        ns_lw_trim(&p1, &n1, &p2, &n2, room > markers ? room - markers : 0);
        if (n1 + n2 > 0) {
            char hdr[112];
            int h = snprintf(hdr, sizeof hdr, "---- netserial: last %u bytes of the previous boot "
                             "(reset: %s) ----\n", (unsigned)(n1 + n2), reset_name(rr));
            ns_ring_write(&s_ring, hdr, (uint32_t)h);
            ns_ring_write(&s_ring, p1, n1);
            ns_ring_write(&s_ring, p2, n2);
            if ((n2 ? p2[n2 - 1] : p1[n1 - 1]) != '\n') ns_ring_write(&s_ring, "\n", 1);
            static const char end[] = "---- netserial: end of previous boot ----\n";
            ns_ring_write(&s_ring, end, sizeof end - 1);
        }
    }
    ns_lw_init(&s_lw, s_lw_buf, sizeof s_lw_buf);
    s_lw_ready = true;
}
#endif

esp_err_t netserial_start(netserial_filter_t filter)
{
    if (s_started) return ESP_OK;
    ns_ring_init(&s_ring, s_buf, sizeof s_buf);
#if CONFIG_NETSERIAL_LAST_WORDS_SIZE > 0
    replay_last_words();
#endif
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
