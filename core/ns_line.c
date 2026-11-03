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
