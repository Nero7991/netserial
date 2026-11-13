import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import netserial as ns  # noqa: E402

BOOT = 0x1234


def data(seq, payload, boot=BOOT):
    return ns.encode(ns.DATA, boot, seq, 0, payload)


def make():
    lines, events = [], []
    rx = ns.Receiver(lines.append, events.append)
    return rx, lines, events


def acked(frames):
    assert len(frames) == 1
    h, _ = ns.decode(frames[0])
    assert h.type == ns.ACK
    return h.seq


def test_in_order_lines():
    rx, lines, events = make()
    rx.feed(data(0, b"one\ntw"))
    rx.feed(data(6, b"o\nthree\n"))
    assert lines == [b"one", b"two", b"three"]
    assert events == ["JOINED 0"]


def test_duplicate_ignored():
    rx, lines, _ = make()
    rx.feed(data(0, b"a\n"))
    assert acked(rx.feed(data(0, b"a\n"))) == 2
    assert lines == [b"a"]


def test_out_of_order_spliced():
    rx, lines, events = make()
    rx.feed(data(0, b"a\n"))
    assert acked(rx.feed(data(4, b"c\n"))) == 2
    assert acked(rx.feed(data(2, b"b\n"))) == 6
    assert lines == [b"a", b"b", b"c"]
    assert not any(e.startswith("GAP") for e in events)


def test_join_mid_stream():
    rx, lines, events = make()
    assert acked(rx.feed(data(5000, b"late\n"))) == 5005
    assert events == ["JOINED 5000"]
    assert lines == [b"late"]


def test_reboot_marker():
    rx, lines, events = make()
    rx.feed(data(0, b"x\n"))
    rx.feed(data(0, b"y\n", boot=0x9999))
    assert lines == [b"x", b"y"]
    assert events == ["JOINED 0", "REBOOT", "JOINED 0"]


def test_dropped_event():
    rx, _, events = make()
    rx.feed(ns.encode(ns.DROPPED, BOOT, 0, 0, (300).to_bytes(4, "little")))
    rx.feed(ns.encode(ns.DROPPED, BOOT, 0, 0, (300).to_bytes(4, "little")))
    rx.feed(ns.encode(ns.DROPPED, BOOT, 0, 0, (350).to_bytes(4, "little")))
    assert events == ["DROPPED 300", "DROPPED 50"]


def test_ack_is_contiguous_high_water():
    rx, _, _ = make()
    rx.feed(data(0, b"abc"))
    assert acked(rx.feed(data(10, b"zz"))) == 3


def test_gap_when_pending_overflows():
    rx, lines, events = make()
    rx.feed(data(0, b"a\n"))
    big = b"x" * 1000 + b"\n"
    for i in range(70):
        rx.feed(data(100 + i * len(big), big))
    assert "GAP 98" in events
    assert len(lines) >= 70


def test_bad_frame_ignored():
    rx, lines, events = make()
    assert rx.feed(b"garbage") == []
    assert lines == [] and events == []


def test_listen_binds_given_port():
    probe = ns.make_socket(0)
    port = probe.getsockname()[1]
    probe.close()
    s = ns.make_socket(port)
    assert s.getsockname()[1] == port
    s.close()


def sync(seq, boot=BOOT):
    return ns.encode(ns.SYNC, boot, seq, 0)


def test_sync_skips_bytes_another_host_took():
    # Rejoining after another host ACKed past us: the device only holds from 500 on.
    rx, lines, events = make()
    rx.feed(data(0, b"a\n"))
    assert acked(rx.feed(sync(500))) == 500
    assert "GAP 498" in events
    assert acked(rx.feed(data(500, b"b\n"))) == 502
    assert lines == [b"a", b"b"]


def test_sync_behind_us_changes_nothing():
    rx, _, events = make()
    rx.feed(data(0, b"abcd"))
    assert acked(rx.feed(sync(2))) == 4
    assert not any(e.startswith("GAP") for e in events)


def test_sync_first_frame_joins():
    rx, _, events = make()
    rx.feed(sync(700))
    assert events == ["JOINED 700"]
    assert acked(rx.feed(data(700, b"x\n"))) == 702


def test_sync_reports_reboot():
    rx, _, events = make()
    rx.feed(data(0, b"x\n"))
    rx.feed(sync(0, boot=0x9999))
    assert events == ["JOINED 0", "REBOOT", "JOINED 0"]


def test_bye_marks_displaced():
    rx, _, events = make()
    payload = bytes([192, 168, 97, 50]) + (40000).to_bytes(2, "big")
    assert rx.feed(ns.encode(ns.BYE, BOOT, 0, 0, payload)) == []
    assert rx.displaced == "192.168.97.50:40000"
    assert events == ["DISPLACED by 192.168.97.50:40000"]


def test_stall_skips_ahead_after_timeout():
    # A hole the device will never fill (SYNC lost): skip it after STALL_S, not after 64 KB.
    rx, lines, events = make()
    rx.feed(data(0, b"a\n"), now=0.0)
    rx.feed(data(10, b"c\n"), now=1.0)
    rx.feed(data(10, b"c\n"), now=1.0 + ns.STALL_S - 0.5)
    assert not any(e.startswith("GAP") for e in events)
    assert acked(rx.feed(data(10, b"c\n"), now=1.0 + ns.STALL_S + 0.1)) == 12
    assert "GAP 8" in events
    assert lines == [b"a", b"c"]


def test_stall_clock_resets_on_progress():
    rx, _, events = make()
    rx.feed(data(0, b"a\n"), now=0.0)
    rx.feed(data(10, b"c\n"), now=1.0)          # hole 2..10
    rx.feed(data(2, b"b\n"), now=2.0)           # partly filled: progress
    rx.feed(data(10, b"c\n"), now=2.0 + ns.STALL_S - 0.5)
    assert not any(e.startswith("GAP") for e in events)
