#ifndef ROM_HWGL_H
#define ROM_HWGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

// Offscreen OpenGL for cores that render through RETRO_HW_CONTEXT_OPENGL*.
// Nothing is ever presented: the core draws into an FBO which is read back
// each frame and handed to the normal software video path. On platforms with
// no backend the stubs report unavailable and hw cores are refused up front.

// True when a backend exists for this build.
bool hwgl_available(void);

// Records the core's SET_HW_RENDER request and fills in the frontend halves
// of the callback. Returns false for context types we cannot serve.
bool hwgl_set_callback(struct retro_hw_render_callback *cb);

// True once a core has asked for hw rendering.
bool hwgl_requested(void);

// Brings up the context and an FBO of at least w x h. Must run on the thread
// that will call retro_run. Does not invoke context_reset; call hwgl_notify
// once the game is loaded.
int  hwgl_init(unsigned w, unsigned h);

// Hands the core its context_reset, after which it may allocate GL objects.
void hwgl_notify_reset(void);

void hwgl_shutdown(void);

// Binds the FBO so the core's get_current_framebuffer lands somewhere real.
// Call before every retro_run.
void hwgl_bind(unsigned w, unsigned h);

// Selects pixel-buffer readback, which returns the *previous* frame so the
// GPU transfer overlaps a frame of emulation instead of stalling it. Costs one
// frame of input latency. Call before hwgl_init. Ignored where unsupported.
void hwgl_set_async(bool on);

// True when readback is actually running through pixel buffers, which is only
// known after hwgl_init.
bool hwgl_async_active(void);

// Reads the FBO back as XRGB8888. `pitch` is the row stride in bytes and
// `bottom_up` reports GL's origin convention, so the caller can flip while it
// converts. Returns NULL if there is nothing to read.
const uint8_t *hwgl_read(unsigned w, unsigned h, size_t *pitch, bool *bottom_up);

// Last error text, for the log.
const char *hwgl_error(void);

// GL version and renderer of the live context, for the log.
const char *hwgl_info(void);

#endif
