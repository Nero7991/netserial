#!/usr/bin/env python3
"""netserial host tool: receive a device's console stream over UDP, log it, optionally expose a PTY.

usage: netserial.py DEVICE [--port 5514] [--listen PORT] [--log FILE] [--pty] [--no-ack]

Sends HELLO to DEVICE:PORT until data arrives (and again after 10 s of silence), splices DATA frames
by stream offset, ACKs the contiguous high-water offset, and reports JOINED / GAP / DROPPED / REBOOT
as events. Wire format: see core/ns_frame.h. Standard library only.
"""
import argparse
import collections
import datetime
import os
import select
import socket
import struct
import sys
import time

DATA, DROPPED, HELLO, ACK = 1, 2, 3, 4
VER = 1
HDR = struct.Struct("<2sBBIIIH")   # magic, ver, type, boot_id, seq, dev_ms, len: 18 bytes
PENDING_MAX = 64 * 1024            # out-of-order bytes held before skipping ahead (a GAP)
Header = collections.namedtuple("Header", "type boot_id seq dev_ms len")


def encode(ftype, boot_id, seq, dev_ms, payload=b""):
    return HDR.pack(b"NS", VER, ftype, boot_id & 0xFFFFFFFF, seq & 0xFFFFFFFF,
                    dev_ms & 0xFFFFFFFF, len(payload)) + payload


def decode(datagram):
    """Return (Header, payload) or None for anything that is not a valid frame."""
    if len(datagram) < HDR.size:
        return None
    magic, ver, ftype, boot_id, seq, dev_ms, n = HDR.unpack_from(datagram)
    if magic != b"NS" or ver != VER or len(datagram) != HDR.size + n:
        return None
    return Header(ftype, boot_id, seq, dev_ms, n), datagram[HDR.size:]


def _diff(a, b):
    """a - b as a signed 32-bit stream-offset distance (offsets wrap at 2^32)."""
    d = (a - b) & 0xFFFFFFFF
    return d - (1 << 32) if d >= (1 << 31) else d


class Receiver:
    """Reassembles one device's stream. feed() takes a datagram and returns the frames to send back.

    on_line(bytes) gets each complete line without its newline; on_raw(bytes), if given, gets the
    reassembled byte stream (for a PTY); on_event(str) gets JOINED n, GAP n, DROPPED n, REBOOT.
    """

    def __init__(self, on_line, on_event, on_raw=None, ack=True):
        self.on_line, self.on_event, self.on_raw, self.ack = on_line, on_event, on_raw, ack
        self.boot_id = None
        self._reset()

    def _reset(self):
        self.next = None          # next stream offset expected
        self.pending = {}         # seq -> payload, frames ahead of self.next
        self.partial = b""
        self.dropped_total = 0

    def _deliver(self, chunk):
        if self.on_raw:
            self.on_raw(chunk)
        self.partial += chunk
        *lines, self.partial = self.partial.split(b"\n")
        for line in lines:
            self.on_line(line.rstrip(b"\r"))

    def _absorb(self, seq, payload):
        """Take whatever part of [seq, seq+len) extends the contiguous stream; True if it did."""
        end = (seq + len(payload)) & 0xFFFFFFFF
        if _diff(seq, self.next) > 0 or _diff(end, self.next) <= 0:
            return False
        self._deliver(payload[_diff(self.next, seq):])
        self.next = end
        return True

    def _pull_pending(self):
        progressed = True
        while progressed and self.pending:
            progressed = False
            for seq in list(self.pending):
                payload = self.pending[seq]
                if self._absorb(seq, payload) or _diff((seq + len(payload)) & 0xFFFFFFFF, self.next) <= 0:
                    del self.pending[seq]
                    progressed = True

    def _skip_ahead(self):
        first = min(self.pending, key=lambda s: _diff(s, self.next))
        self.on_event("GAP %d" % _diff(first, self.next))
        self.next = first
        self._pull_pending()

    def feed(self, datagram):
        frame = decode(datagram)
        if frame is None:
            return []
        h, payload = frame
        if h.type not in (DATA, DROPPED):
            return []
        if self.boot_id is not None and h.boot_id != self.boot_id:
            self.on_event("REBOOT")
            self._reset()
        self.boot_id = h.boot_id
        if h.type == DROPPED:
            total = int.from_bytes(payload[:4], "little")
            if total > self.dropped_total:
                self.on_event("DROPPED %d" % (total - self.dropped_total))
                self.dropped_total = total
            return []
        if self.next is None:
            self.next = h.seq
            self.on_event("JOINED %d" % h.seq)
        if not self._absorb(h.seq, payload) and _diff(h.seq, self.next) > 0:
            if not self.ack:
                self.pending[h.seq] = payload
                self._skip_ahead()
            else:
                self.pending.setdefault(h.seq, payload)
                if sum(len(p) for p in self.pending.values()) > PENDING_MAX:
                    self._skip_ahead()
        self._pull_pending()
        return [encode(ACK, self.boot_id, self.next, 0)] if self.ack else []


def make_socket(listen_port=0):
    """UDP socket for the host side. listen_port 0 = any free port (the device learns it from HELLO);
    use the device's default-host port (5514) to catch boot-time output sent before any HELLO."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", listen_port))
    return sock


def _utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("device", help="device IPv4 address or hostname")
    ap.add_argument("--port", type=int, default=5514, help="device UDP port (default 5514)")
    ap.add_argument("--listen", type=int, default=0,
                    help="local UDP port; set to the device's NETSERIAL_PORT (5514) when the device "
                         "streams to this host from boot (NETSERIAL_DEFAULT_HOST)")
    ap.add_argument("--log", help="append UTC-stamped lines and events to this file")
    ap.add_argument("--pty", action="store_true", help="expose the stream on a pseudo-terminal")
    ap.add_argument("--no-ack", action="store_true", help="device built without ACK mode")
    args = ap.parse_args(argv)

    dev = (socket.gethostbyname(args.device), args.port)
    sock = make_socket(args.listen)
    log = open(args.log, "a", buffering=1) if args.log else None
    master = None
    if args.pty:
        master, slave = os.openpty()
        print("pty: %s" % os.ttyname(slave), file=sys.stderr)

    def out(text):
        print(text, flush=True)
        if log:
            log.write(text + "\n")

    def on_line(line):
        out("%s %s" % (_utc(), line.decode("utf-8", "replace")))

    def on_event(ev):
        out("%s ==== %s ====" % (_utc(), ev))

    def on_raw(chunk):
        if master is not None:
            try:
                os.write(master, chunk)
            except OSError:
                pass   # nobody attached to the PTY yet; the log still has it

    rx = Receiver(on_line, on_event, on_raw, ack=not args.no_ack)
    last_rx = last_hello = 0.0
    try:
        while True:
            now = time.monotonic()
            if now - last_rx > 10 and now - last_hello > 5:
                sock.sendto(encode(HELLO, 0, 0, 0), dev)
                last_hello = now
            ready, _, _ = select.select([sock], [], [], 1.0)
            if not ready:
                continue
            datagram, src = sock.recvfrom(2048)
            if src[0] != dev[0]:
                continue
            last_rx = time.monotonic()
            for reply in rx.feed(datagram):
                sock.sendto(reply, dev)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
