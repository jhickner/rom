#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rom.h"

// Runs a child to completion with its output going straight to our terminal,
// so a multi-minute core build shows progress as it happens.
static int run(const char *cwd, char *const argv[]) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) { perror(cwd); _exit(127); }
        execvp(argv[0], argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static bool have_tool(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) return false;
    char buf[2048];
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t len = sep ? (size_t)(sep - path) : strlen(path);
        if (len > 0 && len < sizeof buf / 2) {
            snprintf(buf, sizeof buf, "%.*s/%s", (int)len, path, name);
            if (access(buf, X_OK) == 0) return true;
        }
        path = sep ? sep + 1 : path + len;
    }
    return false;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { perror(src); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { perror(dst); fclose(in); return -1; }

    int rc = 0;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { perror(dst); rc = -1; break; }
    }
    if (ferror(in)) { perror(src); rc = -1; }
    fclose(in);
    if (fclose(out) != 0) { perror(dst); rc = -1; }
    if (rc == 0) chmod(dst, 0755);
    else remove(dst);
    return rc;
}

static int job_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return (int)n;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int core_fetch(const CoreSpec *spec, char *out, size_t cap) {
    if (!have_tool("git") || !have_tool("make")) {
        fprintf(stderr, APP_NAME ": git and make must be on PATH to build a core\n");
        return -1;
    }

    char srcroot[1024], coresdir[1024];
    snprintf(srcroot, sizeof srcroot, "%s/src", config_dir());
    snprintf(coresdir, sizeof coresdir, "%s/cores", config_dir());
    if (ensure_dir(srcroot) != 0 || ensure_dir(coresdir) != 0) {
        fprintf(stderr, APP_NAME ": cannot create directories under %s\n", config_dir());
        return -1;
    }

    const char *slash = strrchr(spec->repo, '/');
    const char *name = slash ? slash + 1 : spec->repo;

    char srcdir[2048], gitdir[2176];
    snprintf(srcdir, sizeof srcdir, "%s/%s", srcroot, name);
    snprintf(gitdir, sizeof gitdir, "%s/.git", srcdir);

    if (is_dir(gitdir)) {
        printf("Reusing checkout %s\n\n", srcdir);
    } else {
        char url[512];
        snprintf(url, sizeof url, "https://github.com/%s", spec->repo);
        printf("Cloning %s\n\n", url);
        char *argv[] = { "git", "clone", "--depth", "1", url, srcdir, NULL };
        if (run(NULL, argv) != 0) {
            fprintf(stderr, "\n" APP_NAME ": clone failed\n");
            return -1;
        }
    }

    char builddir[2176];
    if (spec->subdir) snprintf(builddir, sizeof builddir, "%s/%s", srcdir, spec->subdir);
    else snprintf(builddir, sizeof builddir, "%s", srcdir);

    char jobs[16];
    snprintf(jobs, sizeof jobs, "-j%d", job_count());
    char *argv[8];
    int n = 0;
    argv[n++] = "make";
    if (spec->makefile) { argv[n++] = "-f"; argv[n++] = (char *)spec->makefile; }
    argv[n++] = jobs;
    for (int i = 0; i < 3 && spec->makeargs[i]; i++) argv[n++] = (char *)spec->makeargs[i];
    argv[n] = NULL;

    printf("\nBuilding %s, this takes a few minutes\n\n", spec->core);
    if (run(builddir, argv) != 0) {
        fprintf(stderr, "\n" APP_NAME ": build failed in %s\n", builddir);
        return -1;
    }

    char artifact[2304];
    struct stat st;
    snprintf(artifact, sizeof artifact, "%s/%s", builddir, spec->core);
    if (stat(artifact, &st) != 0) {
        fprintf(stderr, "\n" APP_NAME ": build finished but %s was not produced\n",
                artifact);
        return -1;
    }

    snprintf(out, cap, "%s/%s", coresdir, spec->core);
    if (copy_file(artifact, out) != 0) return -1;
    printf("\nInstalled %s\n", out);
    return 0;
}
