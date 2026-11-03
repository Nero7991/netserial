# netserial v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Output-only serial console over UDP for ESP-IDF devices: lossless while the ring has room, visible loss otherwise, with a host tool that logs and exposes a PTY.

**Architecture:** A portable C99 core (`core/`: ring, frame codec, transmit state machine) with host unit tests, wrapped by an ESP-IDF component (`netserial.c`: log hook, task, UDP socket). A stdlib-only Python host tool speaks the same frames.

**Tech Stack:** C99, ESP-IDF v5.5.6 (esp32c6 first), CMake host tests, Python 3.10 + pytest.

**Spec:** `docs/superpowers/specs/2026-11-02-netserial-design.md`

## Global Constraints

- The log hook never blocks, allocates, or calls lwIP. It copies into the ring under a spinlock and returns.
- Bytes leave the ring only when ACKed (ACK mode) or when sendto() succeeded (ACK mode off).
- Ring full: keep the oldest, drop new output whole-call, count dropped bytes.
- No dynamic allocation after `netserial_start()`.
- Wire: datagram <= 1400 bytes; header `magic "NS" | ver u8 | type u8 | boot_id u32 | seq u32 | dev_ms u32 | len u16`, little-endian, 16 bytes; `seq` = byte offset of the payload in the output stream since boot. Types: DATA=1, DROPPED=2, HELLO=3, ACK=4 (INPUT=5 reserved, not built).
- Defaults: ring 8192 B, flush_ms 1000, resend_ms 2000, UDP port 5514, task stack 3072.
- Output-only in v1. No emojis, no em-dashes in code or docs.
- Commit identity `-c user.name="Oren" -c user.email="orencollaco97@gmail.com"`; stage explicit paths only.

## Review Focus

1. u32 stream offsets wrapping past 2^32: comparisons must be wrap-safe (`(int32_t)(a - b)`). Task 1 test `ring_offsets_wrap`.
2. Host tool restarted mid-stream: it has no base offset; the first DATA it sees sets the base and it marks "joined at offset N". Task 5 test `test_join_mid_stream`.
3. Device reboot while the host runs: boot_id changes, host writes a REBOOT marker and resets its offset state. Task 5 test `test_reboot_marker`.
4. A single write larger than the ring: dropped whole and counted, ring untouched. Task 1 test `ring_write_larger_than_ring_drops`.
5. Stale or future ACK (ack < tail or ack > sent): ignored, no crash, no tail jump past head. Task 3 test `tx_ack_out_of_range_ignored`.

---

### Task 1: Repo scaffold and the ring

**Files:**
- Create: `CMakeLists.txt` (ESP-IDF component), `core/ns_ring.h`, `core/ns_ring.c`, `test/CMakeLists.txt`, `test/test_main.c`, `test/test_ring.c`, `README.md`, `LICENSE` (MIT, my call), `.gitignore`

**Interfaces:**
- Produces:
  - `typedef struct { uint8_t *buf; uint32_t size; uint32_t head_off; uint32_t tail_off; uint32_t dropped; } ns_ring_t;` (offsets are stream offsets; index = off % size)
  - `void ns_ring_init(ns_ring_t *r, uint8_t *buf, uint32_t size);`
  - `bool ns_ring_write(ns_ring_t *r, const void *p, uint32_t n);` all or nothing; false adds n to `dropped`
  - `uint32_t ns_ring_peek(const ns_ring_t *r, uint32_t from_off, uint8_t *out, uint32_t max);` copies from `from_off` (clamped to [tail, head]); returns bytes copied
  - `void ns_ring_ack(ns_ring_t *r, uint32_t off);` advances tail to `off` if tail <= off <= head (wrap-safe), else no-op
  - `uint32_t ns_ring_used(const ns_ring_t *r);`
  - Locking is the caller's job (the ESP glue holds a spinlock).

- [ ] **Step 1: Write failing tests** in `test/test_ring.c`, a minimal assert runner in `test_main.c` (suite names on argv like quickvolt's runner):
  `ring_write_peek_ack` (write "abc", peek(0)=="abc", ack(2), used==1), `ring_full_keeps_oldest` (size 8, write 6 ok, write 3 false, dropped==3, peek(0) returns the first 6), `ring_write_larger_than_ring_drops` (size 8, write 9 false, used 0, dropped 9), `ring_wraps_index` (repeated write/ack across the buffer end preserves bytes), `ring_offsets_wrap` (init then force head_off=tail_off=0xFFFFFFF0, write 32, peek from 0xFFFFFFF0 returns all 32, ack(0x00000010) gives used 0), `ring_ack_out_of_range_noop`.
- [ ] **Step 2:** `cmake -S test -B test/build && cmake --build test/build && ./test/build/ns_tests` → FAIL (undefined ns_ring_*).
- [ ] **Step 3:** Implement `core/ns_ring.c`.
- [ ] **Step 4:** Run again → all ring tests PASS.
- [ ] **Step 5:** Commit `feat(core): ring with stream offsets and keep-oldest drop`.

### Task 2: Frame codec

**Files:** Create `core/ns_frame.h`, `core/ns_frame.c`, `test/test_frame.c`

**Interfaces:**
- Produces:
  - `#define NS_FRAME_HDR 16`, `#define NS_FRAME_MAX 1400`, `enum { NS_DATA = 1, NS_DROPPED = 2, NS_HELLO = 3, NS_ACK = 4 };`
  - `typedef struct { uint8_t type; uint32_t boot_id, seq, dev_ms; uint16_t len; } ns_hdr_t;`
  - `uint32_t ns_frame_encode(const ns_hdr_t *h, const uint8_t *payload, uint8_t *out, uint32_t cap);` returns total bytes, 0 if it does not fit
  - `bool ns_frame_decode(const uint8_t *in, uint32_t n, ns_hdr_t *h, const uint8_t **payload);` false on bad magic, version != 1, or len mismatch
  - DROPPED payload: u32 total dropped bytes. ACK: `seq` = highest contiguous offset received; HELLO: no payload.

- [ ] **Step 1: Failing tests** `frame_roundtrip` (DATA with 5-byte payload), `frame_rejects_bad_magic`, `frame_rejects_len_mismatch`, `frame_encode_too_big_returns_0` (payload 1385 > 1400-16), `frame_little_endian` (seq 0x01020304 encodes bytes 04 03 02 01 at offset 8).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit `feat(core): frame codec`.

### Task 3: Transmit state machine

**Files:** Create `core/ns_tx.h`, `core/ns_tx.c`, `test/test_tx.c`

**Interfaces:**
- Consumes: `ns_ring_t`, `ns_ring_peek`, `ns_ring_ack`, `ns_frame_encode`.
- Produces:
  - `typedef struct { ns_ring_t *ring; uint32_t boot_id; bool ack_mode; uint32_t resend_ms; uint32_t send_off; uint32_t last_progress_ms; uint32_t reported_dropped; } ns_tx_t;`
  - `void ns_tx_init(ns_tx_t *t, ns_ring_t *r, uint32_t boot_id, bool ack_mode, uint32_t resend_ms);`
  - `uint32_t ns_tx_next(ns_tx_t *t, uint32_t now_ms, uint8_t *out, uint32_t cap);` next datagram or 0. Order: a DROPPED frame if `ring->dropped != reported_dropped`; else DATA from `send_off` up to `cap - NS_FRAME_HDR`. If ack_mode and no ACK progress for `resend_ms` and send_off != tail, rewind send_off to tail first.
  - `void ns_tx_sent(ns_tx_t *t, uint32_t frame_len, bool ok);` caller reports the sendto() result; on ok advances send_off (and in non-ACK mode acks the ring; a DROPPED frame marks reported_dropped). On failure nothing moves.
  - `void ns_tx_on_ack(ns_tx_t *t, uint32_t off, uint32_t now_ms);` acks the ring if tail < off <= send_off (wrap-safe), updates last_progress_ms; otherwise ignored.

- [ ] **Step 1: Failing tests** `tx_sends_data_then_waits_for_ack` (write 10, next gives DATA seq 0 len 10, sent ok, next gives 0, ring used still 10), `tx_ack_frees_ring`, `tx_resends_after_timeout` (no ack, now += 2001 → DATA seq 0 again), `tx_send_failure_keeps_bytes` (sent ok=false → next returns same frame), `tx_no_ack_mode_commits_on_send`, `tx_reports_dropped_once`, `tx_ack_out_of_range_ignored` (ack beyond send_off and ack before tail).
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit `feat(core): transmit state machine with ACK and resend`.

### Task 4: ESP-IDF component glue and example

**Files:** Create `Kconfig`, `include/netserial.h`, `netserial.c`, `examples/basic/` (minimal app that connects Wi-Fi from menuconfig creds and logs a counter every second); modify `CMakeLists.txt` (SRCS core/*.c netserial.c, REQUIRES lwip esp_timer log, PRIV_REQUIRES esp_system).

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:
  - Kconfig: `NETSERIAL_RING_SIZE` 8192, `NETSERIAL_FLUSH_MS` 1000, `NETSERIAL_RESEND_MS` 2000, `NETSERIAL_PORT` 5514, `NETSERIAL_DEFAULT_HOST` "" (empty = wait for HELLO), `NETSERIAL_ACK_MODE` y.
  - `typedef bool (*netserial_filter_t)(const char *line, int len);` return false to skip a line.
  - `esp_err_t netserial_start(netserial_filter_t filter);` idempotent; installs the esp_log vprintf hook (chains the previous one), creates the task. Callable before Wi-Fi is up.
  - `void netserial_stats(uint32_t *used, uint32_t *dropped);`
- Behaviour: hook formats into a 192-byte stack buffer, applies the filter, writes to the ring under a portMUX; an `E (` level line notifies the task to flush now. Task: bound UDP socket on NETSERIAL_PORT, loop every flush_ms or on notify: drain received HELLO/ACK (non-blocking recvfrom; HELLO sets the peer address and rewinds send_off to tail), then `ns_tx_next`/sendto/`ns_tx_sent` until 0 or failure. No peer and empty default host → send nothing. boot_id from esp_random().

- [ ] **Step 1:** `idf.py -C examples/basic set-target esp32c6 build` fails before the glue exists (missing netserial.h).
- [ ] **Step 2:** Implement; build passes with `-Wall -Wextra -Werror` on the component.
- [ ] **Step 3:** Host tests still pass (core unchanged).
- [ ] **Step 4:** Commit `feat: ESP-IDF glue, Kconfig and basic example`.

### Task 5: Host tool

**Files:** Create `host/netserial.py`, `host/test_netserial.py`

**Interfaces:**
- Consumes: wire format from Task 2 (re-implemented in Python with `struct.Struct('<2sBBIIIH')`).
- Produces: CLI `netserial.py DEVICE [--port 5514] [--log FILE] [--pty] [--no-ack]`; class `Receiver(on_line, on_event)` with `feed(datagram) -> list[bytes acks]` so it is testable without sockets.
- Behaviour: sends HELLO every 5 s until data arrives and whenever 10 s pass silent; splices DATA by `seq`, drops duplicates, buffers out-of-order up to 64 KB, ACKs highest contiguous offset after each datagram; emits events `JOINED offset`, `GAP n bytes`, `DROPPED n` (device-side), `REBOOT` (boot_id change). Log lines: `<UTC ISO ms> <line>`; events as `==== EVENT ... ====` like quickvolt's serial_logger. `--pty`: `os.openpty()`, print the slave path, write the reassembled stream to the master.

- [ ] **Step 1: Failing tests** `test_in_order_lines`, `test_duplicate_ignored`, `test_out_of_order_spliced`, `test_join_mid_stream` (first DATA seq 5000 → JOINED 5000, no GAP), `test_reboot_marker`, `test_dropped_event`, `test_ack_is_contiguous_high_water`.
- [ ] **Step 2:** `python3 -m pytest host -q` → FAIL. **Step 3:** Implement. **Step 4:** → PASS.
- [ ] **Step 5:** Commit `feat(host): receiver, log and PTY`.

### Task 6: Hardware check (needs a bench board and the owner's OK to flash it)

- [ ] Flash `examples/basic` to a spare C6; run `host/netserial.py <ip> --log run.log`.
- [ ] Verify: (a) boot lines logged before the IP appear (captured pre-network), (b) toggle the AP or move out of range for 5 s: no GAP event, counter lines continuous, (c) kill and restart the host tool: JOINED, then continuous, (d) `--pty` with `picocom` shows the stream.
- [ ] Save results to `~/quickvolt-logs/results/YYYY-MM-DD_netserial-hil.md` with board IP, MAC, build.
- [ ] Commit README usage section.
