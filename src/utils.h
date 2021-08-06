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

#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>

static inline int os_dupfd_cloexec(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
}

#define hlog(msg, ...) fprintf(stderr, "[obs-vkcapture] " msg "\n", ##__VA_ARGS__)
