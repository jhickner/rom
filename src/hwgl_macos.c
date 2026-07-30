// Offscreen GL backend for macOS, via CGL. The core renders into an FBO that
// is never presented; each frame is read back and pushed through the same
// software path as a software core's framebuffer.
//
// GL entry points are resolved with dlsym rather than pulled from a header so
// that one code path serves both the legacy 2.1 profile (what
// RETRO_HW_CONTEXT_OPENGL gets on macOS) and the 3.2 core profile
// (RETRO_HW_CONTEXT_OPENGL_CORE, which the driver reports as 4.1).

#define GL_SILENCE_DEPRECATION 1   // CGL is the only offscreen GL macOS offers

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OpenGL/OpenGL.h>
#include "hwgl.h"

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLbitfield;
typedef unsigned char GLboolean;
typedef float         GLfloat;

#define GL_TEXTURE_2D                 0x0DE1
#define GL_UNSIGNED_BYTE              0x1401
#define GL_RGBA                       0x1908
#define GL_RGBA8                      0x8058
#define GL_BGRA                       0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV   0x8367
#define GL_NEAREST                    0x2600
#define GL_TEXTURE_MAG_FILTER         0x2800
#define GL_TEXTURE_MIN_FILTER         0x2801
#define GL_PACK_ALIGNMENT             0x0D05
#define GL_COLOR_BUFFER_BIT           0x00004000
#define GL_DEPTH_BUFFER_BIT           0x00000100
#define GL_FRAMEBUFFER                0x8D40
#define GL_RENDERBUFFER               0x8D41
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_DEPTH_ATTACHMENT           0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT   0x821A
#define GL_DEPTH_COMPONENT24          0x81A6
#define GL_DEPTH24_STENCIL8           0x88F0
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define GL_PIXEL_PACK_BUFFER          0x88EB
#define GL_STREAM_READ                0x88E1
#define GL_MAP_READ_BIT               0x0001

static void (*p_glGenTextures)(GLsizei, GLuint *);
static void (*p_glBindTexture)(GLenum, GLuint);
static void (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                              GLenum, GLenum, const void *);
static void (*p_glTexParameteri)(GLenum, GLenum, GLint);
static void (*p_glDeleteTextures)(GLsizei, const GLuint *);
static void (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void (*p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (*p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (*p_glBindRenderbuffer)(GLenum, GLuint);
static void (*p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*p_glDeleteRenderbuffers)(GLsizei, const GLuint *);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*p_glPixelStorei)(GLenum, GLint);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glFinish)(void);
static const unsigned char *(*p_glGetString)(GLenum);

// Pixel-buffer readback is optional: loaded soft so a context without it just
// keeps using the blocking path.
static void (*p_glGenBuffers)(GLsizei, GLuint *);
static void (*p_glBindBuffer)(GLenum, GLuint);
static void (*p_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void *(*p_glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static GLboolean (*p_glUnmapBuffer)(GLenum);
static void (*p_glDeleteBuffers)(GLsizei, const GLuint *);

#define GL_VENDOR                     0x1F00
#define GL_RENDERER                   0x1F01
#define GL_VERSION                    0x1F02

static struct retro_hw_render_callback hw;
static bool       requested;
static CGLContextObj ctx;
static GLuint     fbo, color_tex, depth_rb;
static unsigned   fbo_w, fbo_h;
static uint8_t   *readback;
static size_t     readback_cap;
static GLuint     pbo[2];
static unsigned   pbo_w[2], pbo_h[2];   // geometry queued into each buffer
static int        pbo_next;              // buffer this frame's read goes into
static bool       pbo_primed;            // the other buffer holds a queued read
static bool       async_want = true;
static bool       async_ok;              // entry points present and buffers live
static char       errbuf[256];
static char       gl_desc[256];

static void seterr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
}

// Tries the plain name, then the ARB and EXT spellings the 2.1 profile may
// export instead.
static void *gl_sym(const char *name) {
    char buf[128];
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    snprintf(buf, sizeof buf, "%sARB", name);
    if ((p = dlsym(RTLD_DEFAULT, buf))) return p;
    snprintf(buf, sizeof buf, "%sEXT", name);
    return dlsym(RTLD_DEFAULT, buf);
}

#define LOAD(fn) do {                                       \
        *(void **)&p_##fn = gl_sym(#fn);                    \
        if (!p_##fn) { seterr("missing %s", #fn); return -1; } \
    } while (0)

static int load_gl(void) {
    LOAD(glGenTextures);
    LOAD(glBindTexture);
    LOAD(glTexImage2D);
    LOAD(glTexParameteri);
    LOAD(glDeleteTextures);
    LOAD(glGenFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glFramebufferRenderbuffer);
    LOAD(glCheckFramebufferStatus);
    LOAD(glDeleteFramebuffers);
    LOAD(glGenRenderbuffers);
    LOAD(glBindRenderbuffer);
    LOAD(glRenderbufferStorage);
    LOAD(glDeleteRenderbuffers);
    LOAD(glViewport);
    LOAD(glReadPixels);
    LOAD(glPixelStorei);
    LOAD(glClearColor);
    LOAD(glClear);
    LOAD(glFinish);
    LOAD(glGetString);

    *(void **)&p_glGenBuffers      = gl_sym("glGenBuffers");
    *(void **)&p_glBindBuffer      = gl_sym("glBindBuffer");
    *(void **)&p_glBufferData      = gl_sym("glBufferData");
    *(void **)&p_glMapBufferRange  = gl_sym("glMapBufferRange");
    *(void **)&p_glUnmapBuffer     = gl_sym("glUnmapBuffer");
    *(void **)&p_glDeleteBuffers   = gl_sym("glDeleteBuffers");
    return 0;
}

static bool pbo_supported(void) {
    return p_glGenBuffers && p_glBindBuffer && p_glBufferData &&
           p_glMapBufferRange && p_glUnmapBuffer && p_glDeleteBuffers;
}

bool hwgl_available(void) { return true; }
bool hwgl_requested(void) { return requested; }
void hwgl_set_async(bool on) { async_want = on; if (!on) async_ok = false; }
bool hwgl_async_active(void) { return async_ok; }
const char *hwgl_error(void) { return errbuf[0] ? errbuf : "no error"; }
const char *hwgl_info(void) { return gl_desc[0] ? gl_desc : "no context"; }

static uintptr_t get_current_framebuffer(void) { return fbo; }

static retro_proc_address_t get_proc_address(const char *sym) {
    return (retro_proc_address_t)(uintptr_t)gl_sym(sym);
}

bool hwgl_set_callback(struct retro_hw_render_callback *cb) {
    if (cb->context_type != RETRO_HW_CONTEXT_OPENGL &&
        cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
        seterr("unsupported hw context type %u", (unsigned)cb->context_type);
        return false;
    }
    // macOS caps the core profile at 4.1; anything above that cannot be served.
    if (cb->context_type == RETRO_HW_CONTEXT_OPENGL_CORE &&
        (cb->version_major > 4 ||
         (cb->version_major == 4 && cb->version_minor > 1))) {
        seterr("core wants GL %u.%u, macOS tops out at 4.1",
               cb->version_major, cb->version_minor);
        return false;
    }
    cb->get_current_framebuffer = get_current_framebuffer;
    cb->get_proc_address        = get_proc_address;
    hw = *cb;
    requested = true;
    return true;
}

static int make_context(void) {
    bool core_profile = hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)(core_profile ? kCGLOGLPVersion_3_2_Core
                                               : kCGLOGLPVersion_Legacy),
        kCGLPFAAccelerated,
        kCGLPFAColorSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,   (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAStencilSize, (CGLPixelFormatAttribute)8,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = NULL;
    GLint nvirt = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &pix, &nvirt);
    if (err != kCGLNoError || !pix) {
        seterr("CGLChoosePixelFormat: %s", CGLErrorString(err));
        return -1;
    }
    err = CGLCreateContext(pix, NULL, &ctx);
    CGLDestroyPixelFormat(pix);
    if (err != kCGLNoError || !ctx) {
        seterr("CGLCreateContext: %s", CGLErrorString(err));
        return -1;
    }
    if ((err = CGLSetCurrentContext(ctx)) != kCGLNoError) {
        seterr("CGLSetCurrentContext: %s", CGLErrorString(err));
        return -1;
    }
    return 0;
}

// (Re)sizes the render target. The FBO name is stable so a core that cached
// get_current_framebuffer stays valid across a growth.
static int alloc_target(unsigned w, unsigned h) {
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (fbo && w <= fbo_w && h <= fbo_h) return 0;
    if (w < fbo_w) w = fbo_w;
    if (h < fbo_h) h = fbo_h;

    if (!fbo) p_glGenFramebuffers(1, &fbo);
    if (!color_tex) p_glGenTextures(1, &color_tex);
    if (!depth_rb && (hw.depth || hw.stencil)) p_glGenRenderbuffers(1, &depth_rb);

    p_glBindTexture(GL_TEXTURE_2D, color_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glBindTexture(GL_TEXTURE_2D, 0);

    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, color_tex, 0);

    if (depth_rb) {
        bool packed = hw.stencil;
        p_glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER,
                                packed ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                                (GLsizei)w, (GLsizei)h);
        p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                    packed ? GL_DEPTH_STENCIL_ATTACHMENT
                                           : GL_DEPTH_ATTACHMENT,
                                    GL_RENDERBUFFER, depth_rb);
    }

    GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        seterr("framebuffer incomplete (0x%04x) at %ux%u", status, w, h);
        return -1;
    }

    p_glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    fbo_w = w;
    fbo_h = h;

    size_t need = (size_t)w * h * 4;
    if (need > readback_cap) {
        uint8_t *nb = realloc(readback, need);
        if (!nb) { seterr("out of memory for %zu byte readback", need); return -1; }
        readback = nb;
        readback_cap = need;
    }

    // Buffers are resized with the target, so anything queued against the old
    // dimensions is gone and the pipeline has to refill.
    if (async_want && pbo_supported()) {
        if (!pbo[0]) p_glGenBuffers(2, pbo);
        for (int i = 0; i < 2; i++) {
            p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
            p_glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)need, NULL,
                           GL_STREAM_READ);
        }
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        async_ok = true;
    }
    pbo_next = 0;
    pbo_primed = false;
    return 0;
}

int hwgl_init(unsigned w, unsigned h) {
    if (!requested) return -1;
    if (make_context() != 0) return -1;
    if (load_gl() != 0) return -1;

    const unsigned char *ver = p_glGetString(GL_VERSION);
    const unsigned char *ren = p_glGetString(GL_RENDERER);
    snprintf(gl_desc, sizeof gl_desc, "%s / %s", ver ? (const char *)ver : "?",
             ren ? (const char *)ren : "?");
    if (!ver) { seterr("no GL context is current"); return -1; }

    p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    return alloc_target(w, h);
}

void hwgl_notify_reset(void) {
    if (requested && ctx && hw.context_reset) hw.context_reset();
}

void hwgl_bind(unsigned w, unsigned h) {
    if (!ctx) return;
    if (alloc_target(w, h) != 0) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

const uint8_t *hwgl_read(unsigned w, unsigned h, size_t *pitch, bool *bottom_up) {
    if (!ctx || !fbo || !readback) return NULL;
    if (w > fbo_w) w = fbo_w;
    if (h > fbo_h) h = fbo_h;
    if (w == 0 || h == 0) return NULL;

    *pitch = (size_t)w * 4;
    *bottom_up = hw.bottom_left_origin;
    size_t need = (size_t)w * h * 4;

    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // BGRA/8_8_8_8_REV lands in memory as B,G,R,X - libretro's XRGB8888.
    if (async_ok) {
        int prev = pbo_next ^ 1;
        // Only usable when the queued frame matches what the caller is about to
        // interpret; a geometry change falls back to a blocking read once.
        bool usable = pbo_primed && pbo_w[prev] == w && pbo_h[prev] == h;

        // Start this frame's transfer without waiting on it.
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pbo_next]);
        p_glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA,
                       GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        pbo_w[pbo_next] = w;
        pbo_h[pbo_next] = h;
        pbo_next = prev;
        pbo_primed = true;

        if (usable) {
            // Collect the transfer queued last frame, which the GPU has had a
            // full frame of emulation to finish.
            p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[prev]);
            void *p = p_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                         (GLsizeiptr)need, GL_MAP_READ_BIT);
            if (p) {
                memcpy(readback, p, need);
                p_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                return readback;
            }
        }
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    p_glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA,
                   GL_UNSIGNED_INT_8_8_8_8_REV, readback);
    return readback;
}

void hwgl_shutdown(void) {
    if (!ctx) return;
    CGLSetCurrentContext(ctx);
    if (hw.context_destroy) hw.context_destroy();
    if (pbo[0] && p_glDeleteBuffers) p_glDeleteBuffers(2, pbo);
    if (depth_rb) p_glDeleteRenderbuffers(1, &depth_rb);
    if (color_tex) p_glDeleteTextures(1, &color_tex);
    if (fbo) p_glDeleteFramebuffers(1, &fbo);
    fbo = color_tex = depth_rb = pbo[0] = pbo[1] = 0;
    async_ok = false;
    pbo_primed = false;
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    ctx = NULL;
    free(readback);
    readback = NULL;
    readback_cap = 0;
}
