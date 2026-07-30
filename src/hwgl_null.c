// No offscreen GL backend on this platform yet. Cores that ask for hw
// rendering are refused at SET_HW_RENDER, which is the answer the frontend
// gave before any backend existed, so they fail the same way they always did.

#include "hwgl.h"

bool hwgl_available(void) { return false; }
bool hwgl_requested(void) { return false; }
bool hwgl_set_callback(struct retro_hw_render_callback *cb) { (void)cb; return false; }
int  hwgl_init(unsigned w, unsigned h) { (void)w; (void)h; return -1; }
void hwgl_notify_reset(void) {}
void hwgl_shutdown(void) {}
void hwgl_bind(unsigned w, unsigned h) { (void)w; (void)h; }
void hwgl_set_async(bool on) { (void)on; }
bool hwgl_async_active(void) { return false; }

const uint8_t *hwgl_read(unsigned w, unsigned h, size_t *pitch, bool *bottom_up) {
    (void)w; (void)h; (void)pitch; (void)bottom_up;
    return NULL;
}

const char *hwgl_error(void) { return "no offscreen GL backend on this platform"; }
const char *hwgl_info(void) { return "none"; }
