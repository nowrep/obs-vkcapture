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

#define P_EGL_HEIGHT 0x3056
#define P_EGL_WIDTH 0x3057
#define P_EGL_GL_TEXTURE_2D 0x30B1

struct egl_funcs {
    void *(*GetProcAddress)(const char*);
    void *(*CreateContext)(void *display, void *config, void *share_context, const intptr_t *attrib_list);
    unsigned (*DestroyContext)(void *display, void *context);
    void *(*GetCurrentContext)();
    void *(*CreateImage)(void *display, void *context, unsigned target, intptr_t buffer, const intptr_t *attrib_list);
    unsigned (*DestroyImage)(void *display, void *image);
    unsigned (*QuerySurface)(void *display, void *surface, int attribute, int *value);
    unsigned (*SwapBuffers)(void *display, void *surface);
    unsigned (*ExportDMABUFImageQueryMESA)(void *dpy, void *image, int *fourcc, int *num_planes, uint64_t *modifiers);
    unsigned (*ExportDMABUFImageMESA)(void *dpy, void *image, int *fds, int *strides, int *offsets);

    bool valid;
};
