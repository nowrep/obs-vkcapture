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

#include "capture_wayland.h"
#include "utils.h"

#include <string.h>

#if HAVE_WAYLAND

static struct {
    struct wl_display *display;
    /* Actually a proxy wrapper around the event queue */
    struct wl_display *display_wrapper;
    struct wl_event_queue *queue;

    struct wl_surface *current_surface;
    struct wl_surface *capture_surface;
} wl;

static void pointer_enter(
        void *data, struct wl_pointer *wl_pointer, uint32_t serial,
        struct wl_surface *surface, wl_fixed_t surface_x,
        wl_fixed_t surface_y)
{
    wl.current_surface = surface;
}

static void pointer_leave(
        void *data, struct wl_pointer *wl_pointer, uint32_t serial,
        struct wl_surface *surface)
{
    wl.current_surface = NULL;
}

static void pointer_motion(
        void *data, struct wl_pointer *wl_pointer, uint32_t time,
        wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    if (wl.capture_surface == wl.current_surface) {
        hlog("motion %lf %lf", wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
    }
}

static void pointer_button(
        void *data, struct wl_pointer *wl_pointer, uint32_t serial,
        uint32_t time, uint32_t button, uint32_t state)
{
}

static void pointer_axis(
        void *data, struct wl_pointer *wl_pointer, uint32_t time,
        uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_handle_capabilities(
        void *data, struct wl_seat *seat, uint32_t capabilities)
{
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        struct wl_pointer *pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, seat);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
};

static void handle_global(
        void *data, struct wl_registry *registry,
        uint32_t name, const char *interface,
        uint32_t version)
{
    if (!strcmp(interface, wl_seat_interface.name)) {
        struct wl_seat *seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void handle_global_remove(
        void *data, struct wl_registry *registry,
        uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

void capture_init_wayland(struct wl_display *display)
{
    if (wl.display) {
        return;
    }

    wl.display = display;
    wl.queue = wl_display_create_queue(wl.display);
    if (!wl.queue) {
        hlog("failed to create wl queue");
        return;
    }

    wl.display_wrapper = wl_proxy_create_wrapper(wl.display);
    if (!wl.display_wrapper) {
        hlog("failed to create wl display wrapper");
        return;
    }

    wl_proxy_set_queue((struct wl_proxy*)wl.display_wrapper, wl.queue);
    struct wl_registry *registry = wl_display_get_registry(wl.display_wrapper);
    if (!registry) {
        hlog("failed to get wl registry");
        return;
    }

    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip_queue(wl.display, wl.queue);
    wl_registry_destroy(registry);
}

void capture_update_wayland()
{
    wl_display_dispatch_queue_pending(wl.display, wl.queue);
}

void capture_set_wlsurface(struct wl_surface *surface)
{
    wl.capture_surface = surface;
}

#endif
