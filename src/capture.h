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
#include <assert.h>

#ifndef DRM_FORMAT_XRGB8888
#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
        ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_ARGB8888 fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888 fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_ABGR8888 fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_XRGB2101010 fourcc_code('X', 'R', '3', '0')
#define DRM_FORMAT_ARGB2101010 fourcc_code('A', 'R', '3', '0')
#define DRM_FORMAT_XBGR2101010 fourcc_code('X', 'B', '3', '0')
#define DRM_FORMAT_ABGR2101010 fourcc_code('A', 'B', '3', '0')
#define DRM_FORMAT_XBGR16161616 fourcc_code('X', 'B', '4', '8')
#define DRM_FORMAT_ABGR16161616 fourcc_code('A', 'B', '4', '8')
#define DRM_FORMAT_XBGR16161616F fourcc_code('X', 'B', '4', 'H')
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H')
#define fourcc_mod_code(vendor, val) ((((uint64_t)vendor) << 56) | ((val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_INVALID fourcc_mod_code(0, ((1ULL << 56) - 1))
#define DRM_FORMAT_MOD_LINEAR fourcc_mod_code(0, 0)
#define DRM_FORMAT_MOD_VENDOR_AMD 0x02
#define AMD_FMT_MOD_DCC_SHIFT 13
#define AMD_FMT_MOD_DCC_MASK 0x1
#define IS_AMD_FMT_MOD(val) (((val) >> 56) == DRM_FORMAT_MOD_VENDOR_AMD)
#define AMD_FMT_MOD_GET(field, value) (((value) >> AMD_FMT_MOD_##field##_SHIFT) & AMD_FMT_MOD_##field##_MASK)
#endif

struct capture_client_data {
    uint8_t type;
    char exe[48];
    uint8_t padding[79];
} __attribute__((packed));

#define CAPTURE_CLIENT_DATA_TYPE 10
#define CAPTURE_CLIENT_DATA_SIZE 128
static_assert(sizeof(struct capture_client_data) == CAPTURE_CLIENT_DATA_SIZE, "size mismatch");

struct capture_texture_data {
    uint8_t type;
    uint8_t nfd;
    int32_t width;
    int32_t height;
    int32_t format;
    int32_t strides[4];
    int32_t offsets[4];
    uint64_t modifier;
    uint32_t winid;
    uint8_t flip;
    uint32_t color_space;
    uint8_t padding[65];
} __attribute__((packed));

#define CAPTURE_TEXTURE_DATA_TYPE 11
#define CAPTURE_TEXTURE_DATA_SIZE 128
static_assert(sizeof(struct capture_texture_data) == CAPTURE_TEXTURE_DATA_SIZE, "size mismatch");

struct capture_control_data {
    uint8_t capturing;
    uint8_t no_modifiers;
    uint8_t linear;
    uint8_t map_host;
    uint8_t device_uuid[16];
    uint8_t padding[12];
} __attribute__((packed));

#define CAPTURE_CONTROL_DATA_TYPE 10
#define CAPTURE_CONTROL_DATA_SIZE 32
static_assert(sizeof(struct capture_control_data) == CAPTURE_CONTROL_DATA_SIZE, "size mismatch");

void capture_init();
void capture_update_socket();
void capture_init_shtex(
        int width, int height, int format, int strides[4],
        int offsets[4], uint64_t modifier, uint32_t winid,
        bool flip, uint32_t color_space, int nfd, int fds[4]);
void capture_stop();

bool capture_should_stop();
bool capture_should_init();
bool capture_ready();

bool capture_allocate_no_modifiers();
bool capture_allocate_linear();
bool capture_allocate_map_host();

bool capture_compare_device_uuid(uint8_t uuid[16]);
