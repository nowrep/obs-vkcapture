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

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>
#include <vulkan/vulkan.h>

struct gl_funcs {
    void *(*GetProcAddress)(const char*);
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLGENTEXTURESEXTPROC GenTextures;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLREADBUFFERPROC ReadBuffer;
    PFNGLDRAWBUFFERPROC DrawBuffer;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    PFNGLGETERRORPROC GetError;
    PFNGLGETSTRINGPROC GetString;
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC GetUnsignedBytei_vEXT;
    PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT;
    PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFdEXT;
    PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT;
    PFNGLISMEMORYOBJECTEXTPROC IsMemoryObjectEXT;
};

#define P_EGL_HEIGHT 0x3056
#define P_EGL_WIDTH 0x3057
#define P_EGL_GL_TEXTURE_2D 0x30B1

struct egl_funcs {
    void *(*GetProcAddress)(const char*);
    unsigned (*DestroyContext)(void *display, void *context);
    void *(*GetCurrentContext)();
    void *(*CreateWindowSurface)(void *display, void *config, void *win, const intptr_t *attrib_list);
    void *(*CreateImage)(void *display, void *context, unsigned target, intptr_t buffer, const intptr_t *attrib_list);
    unsigned (*DestroyImage)(void *display, void *image);
    unsigned (*QuerySurface)(void *display, void *surface, int attribute, int *value);
    unsigned (*SwapBuffers)(void *display, void *surface);
    unsigned (*ExportDMABUFImageQueryMESA)(void *dpy, void *image, int *fourcc, int *num_planes, uint64_t *modifiers);
    unsigned (*ExportDMABUFImageMESA)(void *dpy, void *image, int *fds, int *strides, int *offsets);

    bool valid;
};

#define P_GLX_WIDTH 0x801D
#define P_GLX_HEIGHT 0x801E
#define P_GLX_BIND_TO_TEXTURE_RGBA_EXT 0x20D1
#define P_GLX_DRAWABLE_TYPE 0x8010
#define P_GLX_PIXMAP_BIT 0x00000002
#define P_GLX_BIND_TO_TEXTURE_TARGETS_EXT 0x20D3
#define P_GLX_TEXTURE_2D_BIT_EXT 0x00000002
#define P_GLX_DOUBLEBUFFER 5
#define P_GLX_TEXTURE_TARGET_EXT 0x20D6
#define P_GLX_TEXTURE_2D_EXT 0x20DC
#define P_GLX_TEXTURE_FORMAT_EXT 0x20D5
#define P_GLX_TEXTURE_FORMAT_RGBA_EXT 0x20DA
#define P_GLX_FRONT_LEFT_EXT 0x20DE
#define P_GLX_RED_SIZE 8
#define P_GLX_GREEN_SIZE 9
#define P_GLX_BLUE_SIZE 10
#define P_GLX_ALPHA_SIZE 11
#define P_GLX_MIPMAP_TEXTURE_EXT 0x20D7

struct glx_funcs {
    void *(*GetProcAddress)(const char*);
    void *(*GetProcAddressARB)(const char*);
    void (*DestroyContext)(void *display, void *context);
    void (*SwapBuffers)(void *display, void *drawable);
    int64_t (*SwapBuffersMscOML)(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder);
    void *(*CreatePixmap)(void *display, void *config, unsigned long pixmap, const int *attribList);
    void (*DestroyPixmap)(void *display, void *pixmap);
    void *(*ChooseFBConfig)(void *display, int screen, const int *attribList, int *nitems);
    void (*BindTexImageEXT)(void *display, void *drawable, int buffer, const int *attribList);
    void (*QueryDrawable)(void *display, void *drawable, int attribute, unsigned *value);
    void *(*ChooseVisual)(void *display, int screen, int *attribList);

    bool valid;
};

#define P_DefaultRootWindow(dpy) (P_ScreenOfDisplay(dpy,P_DefaultScreen(dpy))->root)
#define P_ScreenOfDisplay(dpy, scr) (&((P_XPrivDisplay)(dpy))->screens[scr])
#define P_DefaultScreen(dpy) (((P_XPrivDisplay)(dpy))->default_screen)

typedef struct {
    void *ext_data;
    void *display;
    unsigned long root;
} P_Screen;

typedef struct
{
    void *ext_data;
    void *private1;
    int fd;
    int private2;
    int proto_major_version;
    int proto_minor_version;
    char *vendor;
    unsigned long private3;
    unsigned long private4;
    unsigned long private5;
    int private6;
    void *resource_alloc;
    int byte_order;
    int bitmap_unit;
    int bitmap_pad;
    int bitmap_bit_order;
    int nformats;
    void *pixmap_format;
    int private8;
    int release;
    void *private9, *private10;
    int qlen;
    unsigned long last_request_read;
    unsigned long request;
    char *private11;
    char *private12;
    char *private13;
    char *private14;
    unsigned max_request_size;
    void *db;
    void *private15;
    char *display_name;
    int default_screen;
    int nscreens;
    P_Screen *screens;
}
*P_XPrivDisplay;

typedef struct P_xcb_dri3_buffers_from_pixmap_reply_t {
    uint8_t  response_type;
    uint8_t  nfd;
    uint16_t sequence;
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t  pad0[4];
    uint64_t modifier;
    uint8_t  depth;
    uint8_t  bpp;
    uint8_t  pad1[6];
} P_xcb_dri3_buffers_from_pixmap_reply_t;

typedef struct P_xcb_dri3_buffers_from_pixmap_cookie_t {
    unsigned int sequence;
} P_xcb_dri3_buffers_from_pixmap_cookie_t;

struct x11_funcs {
    unsigned long (*XCreatePixmap)(void *display, unsigned long drawable, unsigned width, unsigned height, unsigned depth);
    int (*XFreePixmap)(void *display, unsigned long pixmap);
    int (*XFree)(void *data);

    void *(*XGetXCBConnection)(void *display);
    P_xcb_dri3_buffers_from_pixmap_cookie_t (*xcb_dri3_buffers_from_pixmap)(void *c, unsigned long pixmap);
    P_xcb_dri3_buffers_from_pixmap_reply_t *(*xcb_dri3_buffers_from_pixmap_reply)(void *c, P_xcb_dri3_buffers_from_pixmap_cookie_t cookie, void *error);
    int *(*xcb_dri3_buffers_from_pixmap_reply_fds)(void *c, P_xcb_dri3_buffers_from_pixmap_reply_t *reply);
    uint32_t *(*xcb_dri3_buffers_from_pixmap_strides)(P_xcb_dri3_buffers_from_pixmap_reply_t *reply);
    uint32_t *(*xcb_dri3_buffers_from_pixmap_offsets)(P_xcb_dri3_buffers_from_pixmap_reply_t *reply);

    bool valid;
};

struct vk_funcs {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkDestroyDevice DestroyDevice;

    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties2KHR GetPhysicalDeviceFormatProperties2KHR;
    PFN_vkGetPhysicalDeviceImageFormatProperties2KHR GetPhysicalDeviceImageFormatProperties2KHR;

    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkGetImageMemoryRequirements2KHR GetImageMemoryRequirements2KHR;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT GetImageDrmFormatModifierPropertiesEXT;
    PFN_vkBindImageMemory2KHR BindImageMemory2KHR;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;

    bool valid;
};
