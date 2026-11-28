#include "ns_line.h"

int ns_line_fit(char *line, int n, int cap)
{
    if (n <= 0 || cap < 2) return 0;
    if (n > cap - 1) {
        n = cap - 1;
        line[n - 1] = '\n';
    }
    return n;
}

bool ns_line_is_error(const char *line, int n)
{
    int i = 0;
    if (n >= 2 && line[0] == '\033' && line[1] == '[') {
        for (i = 2; i < n && line[i] != 'm'; i++) {
        }
        i++;   // past the 'm' (or past the end when the escape is unterminated)
    }
    return i + 1 < n && line[i] == 'E' && line[i + 1] == ' ';
}
