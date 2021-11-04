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

#include "wlcursor.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <libdrm/drm_fourcc.h>

struct output_data {
    wl_cursor_t *ctx;
    uint32_t id;
    struct wl_output *output;
    struct wl_buffer *buffer;
    uint32_t buffer_width;
    uint32_t buffer_height;
    uint32_t buffer_stride;
    void *buffer_data;
    struct zext_screencopy_surface_v1 *surface;
    int32_t pos_x;
    int32_t pos_y;
    int32_t hotspot_x;
    int32_t hotspot_y;
    bool have_cursor;
    gs_texture_t *tex;
};

static void output_data_reset(struct output_data *data)
{
    if (data->buffer) {
        wl_buffer_destroy(data->buffer);
        data->buffer = NULL;
    }
    if (data->buffer_data) {
        munmap(data->buffer_data, data->buffer_stride * data->buffer_height);
        data->buffer_data = NULL;
    }
    if (data->surface) {
        zext_screencopy_surface_v1_destroy(data->surface);
        data->surface = NULL;
    }
    if (data->tex) {
        gs_texture_destroy(data->tex);
        data->tex = NULL;
    }
}

static enum wl_shm_format drm_format_to_wl_shm(uint32_t in)
{
    switch (in) {
    case DRM_FORMAT_ARGB8888:
        return WL_SHM_FORMAT_ARGB8888;
    case DRM_FORMAT_XRGB8888:
        return WL_SHM_FORMAT_XRGB8888;
    default:
        blog(LOG_ERROR, "unknown shm format %u", in);
        return in;
    }
}

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm,
        enum wl_shm_format fmt, int width, int height,
        int stride, void **data_out)
{
    int size = stride * height;

    const char shm_name[] = "/obs-vkcapture-wlshm";
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return NULL;
    }
    shm_unlink(shm_name);

    int ret;
    while ((ret = ftruncate(fd, size)) == EINTR) { }
    if (ret < 0) {
        close(fd);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    close(fd);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
            stride, fmt);
    wl_shm_pool_destroy(pool);

    *data_out = data;
    return buffer;
}

static void surface_handle_buffer_info(void *data_,
    struct zext_screencopy_surface_v1 *surface,
    enum zext_screencopy_surface_v1_buffer_type type,
    uint32_t format, uint32_t width, uint32_t height,
    uint32_t stride)
{
    struct output_data *data = data_;

    if (type == ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_NONE) {
        if (!data->buffer) {
            blog(LOG_ERROR, "no available shm buffers");
            return;
        }
        zext_screencopy_surface_v1_attach_buffer(surface, data->buffer);
        zext_screencopy_surface_v1_commit(surface,
                ZEXT_SCREENCOPY_SURFACE_V1_OPTIONS_WAIT_FOR_DAMAGE);
        return;
    }

    if (type != ZEXT_SCREENCOPY_SURFACE_V1_BUFFER_TYPE_WL_SHM) {
        return;
    }

    if (data->buffer) {
        return;
    }

    data->buffer_width = width;
    data->buffer_height = height;
    data->buffer_stride = stride;
    data->buffer = create_shm_buffer(data->ctx->shm,
            drm_format_to_wl_shm(format), data->buffer_width,
            data->buffer_height, data->buffer_stride, &data->buffer_data);
    if (!data->buffer) {
        blog(LOG_ERROR, "failed to create shm buffer");
    } else {
        data->tex = gs_texture_create(width, height, GS_BGRA, 1, NULL, GS_DYNAMIC);
    }
}

static void surface_handle_transform(void *data,
    struct zext_screencopy_surface_v1 *surface,
    int32_t transform)
{
}

static void surface_handle_ready(void *data_,
    struct zext_screencopy_surface_v1 *surface)
{
    struct output_data *data = data_;

    data->have_cursor = true;
    gs_texture_set_image(data->tex, data->buffer_data,
            data->buffer_stride, false);

    zext_screencopy_surface_v1_attach_buffer(surface, data->buffer);
    zext_screencopy_surface_v1_commit(surface,
            ZEXT_SCREENCOPY_SURFACE_V1_OPTIONS_WAIT_FOR_DAMAGE);
}

static void capture_output(struct output_data *data);

static void surface_handle_failed(void *data_,
    struct zext_screencopy_surface_v1 *surface,
    enum zext_screencopy_surface_v1_failure_reason reason)
{
    struct output_data *data = data_;

    if (reason != ZEXT_SCREENCOPY_SURFACE_V1_FAILURE_REASON_CURSOR_MISSING) {
        blog(LOG_ERROR, "failed to copy surface %d", reason);
    }

    data->have_cursor = false;
    output_data_reset(data);
    capture_output(data);
}

static void surface_handle_damage(void *data,
    struct zext_screencopy_surface_v1 *surface,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
}

static void surface_handle_cursor_info(void *data_,
    struct zext_screencopy_surface_v1 *surface,
    int32_t pos_x, int32_t pos_y,
    int32_t hotspot_x, int32_t hotspot_y)
{
    struct output_data *data = data_;

    data->pos_x = pos_x;
    data->pos_y = pos_y;
    data->hotspot_x = hotspot_x;
    data->hotspot_y = hotspot_y;
}

static void surface_handle_presentation_time(void *data,
    struct zext_screencopy_surface_v1 *surface,
    uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
}

static const struct zext_screencopy_surface_v1_listener frame_listener = {
    .buffer_info = surface_handle_buffer_info,
    .damage = surface_handle_damage,
    .cursor_info = surface_handle_cursor_info,
    .presentation_time = surface_handle_presentation_time,
    .transform = surface_handle_transform,
    .ready = surface_handle_ready,
    .failed = surface_handle_failed,
};

static void capture_output(struct output_data *data)
{
    data->surface =
        zext_screencopy_manager_v1_capture_output_cursor(data->ctx->screencopy, data->output);
    zext_screencopy_surface_v1_add_listener(data->surface, &frame_listener, data);
}

static void handle_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    wl_cursor_t *ctx = data;

    if (!strcmp(interface, wl_output_interface.name)) {
        struct output_data *output_data = bzalloc(sizeof(struct output_data));
        output_data->ctx = ctx;
        output_data->id = name;
        output_data->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        da_push_back(ctx->outputs, &output_data);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        ctx->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, zext_screencopy_manager_v1_interface.name)) {
        ctx->screencopy = wl_registry_bind(registry, name,
                &zext_screencopy_manager_v1_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
    uint32_t name)
{
    wl_cursor_t *ctx = data;

    for (size_t i = 0; i < ctx->outputs.num; ++i) {
        struct output_data *o = *(ctx->outputs.array + i);
        if (o->id == name) {
            output_data_reset(o);
            bfree(o);
            da_erase(ctx->outputs, i);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

wl_cursor_t *wl_cursor_init(struct wl_display *display)
{
    wl_cursor_t *data = bzalloc(sizeof(wl_cursor_t));

    da_init(data->outputs);

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, data);
    wl_display_roundtrip(display);

    if (!data->shm) {
        blog(LOG_ERROR, "wl_shm not available");
        wl_cursor_destroy(data);
        return NULL;
    }

    if (!data->screencopy) {
        blog(LOG_ERROR, "zext_screencopy_manager_v1 not available");
        wl_cursor_destroy(data);
        return NULL;
    }

    for (size_t i = 0; i < data->outputs.num; ++i) {
        capture_output(*(data->outputs.array + i));
    }

    return data;
}

void wl_cursor_destroy(wl_cursor_t *data)
{
    for (size_t i = 0; i < data->outputs.num; ++i) {
        struct output_data *output_data = *(data->outputs.array + i);
        output_data_reset(output_data);
        bfree(output_data);
    }
    da_free(data->outputs);
    bfree(data);
}

void wl_cursor_render(wl_cursor_t *data)
{
    struct output_data *output_data = NULL;

    for (size_t i = 0; i < data->outputs.num; ++i) {
        struct output_data *o = *(data->outputs.array + i);
        if (o->have_cursor && o->tex) {
            output_data = o;
            break;
        }
    }

    if (!output_data) {
        return;
    }

    const bool linear_srgb = gs_get_linear_srgb();

    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(linear_srgb);

    gs_effect_t *effect = gs_get_effect();
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (linear_srgb)
        gs_effect_set_texture_srgb(image, output_data->tex);
    else
        gs_effect_set_texture(image, output_data->tex);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
    gs_enable_color(true, true, true, false);

    gs_matrix_push();
    gs_matrix_translate3f(output_data->pos_x, output_data->pos_y, 0.0f);
    gs_draw_sprite(output_data->tex, 0, 0, 0);
    gs_matrix_pop();

    gs_enable_color(true, true, true, true);
    gs_blend_state_pop();

    gs_enable_framebuffer_srgb(previous);
}
