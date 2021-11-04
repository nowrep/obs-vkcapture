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

#include <obs.h>
#include <wayland-client.h>

#include "screencopy_unstable_v1.h"

typedef struct {
    struct wl_shm *shm;
    struct zext_screencopy_manager_v1 *screencopy;
    DARRAY(struct output_data*) outputs;
} wl_cursor_t;

wl_cursor_t *wl_cursor_init(struct wl_display *display);
void wl_cursor_destroy(wl_cursor_t *data);
void wl_cursor_render(wl_cursor_t *data);
