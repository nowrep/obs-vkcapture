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
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static inline int os_dupfd_cloexec(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
}

static inline int64_t os_time_get_nano(void)
{
    struct timespec tv;
    clock_gettime(
#if defined(CLOCK_MONOTONIC_RAW)
        CLOCK_MONOTONIC_RAW
#elif defined(CLOCK_MONOTONIC_FAST)
        CLOCK_MONOTONIC_FAST
#else
        CLOCK_MONOTONIC
#endif
        , &tv);
    return tv.tv_nsec + tv.tv_sec * INT64_C(1000000000);
}

static bool hlog_quiet(void)
{
    static int quiet = -1;
    if (quiet == -1) {
        const char *q = getenv("OBS_VKCAPTURE_QUIET");
        quiet = q && atoi(q) == 1;
    }
    return quiet;
}

#define hlog(msg, ...) if (!hlog_quiet()) fprintf(stderr, "[obs-vkcapture] " msg "\n", ##__VA_ARGS__)
