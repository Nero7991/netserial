// ns_line: fit a formatted log line into a fixed buffer without losing its line break.
#pragma once

// n is what vsnprintf returned for a buffer of cap bytes. Returns the byte count to keep; when the
// line was truncated, its last kept byte becomes '\n' so it does not run into the next line.
int ns_line_fit(char *line, int n, int cap);
