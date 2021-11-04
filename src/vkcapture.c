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

#define _GNU_SOURCE

#include <obs-module.h>
#include <obs-nix-platform.h>

#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#include "utils.h"
#include "capture.h"
#include "plugin-macros.h"

#if HAVE_X11_XCB
#include "xcursor-xcb.h"
#endif

typedef struct {
    int id;
    int sockfd;
    int activated;
    int buf_id;
    int buf_fds[4];
    struct capture_client_data cdata;
    struct capture_texture_data tdata;
} vkcapture_client_t;

static struct {
    int quitfd;
    pthread_t thread;
    pthread_mutex_t mutex;
    DARRAY(struct pollfd) fds;
    DARRAY(vkcapture_client_t) clients;
} server;

typedef struct {
    obs_source_t *source;
    gs_texture_t *texture;
#if HAVE_X11_XCB
    xcb_connection_t *xcb;
    xcb_xcursor_t *xcursor;
    uint32_t root_winid;
#endif
    bool show_cursor;
    bool allow_transparency;
    const char *window;

    int buf_id;
    int client_id;
    struct capture_texture_data tdata;

} vkcapture_source_t;

static void cursor_create(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (obs_get_nix_platform() == OBS_NIX_PLATFORM_X11_EGL) {
        ctx->xcb = xcb_connect(NULL, NULL);
        if (!ctx->xcb || xcb_connection_has_error(ctx->xcb)) {
            blog(LOG_ERROR, "Unable to open X display !");
        } else {
            ctx->xcursor = xcb_xcursor_init(ctx->xcb);
        }
    }
#endif
}

static void cursor_destroy(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        obs_enter_graphics();
        xcb_xcursor_destroy(ctx->xcursor);
        obs_leave_graphics();
    }
    if (ctx->xcb) {
        xcb_disconnect(ctx->xcb);
    }
#endif
}

static bool cursor_enabled(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    return ctx->xcursor;
#endif
    return false;
}

static void cursor_tick(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        if (!ctx->root_winid && ctx->tdata.winid) {
            xcb_query_tree_cookie_t tre_c = xcb_query_tree_unchecked(ctx->xcb, ctx->tdata.winid);
            xcb_query_tree_reply_t *tre_r = xcb_query_tree_reply(ctx->xcb, tre_c, NULL);
            if (tre_r) {
                ctx->root_winid = tre_r->root;
                free(tre_r);
            }
        }
        xcb_translate_coordinates_cookie_t tr_c;
        if (ctx->root_winid && ctx->tdata.winid) {
            tr_c = xcb_translate_coordinates_unchecked(ctx->xcb, ctx->tdata.winid, ctx->root_winid, 0, 0);
        }
        xcb_xfixes_get_cursor_image_cookie_t cur_c = xcb_xfixes_get_cursor_image_unchecked(ctx->xcb);
        xcb_xfixes_get_cursor_image_reply_t *cur_r = xcb_xfixes_get_cursor_image_reply(ctx->xcb, cur_c, NULL);
        if (ctx->root_winid && ctx->tdata.winid) {
            xcb_translate_coordinates_reply_t *tr_r = xcb_translate_coordinates_reply(ctx->xcb, tr_c, NULL);
            if (tr_r) {
                xcb_xcursor_offset(ctx->xcursor, tr_r->dst_x, tr_r->dst_y);
                free(tr_r);
            }
        }
        obs_enter_graphics();
        xcb_xcursor_update(ctx->xcursor, cur_r);
        obs_leave_graphics();
        free(cur_r);
    }
#endif
}

static void cursor_render(vkcapture_source_t *ctx)
{
#if HAVE_X11_XCB
    if (ctx->xcursor) {
        xcb_xcursor_render(ctx->xcursor);
    }
#endif
}

static void destroy_texture(vkcapture_source_t *ctx)
{
    if (!ctx->texture) {
        return;
    }

    obs_enter_graphics();
    gs_texture_destroy(ctx->texture);
    obs_leave_graphics();
    ctx->texture = NULL;

    ctx->buf_id = 0;
    memset(&ctx->tdata, 0, sizeof(ctx->tdata));
}

static void vkcapture_source_destroy(void *data)
{
    vkcapture_source_t *ctx = data;

    destroy_texture(ctx);
    cursor_destroy(ctx);

    bfree(ctx);
}

static void vkcapture_source_update(void *data, obs_data_t *settings)
{
    vkcapture_source_t *ctx = data;

    ctx->show_cursor = obs_data_get_bool(settings, "show_cursor");
    ctx->allow_transparency = obs_data_get_bool(settings, "allow_transparency");

    ctx->window = obs_data_get_string(settings, "window");
    if (!strlen(ctx->window)) {
        ctx->window = NULL;
    }
}

static void *vkcapture_source_create(obs_data_t *settings, obs_source_t *source)
{
    vkcapture_source_t *ctx = bzalloc(sizeof(vkcapture_source_t));
    ctx->source = source;

    vkcapture_source_update(ctx, settings);

    cursor_create(ctx);

    UNUSED_PARAMETER(settings);
    return ctx;
}

static vkcapture_client_t *find_matching_client(vkcapture_source_t *ctx)
{
    vkcapture_client_t *client = NULL;
    if (ctx->window) {
        for (size_t i = 0; i < server.clients.num; i++) {
            vkcapture_client_t *c = server.clients.array + i;
            if (!strcmp(c->cdata.exe, ctx->window)) {
                client = c;
                break;
            }
        }
    } else if (server.clients.num) {
        client = server.clients.array;
    }
    return client;
}

static vkcapture_client_t *find_client_by_id(int id)
{
    vkcapture_client_t *client = NULL;
    for (size_t i = 0; i < server.clients.num; i++) {
        vkcapture_client_t *c = server.clients.array + i;
        if (c->id == id) {
            client = c;
            break;
        }
    }
    return client;
}

static void activate_client(vkcapture_client_t *client, bool activate)
{
    char msg;
    if (activate && !client->activated++) {
        msg = '1';
    } else if (!activate && !--client->activated) {
        msg = '0';
    } else {
        return;
    }
    client->buf_id = 0;
    for (int i = 0; i < 4; ++i) {
        if (client->buf_fds[i] >= 0) {
            close(client->buf_fds[i]);
            client->buf_fds[i] = -1;
        }
    }
    memset(&client->tdata, 0, sizeof(client->tdata));
    ssize_t ret = write(client->sockfd, &msg, 1);
    if (ret != 1) {
        blog(LOG_WARNING, "Socket write error: %s", strerror(errno));
    }
}

static void vkcapture_source_show(void *data)
{
    vkcapture_source_t *ctx = data;

    if (ctx->client_id) {
        pthread_mutex_lock(&server.mutex);
        vkcapture_client_t *client = find_client_by_id(ctx->client_id);
        if (client) {
            activate_client(client, true);
        }
        pthread_mutex_unlock(&server.mutex);
    }
}

static void vkcapture_source_hide(void *data)
{
    vkcapture_source_t *ctx = data;

    if (ctx->client_id) {
        pthread_mutex_lock(&server.mutex);
        vkcapture_client_t *client = find_client_by_id(ctx->client_id);
        if (client) {
            activate_client(client, false);
            destroy_texture(ctx);
        }
        pthread_mutex_unlock(&server.mutex);
    }
}

static void vkcapture_source_video_tick(void *data, float seconds)
{
    vkcapture_source_t *ctx = data;

    if (!obs_source_showing(ctx->source)) {
        return;
    }

    if (ctx->texture && ctx->show_cursor) {
        cursor_tick(ctx);
    }

    pthread_mutex_lock(&server.mutex);

    if (ctx->client_id) {
        vkcapture_client_t *client = find_client_by_id(ctx->client_id);
        if (!client) {
            ctx->client_id = 0;
            destroy_texture(ctx);
        } else if (ctx->buf_id != client->buf_id) {
            destroy_texture(ctx);
            memcpy(&ctx->tdata, &client->tdata, sizeof(client->tdata));

            blog(LOG_INFO, "Creating texture from dmabuf %dx%d modifier:%" PRIu64,
                    ctx->tdata.width, ctx->tdata.height, ctx->tdata.modifier);

            uint32_t strides[4];
            uint32_t offsets[4];
            uint64_t modifiers[4];
            for (uint8_t i = 0; i < ctx->tdata.nfd; ++i) {
                strides[i] = ctx->tdata.strides[i];
                offsets[i] = ctx->tdata.offsets[i];
                modifiers[i] = ctx->tdata.modifier;
                blog(LOG_INFO, " [%d] fd:%d stride:%d offset:%d", i, client->buf_fds[i], strides[i], offsets[i]);
            }

            obs_enter_graphics();
            ctx->texture = gs_texture_create_from_dmabuf(ctx->tdata.width, ctx->tdata.height,
                    ctx->tdata.format, GS_BGRX, ctx->tdata.nfd, client->buf_fds, strides, offsets,
                    ctx->tdata.modifier != DRM_FORMAT_MOD_INVALID ? modifiers : NULL);
            obs_leave_graphics();

            if (!ctx->texture) {
                blog(LOG_ERROR, "Could not create texture from dmabuf source");
            }
            ctx->buf_id = client->buf_id;
        } else if (client != find_matching_client(ctx)) {
            activate_client(client, false);
            ctx->client_id = 0;
            destroy_texture(ctx);
        }
    } else {
        vkcapture_client_t *client = find_matching_client(ctx);
        if (client) {
            activate_client(client, true);
            ctx->client_id = client->id;
        }
    }

    pthread_mutex_unlock(&server.mutex);

    UNUSED_PARAMETER(seconds);
}

static void vkcapture_source_render(void *data, gs_effect_t *effect)
{
    vkcapture_source_t *ctx = data;

    if (!ctx->texture) {
        return;
    }

    effect = obs_get_base_effect(ctx->allow_transparency ? OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, ctx->texture);

    while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite(ctx->texture, ctx->tdata.flip ? GS_FLIP_V : 0, 0, 0);
        if (ctx->allow_transparency && ctx->show_cursor) {
            cursor_render(ctx);
        }
    }

    if (!ctx->allow_transparency && ctx->show_cursor) {
        effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        while (gs_effect_loop(effect, "Draw")) {
            cursor_render(ctx);
        }
    }
}

static const char *vkcapture_source_get_name(void *data)
{
    return obs_module_text("GameCapture");
}

static uint32_t vkcapture_source_get_width(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->tdata.width;
}

static uint32_t vkcapture_source_get_height(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->tdata.height;
}

static void vkcapture_source_get_defaults(obs_data_t *defaults)
{
    obs_data_set_default_bool(defaults, "show_cursor", true);
    obs_data_set_default_bool(defaults, "allow_transparency", false);
}

static obs_properties_t *vkcapture_source_get_properties(void *data)
{
    vkcapture_source_t *ctx = data;

    obs_properties_t *props = obs_properties_create();

    obs_property_t *p = obs_properties_add_list(props, "window",
            obs_module_text("CaptureWindow"),
            OBS_COMBO_TYPE_LIST,
            OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(p, obs_module_text("CaptureAnyWindow"), "");

    bool window_found = false;
    pthread_mutex_lock(&server.mutex);
    for (size_t i = 0; i < server.clients.num; i++) {
        vkcapture_client_t *client = server.clients.array + i;
        obs_property_list_add_string(p, client->cdata.exe, client->cdata.exe);
        if (ctx->window && !strcmp(client->cdata.exe, ctx->window)) {
            window_found = true;
        }
    }
    pthread_mutex_unlock(&server.mutex);
    if (ctx->window && !window_found) {
        obs_property_list_add_string(p, ctx->window, ctx->window);
    }

    if (cursor_enabled(ctx)) {
        obs_properties_add_bool(props, "show_cursor", obs_module_text("CaptureCursor"));
    }

    obs_properties_add_bool(props, "allow_transparency", obs_module_text("AllowTransparency"));

    return props;
}

static struct obs_source_info vkcapture_input = {
    .id = "vkcapture-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .get_name = vkcapture_source_get_name,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .create = vkcapture_source_create,
    .destroy = vkcapture_source_destroy,
    .update = vkcapture_source_update,
    .show = vkcapture_source_show,
    .hide = vkcapture_source_hide,
    .video_tick = vkcapture_source_video_tick,
    .video_render = vkcapture_source_render,
    .get_width = vkcapture_source_get_width,
    .get_height = vkcapture_source_get_height,
    .get_defaults = vkcapture_source_get_defaults,
    .get_properties = vkcapture_source_get_properties,
    .icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};

static void server_add_fd(int fd, int events)
{
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    da_push_back(server.fds, &p);
}

static void server_remove_fd(int fd)
{
    for (size_t i = 0; i < server.fds.num; ++i) {
        struct pollfd *p = server.fds.array + i;
        if (p->fd == fd) {
            da_erase(server.fds, i);
            break;
        }
    }
}

static bool server_has_event_on_fd(int fd)
{
    for (size_t i = 0; i < server.fds.num; ++i) {
        struct pollfd *p = server.fds.array + i;
        if (p->fd == fd && p->revents) {
            return true;
        }
    }
    return false;
}

static void server_cleanup_client(vkcapture_client_t *client)
{
    pthread_mutex_lock(&server.mutex);

    close(client->sockfd);
    server_remove_fd(client->sockfd);

    for (int i = 0; i < 4; ++i) {
        if (client->buf_fds[i] >= 0) {
            close(client->buf_fds[i]);
            client->buf_fds[i] = -1;
        }
    }

    da_erase_item(server.clients, client);

    pthread_mutex_unlock(&server.mutex);
}

static void *server_thread_run(void *data)
{
    const char sockname[] = "/com/obsproject/vkcapture";

    int bufid = 0;
    int clientid = 0;

    da_init(server.fds);
    da_init(server.clients);

    struct sockaddr_un addr;
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = '\0'; // Abstract socket
    memcpy(&addr.sun_path[1], sockname, sizeof(sockname) - 1);

    int sockfd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int ret = bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr.sun_family) + sizeof(sockname));
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot bind unix socket to %s: %d", addr.sun_path, errno);
        return NULL;
    }

    ret = listen(sockfd, 1);
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot listen on unix socket bound to %s: %d", addr.sun_path, errno);
        return NULL;
    }

    server_add_fd(sockfd, POLLIN);
    server_add_fd(server.quitfd, POLLIN);

    while (true) {
        int ret = poll(server.fds.array, server.fds.num, -1);
        if (ret <= 0) {
            continue;
        }

        if (server_has_event_on_fd(server.quitfd)) {
            break;
        }

        if (server_has_event_on_fd(sockfd)) {
            int clientfd = accept4(sockfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (clientfd >= 0) {
                vkcapture_client_t client;
                memset(&client, 0, sizeof(client));
                memset(&client.buf_fds, -1, sizeof(client.buf_fds));
                client.id = ++clientid;
                client.sockfd = clientfd;
                pthread_mutex_lock(&server.mutex);
                da_push_back(server.clients, &client);
                pthread_mutex_unlock(&server.mutex);
                server_add_fd(client.sockfd, POLLIN);
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) {
                    blog(LOG_ERROR, "Cannot accept unix socket: %s", strerror(errno));
                }
            }
        }

        for (size_t i = 0; i < server.clients.num; i++) {
            vkcapture_client_t *client = server.clients.array + i;
            if (!server_has_event_on_fd(client->sockfd)) {
                continue;
            }

            uint8_t buf[CAPTURE_TEXTURE_DATA_SIZE];
            struct msghdr msg = {0};
            struct iovec io = {
                .iov_base = buf,
                .iov_len = CAPTURE_TEXTURE_DATA_SIZE,
            };
            msg.msg_iov = &io;
            msg.msg_iovlen = 1;

            char cmsg_buf[CMSG_SPACE(sizeof(int)) * 4];
            msg.msg_control = cmsg_buf;
            msg.msg_controllen = sizeof(cmsg_buf);

            while (true) {
                const ssize_t n = recvmsg(client->sockfd, &msg, MSG_NOSIGNAL);
                if (n == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno != ECONNRESET) {
                        blog(LOG_ERROR, "Socket recv error: %s", strerror(errno));
                    }
                }
                if (n <= 0) {
                    server_cleanup_client(client);
                    break;
                }

                if (buf[0] == CAPTURE_CLIENT_DATA_TYPE) {
                    if (io.iov_len != CAPTURE_CLIENT_DATA_SIZE) {
                        server_cleanup_client(client);
                        break;
                    }
                    pthread_mutex_lock(&server.mutex);
                    memcpy(&client->cdata, buf, CAPTURE_CLIENT_DATA_SIZE);
                    pthread_mutex_unlock(&server.mutex);
                    break;
                } else if (buf[0] == CAPTURE_TEXTURE_DATA_TYPE) {
                    pthread_mutex_lock(&server.mutex);
                    memcpy(&client->tdata, buf, CAPTURE_TEXTURE_DATA_SIZE);
                    pthread_mutex_unlock(&server.mutex);

                    struct cmsghdr *cmsgh = CMSG_FIRSTHDR(&msg);
                    if (!cmsgh || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
                        server_cleanup_client(client);
                        break;
                    }

                    const size_t nfd = (cmsgh->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);

                    int buf_fds[4];
                    memset(buf_fds, -1, sizeof(buf_fds));
                    for (size_t i = 0; i < nfd; ++i) {
                        buf_fds[i] = ((int*)CMSG_DATA(cmsgh))[i];
                    }

                    if (io.iov_len != CAPTURE_TEXTURE_DATA_SIZE || client->tdata.nfd != nfd) {
                        for (size_t i = 0; i < nfd; ++i) {
                            close(buf_fds[i]);
                        }
                        server_cleanup_client(client);
                        break;
                    }

                    pthread_mutex_lock(&server.mutex);
                    for (int i = 0; i < 4; ++i) {
                        if (client->buf_fds[i] >= 0) {
                            close(client->buf_fds[i]);
                        }
                        client->buf_fds[i] = buf_fds[i];
                    }
                    client->buf_id = ++bufid;
                    pthread_mutex_unlock(&server.mutex);
                }
            }
        }
    }

    while (server.clients.num) {
        server_cleanup_client(server.clients.array);
    }

    close(sockfd);

    da_free(server.clients);
    da_free(server.fds);

    return NULL;
}

bool obs_module_load(void)
{
    if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL && obs_get_nix_platform() != OBS_NIX_PLATFORM_WAYLAND) {
        blog(LOG_ERROR, "linux-vkcapture cannot run on non-EGL platforms");
        return false;
    }

    server.quitfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (server.quitfd < 0) {
        blog(LOG_ERROR, "Failed to create eventfd: %s", strerror(errno));
        return false;
    }

    pthread_mutex_init(&server.mutex, NULL);
    if (pthread_create(&server.thread, NULL, server_thread_run, NULL) != 0) {
        blog(LOG_ERROR, "Failed to create thread");
        return false;
    }

    obs_register_source(&vkcapture_input);
    blog(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

    return true;
}

void obs_module_unload()
{
    uint64_t q = 1;
    write(server.quitfd, &q, sizeof(q));

    pthread_join(server.thread, NULL);

    blog(LOG_INFO, "plugin unloaded");
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
