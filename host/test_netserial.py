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
