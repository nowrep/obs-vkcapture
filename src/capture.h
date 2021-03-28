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

#include <stdbool.h>

enum capture_format {
    CAPTURE_FORMAT_RGBA,
    CAPTURE_FORMAT_BGRA,
    CAPTURE_FORMAT_BGRX
};

struct capture_texture_data {
    int width;
    int height;
    int format;
    int stride;
    int offset;
    bool flip;
};

void capture_init();
void capture_update_socket();
void capture_init_shtex(int width, int height, int format, int stride, int offset, bool flip, int fd);
void capture_stop();

bool capture_should_stop();
bool capture_should_init();
bool capture_ready();
