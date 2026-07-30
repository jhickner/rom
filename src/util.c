#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rom.h"

double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char *config_dir(void) {
    static char dir[512];
    if (dir[0]) return dir;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/%s", xdg, APP_NAME);
    else if (home && *home) snprintf(dir, sizeof dir, "%s/.config/%s", home, APP_NAME);
    else snprintf(dir, sizeof dir, ".%s", APP_NAME);
    return dir;
}

// Write to a temp file and rename, so an interrupted save never truncates a
// good one.
int write_file_atomic(const char *path, const void *data, size_t size) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, size, f);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || w != size) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int ensure_dir(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}
