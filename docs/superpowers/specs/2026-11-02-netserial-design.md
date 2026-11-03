# netserial: serial console over the network (design draft, 2026-11-02)

## Intent
A small reusable ESP-IDF component that does what a USB/UART serial cable does for a device, over
the network: stream the device's console output to a host, and optionally accept console input.
It must keep working exactly where a cable fails: in light sleep (the C6 USB-Serial/JTAG dies at
the first light sleep), on a board with no USB attached, and across Wi-Fi drops. Ethernet comes for
free (same lwIP path); CAN is a later transport.

Origin: the #386 UDP log diag (quickvolt `qv_diaglog.c`). It worked, and it taught four rules this
design keeps:
1. The log hook never touches the network. It copies into a RAM ring under a spinlock and returns.
2. Bytes leave the ring only after a send succeeds (the first diag lost the whole disconnect window
   because it consumed before sendto()).
3. Capture starts before the network is up; the ring holds boot and link-down output.
4. Sending must not keep a power-saving radio awake: batch, and let the app filter chatty lines.

## Goals
- Lossless output while the ring has room, including across link drops and reboots of the host tool.
- Loss that does happen is visible: sequence numbers, a dropped-byte counter, a boot id.
- Host side looks like a serial port: a PTY so `minicom`, `picocom`, `idf.py monitor --port` attach.
- Zero dynamic allocation after init; static ring; one low-priority task.
- Portable core (ring, framing, ack logic) in plain C99, unit-tested on the host.

## Non-goals (v1)
- Not RFC 2217 (no baud/DTR/RTS emulation; there is no physical UART to configure).
- No reset or bootloader entry over the network (OTA already covers flashing).
- No CAN transport in v1; the transport interface is shaped so it can be added.
- No encryption of output in v1 (LAN only; see Security).

## Anti-goals (a failure even if it works)
- Any way for the log path to block, allocate, or recurse into lwIP.
- Unauthenticated console input. A console can drive hardware (QuickVolt's can energize an output).
- Keeping a TWT/Low-power radio awake on an idle device.

## Shape
```
 esp_log vprintf hook ─┐
 stdout/printf ────────┼─> ring (static, spinlock, drop+count when full)
 optional UART RX tap ─┘          │ peek / commit-on-ack
                                  v
                     sender task (prio low, every flush_ms or on ERROR)
                                  │ frames
                     transport: UDP (v1) | TCP (v1.1) | CAN ISO-TP (later)
                                  │
                     host: netserial.py  -> log file (UTC stamps) + PTY
                                  │ ACK / HELLO / optional signed input
                                  └──────────────> device input ring -> app callback
```

## Wire format (UDP)
Datagram <= 1400 bytes:
`magic "NS" | ver u8 | type u8 | boot_id u32 | seq u32 (byte offset of payload) | dev_ms u32 | len u16 | payload`
- `seq` is a byte offset in the device's output stream since boot, so the host can splice, dedupe
  retransmits and measure gaps exactly.
- `boot_id` is random per boot; a change tells the host the device rebooted (offset restarts).
- Types: DATA, DROPPED (count), HELLO (host->device), ACK (host->device, highest contiguous
  offset), INPUT (host->device, signed).

## Reliability
- Device keeps bytes in the ring until ACKed (not just until sendto() succeeds). It resends from the
  last ACKed offset after `resend_ms` with no ACK progress. With no host attached it simply holds
  output until the ring fills, then drops new output and counts it, so the boot log survives.
  (Decided: keep oldest.)
- ACK mode can be disabled (fire-and-forget) for minimum airtime.

## Discovery and addressing
- Host tool sends HELLO to the device (by IP or mDNS name). The device streams to the last host that
  said HELLO, so there is no hard-coded host IP. A compile-time default host is allowed for
  boot-time capture before any HELLO.
- mDNS service `_netserial._udp` advertised when the app already runs mDNS.

## Input (host -> device), off by default
- Enabled by Kconfig plus a pre-shared key provisioned by the app (NVS), never a default key.
- Each INPUT frame carries a counter and HMAC-SHA256(key, boot_id|counter|payload); replays and stale
  counters are rejected. Delivered to an app callback or a small input ring the app reads like stdin.

## Power behaviour
- `flush_ms` (default 1000) batches output; an ERROR-level line flushes immediately.
- App-supplied line filter (the #386 diag filtered the 200 ms control-loop heartbeat).
- No transmission when the ring is empty; ACKs ride the same wake.

## Resources (defaults, Kconfig)
Ring 8 KB (16 KB used for #386), task stack 3 KB, one UDP socket, one esp_timer-free loop.

## Host tool (Python 3, no deps beyond stdlib)
`netserial.py <device> [--port 5514] [--log FILE] [--pty] [--key FILE]`
- Writes UTC-stamped lines (board ms kept alongside), marks reboots and gaps as events, like
  quickvolt's serial_logger.
- `--pty` creates a pseudo-terminal and prints its path; typed input goes out as signed INPUT frames.

## Testing
- Host unit tests for ring (wrap, full, commit), framing, ACK/resend, HMAC verify and replay reject.
- HIL: (a) link-drop test: hold output through a forced AP disconnect, verify no byte gap;
  (b) light-sleep board with USB unplugged, verify continuous capture; (c) throughput at 115200-
  equivalent rates; (d) input rejected without the key.

## Left to the builder
Exact Kconfig names, task priority, resend backoff, PTY implementation details.

## Decided (2026-11-02)
1. Its own repo, `~/GitHub/netserial`: ESP-IDF component plus host tool; quickvolt consumes it later.
2. v1 is output-only (streaming, ACK/resend, log file, PTY view). Signed input is v1.1; the INPUT
   frame type and HMAC scheme above are reserved for it and not built in v1.
3. Ring full with no host draining: keep the oldest, drop and count new output.
