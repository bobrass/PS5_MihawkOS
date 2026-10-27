/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Open as many descriptors as the process budget allows and exit with them
 * still open. The PS5 kernel prints a BUDGET_FD_FILE warning on this path; the
 * panic-hook ring (hvctl hookpoll) should capture it, which validates the hook
 * without a panic.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    static int fds[8192];
    const char *paths[] = {"/dev/null", "/data", "/"};
    unsigned used = 0;
    for (unsigned p = 0; p < sizeof paths / sizeof paths[0]; p++) {
        int n = 0;
        for (; n < (int)(sizeof fds / sizeof fds[0]); n++) {
            int fd = open(paths[p], O_RDONLY);
            if (fd < 0) break;
            fds[n] = fd;
        }
        if (n > 0) { used = (unsigned)n; printf("fdstress: path=%s opened=%d\n", paths[p], n); break; }
    }
    if (!used) printf("fdstress: no path opened\n");
    return 0;
}
