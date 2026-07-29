#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rom.h"

static void *sym(void *h, const char *name, int *err) {
    void *p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "core: missing symbol %s\n", name);
        *err = 1;
    }
    return p;
}

int core_load(Core *c, const char *path) {
    memset(c, 0, sizeof *c);
    c->handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!c->handle) {
        fprintf(stderr, "core: %s\n", dlerror());
        return -1;
    }
    int err = 0;
    void *h = c->handle;
    *(void **)&c->init = sym(h, "retro_init", &err);
    *(void **)&c->deinit = sym(h, "retro_deinit", &err);
    *(void **)&c->api_version = sym(h, "retro_api_version", &err);
    *(void **)&c->get_system_info = sym(h, "retro_get_system_info", &err);
    *(void **)&c->get_system_av_info = sym(h, "retro_get_system_av_info", &err);
    *(void **)&c->set_environment = sym(h, "retro_set_environment", &err);
    *(void **)&c->set_video_refresh = sym(h, "retro_set_video_refresh", &err);
    *(void **)&c->set_audio_sample = sym(h, "retro_set_audio_sample", &err);
    *(void **)&c->set_audio_sample_batch = sym(h, "retro_set_audio_sample_batch", &err);
    *(void **)&c->set_input_poll = sym(h, "retro_set_input_poll", &err);
    *(void **)&c->set_input_state = sym(h, "retro_set_input_state", &err);
    *(void **)&c->set_controller_port_device = sym(h, "retro_set_controller_port_device", &err);
    *(void **)&c->reset = sym(h, "retro_reset", &err);
    *(void **)&c->run = sym(h, "retro_run", &err);
    *(void **)&c->serialize_size = sym(h, "retro_serialize_size", &err);
    *(void **)&c->serialize = sym(h, "retro_serialize", &err);
    *(void **)&c->unserialize = sym(h, "retro_unserialize", &err);
    *(void **)&c->load_game = sym(h, "retro_load_game", &err);
    *(void **)&c->unload_game = sym(h, "retro_unload_game", &err);
    *(void **)&c->get_memory_data = sym(h, "retro_get_memory_data", &err);
    *(void **)&c->get_memory_size = sym(h, "retro_get_memory_size", &err);
    if (err) { dlclose(h); c->handle = NULL; return -1; }

    unsigned api = c->api_version();
    if (api != RETRO_API_VERSION) {
        fprintf(stderr, "core: API version %u, expected %u\n", api, RETRO_API_VERSION);
        dlclose(h); c->handle = NULL; return -1;
    }
    memset(&c->sys, 0, sizeof c->sys);
    c->get_system_info(&c->sys);
    return 0;
}

int core_load_game(Core *c, const char *rom_path) {
    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = rom_path;

    void *data = NULL;
    size_t size = 0;
    if (!c->sys.need_fullpath) {
        FILE *f = fopen(rom_path, "rb");
        if (!f) { perror("rom"); return -1; }
        struct stat st;
        if (fstat(fileno(f), &st) != 0) { fclose(f); return -1; }
        size = (size_t)st.st_size;
        data = malloc(size);
        if (!data) { fclose(f); return -1; }
        if (fread(data, 1, size, f) != size) {
            fprintf(stderr, "rom: short read\n");
            free(data); fclose(f); return -1;
        }
        fclose(f);
        gi.data = data;
        gi.size = size;
    }

    bool ok = c->load_game(&gi);
    free(data);   // cores copy what they need during load_game
    if (!ok) {
        fprintf(stderr, "core: failed to load %s\n", rom_path);
        return -1;
    }
    c->loaded = true;
    memset(&c->av, 0, sizeof c->av);
    c->get_system_av_info(&c->av);
    return 0;
}

void core_unload(Core *c) {
    if (!c->handle) return;
    if (c->loaded && c->unload_game) c->unload_game();
    if (c->deinit) c->deinit();
    dlclose(c->handle);
    memset(c, 0, sizeof *c);
}
