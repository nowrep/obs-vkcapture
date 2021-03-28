/*
OBS Linux Vulkan/OpenGL game capture
Copyright (C) 2021 David Rosca <nowrep@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "glinject.h"
#include "capture.h"
#include "utils.h"
#include "glad/glad.h"
#include "plugin-macros.h"

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static bool gl_seen = false;
static struct egl_funcs egl_f;
static struct glx_funcs glx_f;
static struct x11_funcs x11_f;

struct gl_data {
    void *display;
    void *surface;
    int width;
    int height;
    GLuint fbo;
    GLuint texture;
    void *image;
    int buf_fourcc;
    int buf_offset;
    int buf_stride;
    uint64_t buf_modifiers;
    int buf_fd;

    bool glx;
    void *xcb_con;
    Pixmap xpixmap;
    void *glxpixmap;
};
static struct gl_data data;

#define GETADDR(s, p, func) \
    p.func = (typeof(p.func))dlsym(handle, #s #func); \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETEGLADDR(func) GETADDR(egl, egl_f, func)
#define GETGLXADDR(func) GETADDR(glX, glx_f, func)

#define GETPROCADDR(s, p, func) \
    p.func = (typeof(p.func))p.GetProcAddress(#s #func); \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETEGLPROCADDR(func) GETPROCADDR(egl, egl_f, func)
#define GETGLXPROCADDR(func) GETPROCADDR(glX, glx_f, func)

#define GETXADDR(func) \
    x11_f.func = (typeof(x11_f.func))dlsym(handle, #func); \
    if (!x11_f.func) { \
        hlog("Failed to resolve " #func); \
        return false; \
    } \

static bool gl_init_funcs(bool glx)
{
    if (gl_seen) {
        return glx ? glx_f.valid && x11_f.valid : egl_f.valid;
    }

    hlog("Init %s", PLUGIN_VERSION);

    gl_seen = true;
    egl_f.valid = false;
    glx_f.valid = false;
    x11_f.valid = false;

    capture_init();
    memset(&data, 0, sizeof(struct gl_data));
    data.buf_fd = -1;
    data.glx = glx;

    if (glx) {
        void *handle = dlopen("libGL.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libGL.so.1");
            return false;
        }
        GETGLXADDR(GetProcAddress);
        GETGLXADDR(GetProcAddressARB);
        GETGLXPROCADDR(DestroyContext);
        GETGLXPROCADDR(SwapBuffers);
        GETGLXPROCADDR(SwapBuffersMscOML);
        GETGLXPROCADDR(CreatePixmap);
        GETGLXPROCADDR(DestroyPixmap);
        GETGLXPROCADDR(ChooseFBConfig);
        GETGLXPROCADDR(BindTexImageEXT);
        GETGLXPROCADDR(QueryDrawable);
        GETGLXPROCADDR(ChooseVisual);
        glx_f.valid = true;

        handle = dlopen("libX11.so.6", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libX11.so.6");
            return false;
        }
        GETXADDR(XCreatePixmap);
        GETXADDR(XFreePixmap);
        GETXADDR(XFree);

        handle = dlopen("libxcb.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libxcb.so.1");
            return false;
        }
        GETXADDR(xcb_connect);
        GETXADDR(xcb_disconnect);

        handle = dlopen("libxcb-dri3.so.0", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libxcb-dri3.so.0");
            return false;
        }
        GETXADDR(xcb_dri3_buffer_from_pixmap);
        GETXADDR(xcb_dri3_buffer_from_pixmap_reply);
        GETXADDR(xcb_dri3_buffer_from_pixmap_reply_fds);
        x11_f.valid = true;
    } else {
        void *handle = dlopen("libEGL.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libEGL.so.1");
            return false;
        }
        GETEGLADDR(GetProcAddress);
        GETEGLADDR(DestroyContext);
        GETEGLADDR(GetCurrentContext);
        GETEGLADDR(CreateImage);
        GETEGLADDR(DestroyImage);
        GETEGLADDR(QuerySurface);
        GETEGLADDR(SwapBuffers);
        GETEGLPROCADDR(ExportDMABUFImageQueryMESA);
        GETEGLPROCADDR(ExportDMABUFImageMESA);
        egl_f.valid = true;
    }

    gladLoadGL();
    return true;
}

#undef GETADDR
#undef GETEGLADDR
#undef GETPROCADDR
#undef GETEGLPROCADDR

static void querySurface(int *width, int *height)
{
    if (data.glx) {
        unsigned w, h;
        glx_f.QueryDrawable(data.display, data.surface, P_GLX_WIDTH, &w);
        glx_f.QueryDrawable(data.display, data.surface, P_GLX_HEIGHT, &h);
        *width = w;
        *height = h;
    } else {
        egl_f.QuerySurface(data.display, data.surface, P_EGL_WIDTH, width);
        egl_f.QuerySurface(data.display, data.surface, P_EGL_HEIGHT, height);
    }
}

static void gl_free()
{
    if (data.buf_fd >= 0) {
        close(data.buf_fd);
        data.buf_fd = -1;
    }

    if (data.image) {
        egl_f.DestroyImage(data.display, data.image);
        data.image = NULL;
    }

    if (data.xpixmap) {
        x11_f.XFreePixmap(data.display, data.xpixmap);
        data.xpixmap = 0;
    }

    if (data.glxpixmap) {
        glx_f.DestroyPixmap(data.display, data.glxpixmap);
        data.glxpixmap = NULL;
    }

    if (data.texture) {
        glDeleteTextures(1, &data.texture);
        data.texture = 0;
    }

    if (data.fbo) {
        glDeleteFramebuffers(1, &data.fbo);
        data.fbo = 0;
    }

    if (data.xcb_con) {
        x11_f.xcb_disconnect(data.xcb_con);
        data.xcb_con = NULL;
    }

    capture_stop();

    hlog("------------------- opengl capture freed -------------------");
}

static void gl_copy_backbuffer(GLuint dst)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, data.fbo);
    glBindTexture(GL_TEXTURE_2D, dst);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    glReadBuffer(GL_BACK);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, data.width, data.height, 0, 0, data.width, data.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

static void gl_shtex_capture()
{
    GLint last_fbo;
    GLint last_tex;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &last_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);

    gl_copy_backbuffer(data.texture);

    glBindTexture(GL_TEXTURE_2D, last_tex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, last_fbo);
}

static bool gl_shtex_init()
{
    glGenFramebuffers(1, &data.fbo);
    if (data.fbo == 0) {
        hlog("Failed to initialize FBO");
        return false;
    }

    glGenTextures(1, &data.texture);
    glBindTexture(GL_TEXTURE_2D, data.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (data.glx) {
        data.xcb_con = x11_f.xcb_connect(NULL, NULL);
        Window root = DefaultRootWindow(data.display);
        data.xpixmap = x11_f.XCreatePixmap(data.display, root, data.width, data.height, 24);

        const int pixmap_config[] = {
            P_GLX_BIND_TO_TEXTURE_RGBA_EXT, true,
            P_GLX_DRAWABLE_TYPE, P_GLX_PIXMAP_BIT,
            P_GLX_BIND_TO_TEXTURE_TARGETS_EXT, P_GLX_TEXTURE_2D_BIT_EXT,
            P_GLX_Y_INVERTED_EXT, true,
            P_GLX_DOUBLEBUFFER, false,
            P_GLX_RED_SIZE, 8,
            P_GLX_GREEN_SIZE, 8,
            P_GLX_BLUE_SIZE, 8,
            P_GLX_ALPHA_SIZE, 8,
            0
        };
        int nelements;
        void **fbc = glx_f.ChooseFBConfig(data.display, DefaultScreen(data.display), pixmap_config, &nelements);
        if (nelements <= 0) {
            hlog("Failed to choose FBConfig");
            return false;
        }

        const int pixmapAttribs[] = {
            P_GLX_TEXTURE_TARGET_EXT, P_GLX_TEXTURE_2D_EXT,
            P_GLX_TEXTURE_FORMAT_EXT, P_GLX_TEXTURE_FORMAT_RGBA_EXT,
            P_GLX_MIPMAP_TEXTURE_EXT, false,
            0
        };
        data.glxpixmap = glx_f.CreatePixmap(data.display, fbc[0], data.xpixmap, pixmapAttribs);
        x11_f.XFree(fbc);

        glx_f.BindTexImageEXT(data.display, data.glxpixmap, P_GLX_FRONT_LEFT_EXT, NULL);

        void *cookie = x11_f.xcb_dri3_buffer_from_pixmap(data.xcb_con, data.xpixmap);
        P_xcb_dri3_buffer_from_pixmap_reply_t *reply = x11_f.xcb_dri3_buffer_from_pixmap_reply(data.xcb_con, cookie, NULL);
        if (!reply) {
            hlog("Failed to get buffer from pixmap");
            return false;
        }
        if (reply->nfd != 1) {
            hlog("Expected 1 fd, got %d", reply->nfd);
            free(reply);
            return false;
        }
        data.buf_fd = x11_f.xcb_dri3_buffer_from_pixmap_reply_fds(data.xcb_con, reply)[0];
        data.buf_stride = reply->stride;
        free(reply);
    } else {
        data.image = egl_f.CreateImage(data.display, egl_f.GetCurrentContext(), P_EGL_GL_TEXTURE_2D, data.texture, NULL);
        if (!data.image) {
            hlog("Failed to create EGL image");
            return false;
        }

        int num_planes;
        const int queried = egl_f.ExportDMABUFImageQueryMESA(data.display, data.image, &data.buf_fourcc, &num_planes, &data.buf_modifiers);
        if (!queried) {
            hlog("Failed to query dmabuf export");
            return false;
        }
        if (num_planes != 1) {
            hlog("Expected 1 plane, got %d", num_planes);
            return false;
        }

        const int exported = egl_f.ExportDMABUFImageMESA(data.display, data.image, &data.buf_fd, &data.buf_stride, &data.buf_offset);
        if (!exported) {
            hlog("Failed dmabuf export");
            return false;
        }
    }

    return true;
}

static bool gl_init(void *display, void *surface)
{
    data.display = display;
    data.surface = surface;
    querySurface(&data.width, &data.height);

    if (!gl_shtex_init()) {
        hlog("shtex init failed");
        return false;
    }

    capture_init_shtex(data.width, data.height, data.glx ? CAPTURE_FORMAT_BGRA : CAPTURE_FORMAT_RGBA,
            data.buf_stride, data.buf_offset, /*flip*/true, data.buf_fd);

    hlog("------------------ opengl capture started ------------------");

    return true;
}

static void gl_capture(void *display, void *surface)
{
    capture_update_socket();

    if (capture_should_stop()) {
        gl_free();
    }

    if (capture_should_init()) {
        if (!gl_init(display, surface)) {
            gl_free();
            hlog("gl_init failed");
        }
    }

    if (capture_ready() && data.surface == surface) {
        int width, height;
        querySurface(&width, &height);
        if (data.height != height || data.width != width) {
            if (width != 0 && height != 0) {
                gl_free();
            }
            return;
        }
        gl_shtex_capture();
    }
}

/* ======================================================================== */

void *eglGetProcAddress(const char *procName);
unsigned eglDestroyContext(void *display, void *context);
unsigned eglSwapBuffers(void *display, void *surface);

static struct {
    void *func;
    const char *name;
} egl_hooks_map[] = {
#define ADD_HOOK(fn) { #fn, (void *) fn }
    ADD_HOOK(eglGetProcAddress),
    ADD_HOOK(eglSwapBuffers),
    ADD_HOOK(eglDestroyContext)
#undef ADD_HOOK
};

void *obs_vkcapture_eglGetProcAddress(const char *name)
{
   for (int i = 0; i < sizeof(egl_hooks_map) / sizeof(egl_hooks_map[0]); ++i) {
       if (strcmp(name, egl_hooks_map[i].name) == 0) {
           return egl_hooks_map[i].func;
       }
   }
   return NULL;
}

void *eglGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return NULL;
    }
    void *func = obs_vkcapture_eglGetProcAddress(procName);
    return func ? func : egl_f.GetProcAddress(procName);
}

unsigned eglDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    gl_free();

    return egl_f.DestroyContext(display, context);
}

unsigned eglSwapBuffers(void *display, void *surface)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    gl_capture(display, surface);

    return egl_f.SwapBuffers(display, surface);
}

/* ======================================================================== */

void *glXGetProcAddress(const char *procName);
void *glXGetProcAddressARB(const char *procName);
void glXDestroyContext(void *display, void *context);
void glXSwapBuffers(void *display, void *surface);
int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder);

static struct {
    void *func;
    const char *name;
} glx_hooks_map[] = {
#define ADD_HOOK(fn) { #fn, (void *) fn }
    ADD_HOOK(glXGetProcAddress),
    ADD_HOOK(glXGetProcAddressARB),
    ADD_HOOK(glXSwapBuffers),
    ADD_HOOK(glXSwapBuffersMscOML),
    ADD_HOOK(glXDestroyContext)
#undef ADD_HOOK
};

void *obs_vkcapture_glXGetProcAddress(const char *name)
{
   for (int i = 0; i < sizeof(glx_hooks_map) / sizeof(glx_hooks_map[0]); ++i) {
       if (strcmp(name, glx_hooks_map[i].name) == 0) {
           return glx_hooks_map[i].func;
       }
   }
   return NULL;
}

void *glXGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddress(procName);
}

void *glXGetProcAddressARB(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddressARB(procName);
}

void glXDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    gl_free();

    glx_f.DestroyContext(display, context);
}

void glXSwapBuffers(void *display, void *drawable)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    gl_capture(display, drawable);

    glx_f.SwapBuffers(display, drawable);
}

int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return 0;
    }

    gl_capture(display, drawable);

    return glx_f.SwapBuffersMscOML(display, drawable, target_msc, divisor, remainder);
}
