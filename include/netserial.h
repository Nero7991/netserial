// netserial: stream this device's log output to a host over UDP, like a serial cable over the
// network. See README.md. Output-only in v1.
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return false to keep a formatted log line out of the stream (for example a periodic heartbeat that
// would otherwise keep a power-saving radio awake). Called from the logging task; must not block.
typedef bool (*netserial_filter_t)(const char *line, int len);

// Install the esp_log hook (chaining the existing one) and start the sender task. Call after
// esp_netif_init(); the network does not need to be up, output is held until it is. Idempotent.
esp_err_t netserial_start(netserial_filter_t filter);

// Bytes currently held in the ring, and total bytes dropped because it was full.
void netserial_stats(uint32_t *used, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
