/*
OBS Linux Vulkan game capture
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

#include <obs-module.h>
#include <obs-nix-platform.h>

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>

#include "utils.h"
#include "plugin-macros.h"

struct msg_texture_data {
    int width;
    int height;
    int format;
    int stride;
    int offset;
};

typedef struct {
    obs_source_t *source;
    gs_texture_t *texture;

    struct msg_texture_data data;
    int buf_fd;
    int sockfd;
    int clientfd;
} vkcapture_source_t;

static const char *socket_filename = "/tmp/obs-vkcapture.sock";

static void vkcapture_cleanup_client(vkcapture_source_t *ctx)
{
    if (ctx->clientfd) {
        close(ctx->clientfd);
        ctx->clientfd = -1;
    }

    if (ctx->texture) {
        gs_texture_destroy(ctx->texture);
        ctx->texture = NULL;
        close(ctx->buf_fd);
        ctx->buf_fd = -1;
    }
}

static void vkcapture_source_destroy(void *data)
{
    vkcapture_source_t *ctx = data;

    vkcapture_cleanup_client(ctx);

    if (ctx->sockfd >= 0) {
        close(ctx->sockfd);
    }

    unlink(socket_filename);

    bfree(data);
}

static void *vkcapture_source_create(obs_data_t *settings, obs_source_t *source)
{
    vkcapture_source_t *ctx = bzalloc(sizeof(vkcapture_source_t));
    ctx->source = source;
    ctx->buf_fd = -1;
    ctx->clientfd = -1;

    unlink(socket_filename);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, socket_filename);
    ctx->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

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

    UNUSED_PARAMETER(settings);
    return ctx;
}

static void vkcapture_source_video_tick(void *data, float seconds)
{
    vkcapture_source_t *ctx = data;

    if (ctx->clientfd < 0) {
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(ctx->sockfd, &set);
        const int maxfd = ctx->sockfd;
        const int nfds = select(maxfd + 1, &set, NULL, NULL, &timeout);
        if (nfds < 0 || !FD_ISSET(ctx->sockfd, &set)) {
            return;
        }

        ctx->clientfd = accept(ctx->sockfd, NULL, NULL);
        if (ctx->clientfd < 0) {
            blog(LOG_ERROR, "Cannot accept unix socket: %d", errno);
            return;
        }

        os_socket_block(ctx->clientfd, false);
    }

    if (ctx->clientfd >= 0) {
        struct msghdr msg = {0};
        struct iovec io = {
            .iov_base = &ctx->data,
            .iov_len = sizeof(struct msg_texture_data),
        };
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        const ssize_t n = recvmsg(ctx->clientfd, &msg, 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno != ECONNRESET) {
                blog(LOG_ERROR, "Socket recv error: %s", strerror(errno));
            }
        }
        if (n <= 0) {
            vkcapture_cleanup_client(ctx);
            return;
        }

        if (io.iov_len != sizeof(struct msg_texture_data)) {
            return;
        }

        struct cmsghdr *cmsgh = CMSG_FIRSTHDR(&msg);
        if (!cmsgh || cmsgh->cmsg_level != SOL_SOCKET || cmsgh->cmsg_type != SCM_RIGHTS) {
            return;
        }

        if (ctx->texture) {
            gs_texture_destroy(ctx->texture);
        }

        if (ctx->buf_fd >= 0) {
            close(ctx->buf_fd);
        }

        ctx->buf_fd = *((int *)CMSG_DATA(cmsgh));

        blog(LOG_INFO, "Creating texture from dmabuf %d %dx%d stride:%d offset:%d", ctx->buf_fd,
                ctx->data.width, ctx->data.height, ctx->data.stride, ctx->data.offset);

        obs_enter_graphics();
        const uint32_t stride = ctx->data.stride;
        const uint32_t offset = ctx->data.offset;
        ctx->texture = gs_texture_create_from_dmabuf(ctx->data.width, ctx->data.height, GS_BGRX,
                1, &ctx->buf_fd, &stride, &offset, NULL);
        obs_leave_graphics();

        if (!ctx->texture) {
            blog(LOG_ERROR, "Could not create texture from dmabuf source");
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

    gs_draw_sprite(ctx->texture, 0, 0, 0);
}

static const char *vkcapture_source_get_name(void *data)
{
    return obs_module_text("GameCapture");
}

static uint32_t vkcapture_source_get_width(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->data.width;
}

static uint32_t vkcapture_source_get_height(void *data)
{
    const vkcapture_source_t *ctx = data;
    return ctx->data.height;
}

struct obs_source_info vkcapture_input = {
    .id = "vkcapture-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .get_name = vkcapture_source_get_name,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .create = vkcapture_source_create,
    .destroy = vkcapture_source_destroy,
    .video_tick = vkcapture_source_video_tick,
    .video_render = vkcapture_source_render,
    .get_width = vkcapture_source_get_width,
    .get_height = vkcapture_source_get_height,
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
