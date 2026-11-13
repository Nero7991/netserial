// ns_line: fit a formatted log line into a fixed buffer without losing its line break.
#pragma once
#include <stdbool.h>

// n is what vsnprintf returned for a buffer of cap bytes. Returns the byte count to keep; when the
// line was truncated, its last kept byte becomes '\n' so it does not run into the next line.
int ns_line_fit(char *line, int n, int cap);

// True for an esp_log error line ("E (...)"), with or without a leading color escape
// (CONFIG_LOG_COLORS puts "\033[0;31m" before the level letter).
bool ns_line_is_error(const char *line, int n);
