# netserial hardware test findings (ESP32-S3, 2026-11-13)

## 1. The question

2026-11-13: "Test out netserial thoroughly so we can figure out bugs."

- Device: ESP32-S3 N16R8 + DW3110 UWB anchor (esp32-s3-dw3110, branch `ota-component`).
  netserial `bda0df0` defaults: 8 KB ring, FLUSH 1000 ms, RESEND 2000 ms, ACK mode.
- Logging load: the anchor logs ~40 lines/s, 2-6 KB/s, mostly per-ranging lines.
- Host: `host/netserial.py` on the same LAN over WiFi.

## 2. The answer

Delivery is correct: no corruption, no reordering, no duplicates, and every lost byte is reported
as `DROPPED`, including with 20% packet loss in each direction. Four real problems:

1. **Drops with a host attached.** ACKs are only read when the task wakes, once per FLUSH_MS
   (1 s), so the ring has to hold about 2 s of output. At 4-6 KB/s, 8 KB overflows. FLUSH_MS 200
   nearly eliminates it. Fix: read ACKs promptly (select/recv with a timeout instead of
   `ulTaskNotifyTake`), and/or notify the task when the ring passes half full.
2. **The error-line fast path never fires.** `ns_vprintf` checks `line[0]=='E'`, but with
   `CONFIG_LOG_COLORS=y` lines start with `\033[0;31m`. Fix: skip a leading ESC `[...m` sequence.
3. **Two hosts starve each other.** The last HELLO wins, and each host re-HELLOs after 10 s of
   silence, so they take turns stealing the stream. A host that re-joins after another host
   ACKed bytes also stalls: it holds the device's resend as out-of-order data until 64 KB piles
   up. Fix: the device could serve more than one peer, or tell a host it was displaced. The host
   should reset `next` to the first frame after its own HELLO.
4. **The hook doubles the printf stack on every logging task** (vsnprintf into 192 bytes, then
   the chained console vprintf). This overflowed the app's 2304-byte `sys_evt` stack (see the
   app's `docs/debugging/2026-11-13_sys-evt-stack-overflow.md`). Document the cost, or avoid
   formatting twice.

Usability:

5. **Keep-oldest hides recent output.** After 7 min with no host, joining shows the boot log
   and then `DROPPED 1523042`. The recent context you want when debugging is gone.
   `CONFIG_NETSERIAL_DEFAULT_HOST` plus `--listen 5514` avoids it and was verified working
   (JOINED 0 two seconds after boot, nothing dropped). Recommend it for field debugging.
6. **The host notices a reboot only after its 10 s silence plus a HELLO** (about 15 s). The boot
   log can overflow the ring before then. The default host avoids this too.

Added on branch `last-words` (`41a1405`): a `.noinit` flight recorder that replays the previous
boot's newest lines after a panic or watchdog reset. Verified for six crash types.

## 3. Procedure (each isolates one thing)

| # | Test | Controls for |
|---|---|---|
| T1 | Join a long-running device | ring behaviour with no host (keep-oldest) |
| T2 | netserial and USB console capture of the same boot, diffed line by line | loss, corruption, truncation; USB is ground truth |
| T3 | Delay of each line (host arrival - device ms, normalized), E vs I | error fast path |
| T4 | UDP proxy dropping 20% each way, 10% delayed 50-300 ms | ACK/resend correctness |
| T5 | Two host tools on one device | peer takeover |
| T6 | `--pty` with `cat`, then SIGSTOP the host for 30 s | PTY path, held-output catch-up |
| T7 | FLUSH 200 + DEFAULT_HOST build, host `--listen 5514` | boot capture without HELLO |
| T9 | Controlled load: HTTP 404s (one W line each) at a fixed rate, FLUSH 1000 vs 200 | ACK-latency hypothesis, A/B at matched load |

## 4. Evidence

T2: of 3258 USB lines in the window, 654 were missing from netserial. They total 41,475 bytes
(as written to the ring, with colors), against a reported `DROPPED` total of 44,080. Every
delivered line matches USB byte for byte. The losses fall exactly in the seconds where the
device logged about 6 KB/s.

T3: line delay normalized to the minimum offset:
```
I n=2082 median delay 0.648 s
E n=216  median delay 0.545 s     (no fast path)
```
T4: proxy saw up 170 / dropped 34, down 250 / dropped 48. Host got 2195 lines: no GAP, no
backwards device time, no duplicates, no garbled lines, TWR counter contiguous.

T5 lines per 5 s: host A `34:55=204 35:00=41` then nothing for 65 s; host B
`35:00=144 35:05=192 35:10=41 35:20=204 35:25=188 35:30=41 35:40=162 ...` (10 s blackouts).

T9 (40 s of controlled load):
```
FLUSH 200,  45.7 req/s: load lines delivered 1830 of 1830; DROPPED after join 377 B
FLUSH 1000, 57.0 req/s: delivered  784 of 2282; DROPPED 325,270 B
FLUSH 1000, 29.3 req/s: delivered  528 of 1170; DROPPED 152,536 B
```

## 5. Measured and REJECTED (do not retry)

- "Drops come from packet loss": T4 shows ACK/resend recovers 20% loss with no GAP. The drops
  come from ring capacity versus ACK latency (T9).
- "Ring too small, raise the ring size": not tested as the fix. T9 shows FLUSH latency is the
  lever at the same ring size.

## 6. Measurement traps

- `catch.py` opened the first `/dev/ttyACM*` it found, which was the tag, not the anchor: T1's USB
  side is invalid. T2 onward used an explicit port.
- Opening the S3's USB-Serial-JTAG port resets the chip (`USB_UART_CHIP_RESET`). Every USB
  capture starts a fresh boot.
- T9's first matched-rate attempt achieved only 29.3 req/s (the board refuses some
  connections). Compare delivered/requested ratios, not absolute counts.
- Natural log load varies 2-6 KB/s with ranging activity. Uncontrolled A/B (T2 vs T7) was
  inconclusive for that reason.

## Open, not yet answered

- Behaviour across a real AP disconnect (the design's HIL test a) was not run.
- Light sleep and C6 targets were not tested.
- Throughput ceiling with FLUSH 200 was not found (T9 only reached about 4 KB/s of added load).

## CORRECTION (2026-11-13, later): the hook does not double the printf stack

Finding 4 above said the hook "doubles the printf stack" because each line is formatted twice. That
is withdrawn: the hook's `vsnprintf` and the chained console `vprintf` run one after the other, not
nested, so the peak is one formatting frame plus the hook's own frame. Measured on the anchor
(4096-byte task, one line, high-water mark):

```
printf                       1900 B
ESP_LOGI, no hook            2012 B     ESP_LOGI with %f, no hook    2008 B
ESP_LOGI, hook (bda0df0)     2264 B     with %f, hook (bda0df0)      2340 B    (+252 / +332)
```

The cost is the 192-byte line buffer and the hook frame, not a second printf frame. That was
still enough to overflow the app's 2304-byte `sys_evt` stack.

## RESOLUTION (2026-11-13): fixes for findings 1-4, verified on the anchor

Same anchor and app; netserial at default settings (8 KB ring, FLUSH 1000 ms).

1. **Drops**: the task listens on the socket (select, 50 ms slices) while sent bytes await their ACK,
   and the hook wakes it when the ring reaches half full. T9 repeated at default FLUSH 1000:
   ```
   before (FLUSH 1000, 57.0 req/s): delivered  784 of 2282, DROPPED 325,270 B
   after  (FLUSH 1000, 55.4 req/s): delivered 2218 of 2218, DROPPED 0 B after the join
   ```
   (The one DROPPED 11001 at join is output from before any host connected: keep-oldest, finding 5.)
2. **Error fast path**: `ns_line_is_error` skips a leading color escape. T3 repeated:
   ```
   before: E median 0.545 s           after: E n=1148 median 0.006 s, p90 0.012 s
   ```
3. **Two hosts**: new device-to-host frames. SYNC (reply to HELLO, seq = oldest held offset) lets a
   rejoining host skip what another host took, instead of stalling. BYE (to the previous peer, and
   to any non-peer that ACKs) makes the host log `DISPLACED` and stop sending HELLO. The host also
   skips a hole after 6 s without progress (`STALL_S`), as a backstop for a lost SYNC. T5 repeated,
   lines per 5 s:
   ```
   A: 33:00=156 33:05=188 33:10=189 33:15=160, then DISPLACED by B, stops
   B: 33:15=28 33:20=185 ... 34:10=185 34:15=160 (60 s, no blackout, no GAP)
   ```
4. **Hook stack**: lines are formatted into one of 4 static buffers (claimed under the ring
   spinlock), falling back to a stack buffer only when all 4 are in use. When the console hook is
   plain `vprintf`, the formatted line is written with `fwrite` instead of being formatted again:
   ```
   ESP_LOGI, hook (fixed)       1656 B  (no hook: 2012)
   with %f, hook (fixed)        2148 B  (no hook: 2008)
   ```
   The %f case still costs +140 B (newlib's float formatting into a string is heavier); integer
   lines are now cheaper than without the hook.

Regression: T4 (20% loss each way, 60 s) repeated. No GAP, no out-of-order lines. DROPPED after
the join: before 26 events, 71,994 B; after 43 events, 15,850 B. Under loss, each lost ACK still
holds the ring until the 2 s resend.

Measurement trap found while repeating T4: counting duplicate lines with `sort | uniq -d` is not a
protocol check. The app logs identical lines in the same millisecond (`TWR: retry limit exceeded`
twice), and the old T4 log has 38 such pairs too. Check stream offsets for duplication, not text.

Still open: 5 (keep-oldest; documented in the README, with DEFAULT_HOST as the workaround), 6
(slow reboot detection), 7 (printf not captured), 8 (truncation at 191 bytes, now documented). The
displaced host does not reclaim the stream if the other host goes away (restart it).
