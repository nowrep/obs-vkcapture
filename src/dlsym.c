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

#include "dlsym.h"
#include "elfhacks.h"
#include "utils.h"

#include <stdio.h>

static bool dl_seen = false;
static struct dl_funcs dl_f;

#define GETDLADDR(func) \
    ret = eh_find_sym(&libdl, #func, (void**)&dl_f.func); \
    if (ret != 0 || !dl_f.func) { \
        hlog("Failed to resolve " #func); \
        return false; \
    } \

static bool dl_init_funcs()
{
    if (dl_seen) {
        return dl_f.valid;
    }

    dl_seen = true;
    dl_f.valid = false;

    eh_obj_t libdl;
    int ret = eh_find_obj(&libdl, "*libdl.so*");
    if (ret != 0) {
        ret = eh_find_obj(&libdl, "*libc.so*");
        if (ret != 0) {
            hlog("Failed to open libdl.so and libc.so");
            return false;
        }
    }

    GETDLADDR(dlsym);
    GETDLADDR(dlvsym);
    eh_destroy_obj(&libdl);
    dl_f.valid = true;

    return true;
}

#undef GETDLADDR

void *real_dlsym(void *handle, const char *symbol)
{
    if (!dl_init_funcs()) {
        return NULL;
    }
    return dl_f.dlsym(handle, symbol);
}

void *real_dlvsym(void *handle, const char *symbol, const char *version)
{
    if (!dl_init_funcs()) {
        return NULL;
    }
    return dl_f.dlvsym(handle, symbol, version);
}

extern void *obs_vkcapture_glXGetProcAddress(const char *name);
extern void *obs_vkcapture_eglGetProcAddress(const char *name);

void *dlsym(void *handle, const char *symbol)
{
    void *real_func = real_dlsym(handle, symbol);
    if (!real_func) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(symbol);
    if (!func) {
        func = obs_vkcapture_eglGetProcAddress(symbol);
    }
    return func ? func : real_func;
}

void *dlvsym(void *handle, const char *symbol, const char *version)
{
    void *real_func = real_dlvsym(handle, symbol, version);
    if (!real_func) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(symbol);
    if (!func) {
        func = obs_vkcapture_eglGetProcAddress(symbol);
    }
    return func ? func : real_func;
}
