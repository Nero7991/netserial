# netserial

Serial console over the network for ESP-IDF devices. The device's log output is copied into a RAM
ring and streamed over UDP to a host tool, which writes a timestamped log and can expose the stream
as a pseudo-terminal. Output survives Wi-Fi drops (bytes stay in the ring until the host ACKs them),
is captured from boot (before the network is up), and any loss is reported, never silent.

Status: v1 in development (output only). Design: `docs/superpowers/specs/2026-11-02-netserial-design.md`.

## Host tests

```bash
cmake -S test -B test/build && cmake --build test/build && ./test/build/ns_tests
```
