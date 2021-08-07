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

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/un.h>
#include <sys/socket.h>

#include "utils.h"
#include "capture.h"
#include "plugin-macros.h"

#if HAVE_X11_XCB
#include "xcursor-xcb.h"
#endif

typedef struct {
    int id;
    int sockfd;
    struct capture_client_data cdata;
} vkcapture_client_t;

typedef struct {
    obs_source_t *source;
    gs_texture_t *texture;
#if HAVE_X11_XCB
    xcb_connection_t *xcb;
    xcb_xcursor_t *cursor;
    uint32_t root_winid;
#endif
    bool show_cursor;

    struct capture_texture_data tdata;
    int buf_fds[4];
    int sockfd;

    int clientid;
    int current_client;
    DARRAY(vkcapture_client_t) clients;
} vkcapture_source_t;

static const char *socket_filename = "/tmp/obs-vkcapture.sock";

static void vkcapture_cleanup_client(vkcapture_source_t *ctx, vkcapture_client_t *client)
{
    if (client->sockfd >= 0) {
        close(client->sockfd);
        client->sockfd = -1;
    }

    if (ctx->current_client == client->id) {
        ctx->current_client = 0;
        if (ctx->texture) {
            obs_enter_graphics();
            gs_texture_destroy(ctx->texture);
            obs_leave_graphics();
            ctx->texture = NULL;
        }
        for (int i = 0; i < 4; ++i) {
            if (ctx->buf_fds[i] >= 0) {
                close(ctx->buf_fds[i]);
                ctx->buf_fds[i] = -1;
            }
        }
        memset(&ctx->tdata, 0, CAPTURE_TEXTURE_DATA_SIZE);
#if HAVE_X11_XCB
        ctx->root_winid = 0;
#endif
    }

    da_erase_item(ctx->clients, client);
}

static void vkcapture_source_destroy(void *data)
{
    vkcapture_source_t *ctx = data;

    while (ctx->clients.num) {
        vkcapture_cleanup_client(ctx, ctx->clients.array);
    }

    if (ctx->sockfd >= 0) {
        close(ctx->sockfd);
    }

    unlink(socket_filename);

#if HAVE_X11_XCB
    if (ctx->cursor) {
        obs_enter_graphics();
        xcb_xcursor_destroy(ctx->cursor);
        obs_leave_graphics();
    }
    if (ctx->xcb) {
        xcb_disconnect(ctx->xcb);
    }
#endif

    da_free(ctx->clients);

    bfree(ctx);
}

static void vkcapture_source_update(void *data, obs_data_t *settings)
{
    vkcapture_source_t *ctx = data;

    ctx->show_cursor = obs_data_get_bool(settings, "show_cursor");
}

static void *vkcapture_source_create(obs_data_t *settings, obs_source_t *source)
{
    vkcapture_source_t *ctx = bzalloc(sizeof(vkcapture_source_t));
    ctx->source = source;
    memset(ctx->buf_fds, -1, sizeof(ctx->buf_fds));
    da_init(ctx->clients);

    vkcapture_source_update(ctx, settings);

    unlink(socket_filename);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, socket_filename);
    ctx->sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

    int ret = bind(ctx->sockfd, (const struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot bind unix socket to %s: %d", addr.sun_path, errno);
        vkcapture_source_destroy(ctx);
        return NULL;
    }

    ret = listen(ctx->sockfd, 1);
    if (ret < 0) {
        blog(LOG_ERROR, "Cannot listen on unix socket bound to %s: %d", addr.sun_path, errno);
        vkcapture_source_destroy(ctx);
        return NULL;
    }

#if HAVE_X11_XCB
    if (obs_get_nix_platform() == OBS_NIX_PLATFORM_X11_EGL) {
        ctx->xcb = xcb_connect(NULL, NULL);
        if (!ctx->xcb || xcb_connection_has_error(ctx->xcb)) {
            blog(LOG_ERROR, "Unable to open X display !");
        } else {
            ctx->cursor = xcb_xcursor_init(ctx->xcb);
        }
    }
#endif

    UNUSED_PARAMETER(settings);
    return ctx;
}

static void vkcapture_source_video_tick(void *data, float seconds)
{
    vkcapture_source_t *ctx = data;

#if HAVE_X11_XCB
    if (ctx->texture && ctx->show_cursor && ctx->cursor && obs_source_showing(ctx->source)) {
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
                xcb_xcursor_offset(ctx->cursor, tr_r->dst_x, tr_r->dst_y);
                free(tr_r);
            }
        }
        obs_enter_graphics();
        xcb_xcursor_update(ctx->cursor, cur_r);
        obs_leave_graphics();
        free(cur_r);
    }
#endif

    int clientfd = accept4(ctx->sockfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (clientfd >= 0) {
        vkcapture_client_t client;
        memset(&client, 0, sizeof(client));
        client.id = ++ctx->clientid;
        client.sockfd = clientfd;
        da_push_back(ctx->clients, &client);

        if (!ctx->current_client) {
            char b = '1';
            ssize_t ret = write(client.sockfd, &b, 1);
            ctx->current_client = client.id;
            if (ret != 1) {
                blog(LOG_WARNING, "Socket write error: %s", strerror(errno));
            }
        }
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED) {
            blog(LOG_ERROR, "Cannot accept unix socket: %s", strerror(errno));
        }
    }

    for (size_t i = 0; i < ctx->clients.num; i++) {
        vkcapture_client_t *client = ctx->clients.array + i;

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
                vkcapture_cleanup_client(ctx, client);
                break;
            }

            if (buf[0] == CAPTURE_CLIENT_DATA_TYPE) {
                if (io.iov_len != CAPTURE_CLIENT_DATA_SIZE) {
                    vkcapture_cleanup_client(ctx, client);
                    break;
                }
                memcpy(&client->cdata, buf, CAPTURE_CLIENT_DATA_SIZE);
                break;
            } else if (buf[0] == CAPTURE_TEXTURE_DATA_TYPE) {
                memcpy(&ctx->tdata, buf, CAPTURE_TEXTURE_DATA_SIZE);

                struct cmsghdr *cmsgh = CMSG_FIRSTHDR(&msg);
                if (!cmsgh || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
                    vkcapture_cleanup_client(ctx, client);
                    break;
                }

                const size_t nfd = (cmsgh->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int);

                int buf_fds[4];
                memset(buf_fds, -1, sizeof(buf_fds));
                for (size_t i = 0; i < nfd; ++i) {
                    buf_fds[i] = ((int*)CMSG_DATA(cmsgh))[i];
                }

                if (io.iov_len != CAPTURE_TEXTURE_DATA_SIZE || ctx->tdata.nfd != nfd) {
                    for (size_t i = 0; i < nfd; ++i) {
                        close(buf_fds[i]);
                    }
                    vkcapture_cleanup_client(ctx, client);
                    break;
                }

                if (ctx->current_client != client->id) {
                    for (size_t i = 0; i < nfd; ++i) {
                        close(buf_fds[i]);
                    }
                    break;
                }

                if (ctx->texture) {
                    obs_enter_graphics();
                    gs_texture_destroy(ctx->texture);
                    obs_leave_graphics();
                }

                for (int i = 0; i < 4; ++i) {
                    if (ctx->buf_fds[i] >= 0) {
                        close(ctx->buf_fds[i]);
                        ctx->buf_fds[i] = -1;
                    }
                }

                blog(LOG_INFO, "Creating texture from dmabuf %dx%d modifier:%" PRIu64,
                        ctx->tdata.width, ctx->tdata.height, ctx->tdata.modifier);

                uint32_t strides[4];
                uint32_t offsets[4];
                uint64_t modifiers[4];
                for (uint8_t i = 0; i < ctx->tdata.nfd; ++i) {
                    strides[i] = ctx->tdata.strides[i];
                    offsets[i] = ctx->tdata.offsets[i];
                    modifiers[i] = ctx->tdata.modifier;
                    ctx->buf_fds[i] = buf_fds[i];
                    blog(LOG_INFO, " [%d] fd:%d stride:%d offset:%d", i, buf_fds[i], strides[i], offsets[i]);
                }

                obs_enter_graphics();
                ctx->texture = gs_texture_create_from_dmabuf(ctx->tdata.width, ctx->tdata.height,
                        ctx->tdata.format, GS_BGRX, ctx->tdata.nfd, buf_fds, strides, offsets,
                        ctx->tdata.modifier != DRM_FORMAT_MOD_INVALID ? modifiers : NULL);
                obs_leave_graphics();

                if (!ctx->texture) {
                    blog(LOG_ERROR, "Could not create texture from dmabuf source");
                } else {
                    ctx->current_client = client->id;
                }
            }
        }
    }

    if (!ctx->current_client && ctx->clients.num) {
        vkcapture_client_t *client = ctx->clients.array;
        char b = '1';
        ssize_t ret = write(client->sockfd, &b, 1);
        ctx->current_client = client->id;
        if (ret != 1) {
            blog(LOG_WARNING, "Socket write error: %s", strerror(errno));
        }
    }

    UNUSED_PARAMETER(seconds);
}

static void vkcapture_source_render(void *data, gs_effect_t *effect)
{
    const vkcapture_source_t *ctx = data;

    if (!ctx->texture) {
        return;
    }

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, ctx->texture);

    gs_draw_sprite(ctx->texture, ctx->tdata.flip ? GS_FLIP_V : 0, 0, 0);

#if HAVE_X11_XCB
    if (ctx->show_cursor && ctx->cursor) {
        xcb_xcursor_render(ctx->cursor);
    }
#endif
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
}

static obs_properties_t *vkcapture_source_get_properties(void *data)
{
    const vkcapture_source_t *ctx = data;

    obs_properties_t *props = obs_properties_create();
#if HAVE_X11_XCB
    if (ctx->cursor) {
        obs_properties_add_bool(props, "show_cursor", obs_module_text("CaptureCursor"));
    }
#endif
    return props;
}

struct obs_source_info vkcapture_input = {
    .id = "vkcapture-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .get_name = vkcapture_source_get_name,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .create = vkcapture_source_create,
    .destroy = vkcapture_source_destroy,
    .update = vkcapture_source_update,
    .video_tick = vkcapture_source_video_tick,
    .video_render = vkcapture_source_render,
    .get_width = vkcapture_source_get_width,
    .get_height = vkcapture_source_get_height,
    .get_defaults = vkcapture_source_get_defaults,
    .get_properties = vkcapture_source_get_properties,
    .icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};

bool obs_module_load(void)
{
    if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL && obs_get_nix_platform() != OBS_NIX_PLATFORM_WAYLAND) {
        blog(LOG_ERROR, "linux-vkcapture cannot run on non-EGL platforms");
        return false;
    }

    obs_register_source(&vkcapture_input);
    blog(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload()
{
    blog(LOG_INFO, "plugin unloaded");
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
