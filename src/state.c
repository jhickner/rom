#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "emu.h"

static char g_base[256];
static char g_states_dir[512];
static char g_saves_dir[512];
static uint8_t *g_sram_shadow;
static size_t   g_sram_shadow_size;

void state_paths_init(const char *rom_path) {
    const char *slash = strrchr(rom_path, '/');
    const char *name = slash ? slash + 1 : rom_path;
    snprintf(g_base, sizeof g_base, "%s", name);
    char *dot = strrchr(g_base, '.');
    if (dot && dot != g_base) *dot = 0;

    snprintf(g_states_dir, sizeof g_states_dir, "%s/states", config_dir());
    snprintf(g_saves_dir, sizeof g_saves_dir, "%s/saves", config_dir());
    ensure_dir(g_states_dir);
    ensure_dir(g_saves_dir);
}

const char *state_rom_base(void) { return g_base; }

static void state_path(char *out, size_t cap, int slot) {
    snprintf(out, cap, "%s/%s.state%d", g_states_dir, g_base, slot);
}

static void sram_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.srm", g_saves_dir, g_base);
}

// Write to a temp file and rename, so an interrupted save never truncates a
// good one.
static int write_atomic(const char *path, const void *data, size_t size) {
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

int state_save(Core *c, int slot, char *err, size_t errlen) {
    size_t size = c->serialize_size();
    if (size == 0) {
        snprintf(err, errlen, "core does not support save states");
        return -1;
    }
    void *buf = malloc(size);
    if (!buf) { snprintf(err, errlen, "out of memory"); return -1; }
    if (!c->serialize(buf, size)) {
        free(buf);
        snprintf(err, errlen, "serialize failed");
        return -1;
    }
    char path[600];
    state_path(path, sizeof path, slot);
    int rc = write_atomic(path, buf, size);
    free(buf);
    if (rc != 0) { snprintf(err, errlen, "write failed"); return -1; }
    return 0;
}

int state_load(Core *c, int slot, char *err, size_t errlen) {
    char path[600];
    state_path(path, sizeof path, slot);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "slot %d empty", slot); return -1; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); snprintf(err, errlen, "stat failed"); return -1; }
    size_t size = (size_t)st.st_size;
    void *buf = malloc(size);
    if (!buf) { fclose(f); snprintf(err, errlen, "out of memory"); return -1; }
    if (fread(buf, 1, size, f) != size) {
        fclose(f); free(buf);
        snprintf(err, errlen, "short read");
        return -1;
    }
    fclose(f);
    bool ok = c->unserialize(buf, size);
    free(buf);
    if (!ok) { snprintf(err, errlen, "state incompatible"); return -1; }
    return 0;
}

bool state_slot_exists(int slot) {
    char path[600];
    state_path(path, sizeof path, slot);
    struct stat st;
    return stat(path, &st) == 0;
}

int sram_load(Core *c) {
    void *mem = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || size == 0) return 0;

    char path[600];
    sram_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(mem, 1, size, f);
        fclose(f);
        (void)n;
    }
    free(g_sram_shadow);
    g_sram_shadow = malloc(size);
    g_sram_shadow_size = size;
    if (g_sram_shadow) memcpy(g_sram_shadow, mem, size);
    return 0;
}

bool sram_dirty(Core *c) {
    void *mem = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || size == 0 || !g_sram_shadow || size != g_sram_shadow_size)
        return false;
    return memcmp(mem, g_sram_shadow, size) != 0;
}

int sram_save(Core *c) {
    void *mem = c->get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = c->get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!mem || size == 0) return 0;

    char path[600];
    sram_path(path, sizeof path);
    if (write_atomic(path, mem, size) != 0) return -1;
    if (g_sram_shadow && g_sram_shadow_size == size)
        memcpy(g_sram_shadow, mem, size);
    return 0;
}
