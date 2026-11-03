# netserial

Serial console over the network for ESP-IDF devices. The device's log output is copied into a RAM
ring and streamed over UDP to a host tool, which writes a timestamped log and can expose the stream
as a pseudo-terminal. Output survives Wi-Fi drops (bytes stay in the ring until the host ACKs them),
is captured from boot (before the network is up), and any loss is reported, never silent.

Status: v1 in development (output only). Design: `docs/superpowers/specs/2026-11-02-netserial-design.md`.

## Use

In the app (after `esp_netif_init()`; the network does not need to be up yet):

```c
#include "netserial.h"
ESP_ERROR_CHECK(netserial_start(NULL));   // or a filter that returns false for chatty lines
```

On the host:

```bash
python3 host/netserial.py <device-ip> --log run.log          # log with UTC stamps
python3 host/netserial.py <device-ip> --pty                  # then: picocom /dev/pts/N
python3 host/netserial.py <device-ip> --listen 5514          # if the device streams from boot
                                                             # (CONFIG_NETSERIAL_DEFAULT_HOST)
```

Events in the log: `JOINED <offset>`, `GAP <bytes>`, `DROPPED <bytes>` (device ring was full),
`REBOOT`. Kconfig: ring size (power of two), flush interval, resend interval, port, default host,
ACK mode. Requires log version 1 (the default): under log v2 the line filter sees fragments.

## Host tests

```bash
cmake -S test -B test/build && cmake --build test/build && ./test/build/ns_tests
```
