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

#include "capture.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/un.h>
#include <sys/socket.h>

static struct {
    int connfd;
    bool accepted;
    bool capturing;
    bool no_modifiers;
    bool linear;
    bool map_host;
    bool need_reinit;
    uint8_t device_uuid[16];
} data;

static bool get_wine_exe(char *buf, size_t bufsize)
{
    FILE *f = fopen("/proc/self/comm", "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, sizeof(char), bufsize, f);
    fclose(f);
    if (n < 1) {
        return false;
    }
    buf[n - 1] = '\0';
    return true;
}

static bool get_exe(char *buf, size_t bufsize)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX);
    if (n <= 0) {
        return false;
    }
    exe[n] = '\0';
    strncpy(buf, basename(exe), bufsize);
    buf[bufsize - 1] = '\0';
    if (!strcmp(buf, "wine-preloader") || !strcmp(buf, "wine64-preloader")) {
        return get_wine_exe(buf, bufsize);
    }
    return true;
}

static bool capture_try_connect()
{
    const char sockname[] = "/com/obsproject/vkcapture";

    struct sockaddr_un addr;
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = '\0'; // Abstract socket
    memcpy(&addr.sun_path[1], sockname, sizeof(sockname) - 1);

    int sock = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int ret = connect(sock, (const struct sockaddr *)&addr, sizeof(addr.sun_family) + sizeof(sockname));
    if (ret == -1) {
        close(sock);
        return false;
    }

    data.connfd = sock;

    struct capture_client_data cd;
    cd.type = CAPTURE_CLIENT_DATA_TYPE;
    get_exe(cd.exe, sizeof(cd.exe));

    struct msghdr msg = {0};
    struct iovec io = {
        .iov_base = &cd,
        .iov_len = CAPTURE_CLIENT_DATA_SIZE,
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    const ssize_t sent = sendmsg(data.connfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        hlog("Socket sendmsg error %s", strerror(errno));
    }

    return true;
}

void capture_init()
{
    memset(&data, 0, sizeof(data));
    data.connfd = -1;
}

void capture_update_socket()
{
    static int64_t last_check = 0;
    const int64_t now = os_time_get_nano();
    if (now - last_check < 1000000000) {
        return;
    }
    last_check = now;

    if (data.connfd < 0 && !capture_try_connect()) {
        return;
    }

    struct capture_control_data control;
    ssize_t n = recv(data.connfd, &control, sizeof(control), 0);
    if (n == sizeof(control)) {
        const bool old_no_modifiers = data.no_modifiers;
        const bool old_linear = data.linear;
        const bool old_map_host = data.map_host;
        data.accepted = control.capturing == 1;
        data.no_modifiers = control.no_modifiers == 1;
        data.linear = control.linear == 1;
        data.map_host = control.map_host == 1;
        memcpy(data.device_uuid, control.device_uuid, 16);
        if (data.capturing && (old_no_modifiers != data.no_modifiers
            || old_linear != data.linear
            || old_map_host != data.map_host)) {
            data.need_reinit = true;
        }
    }
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno != ECONNRESET) {
            hlog("Socket recv error %s", strerror(errno));
        }
    }
    if (n <= 0) {
        close(data.connfd);
        data.connfd = -1;
        data.accepted = false;
    }
}

void capture_init_shtex(
        int width, int height, int format, int strides[4],
        int offsets[4], uint64_t modifier, uint32_t winid,
        bool flip, uint32_t color_space, int nfd, int fds[4])
{
    struct capture_texture_data td = {0};
    td.type = CAPTURE_TEXTURE_DATA_TYPE;
    td.nfd = nfd;
    td.width = width;
    td.height = height;
    td.format = format;
    memcpy(td.strides, strides, sizeof(int) * nfd);
    memcpy(td.offsets, offsets, sizeof(int) * nfd);
    td.modifier = modifier;
    td.winid = winid;
    td.flip = flip;
    td.color_space = color_space;

    struct msghdr msg = {0};

    struct iovec io = {
        .iov_base = &td,
        .iov_len = CAPTURE_TEXTURE_DATA_SIZE,
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * 4)];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfd);

    const ssize_t sent = sendmsg(data.connfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        hlog("Socket sendmsg error %s", strerror(errno));
    }

    data.capturing = true;
    data.need_reinit = false;
}

void capture_stop()
{
    data.capturing = false;
}

bool capture_should_stop()
{
    return data.capturing && (data.connfd < 0 || !data.accepted || data.need_reinit);
}

bool capture_should_init()
{
    return !data.capturing && data.connfd >= 0 && data.accepted;
}

bool capture_ready()
{
    return data.capturing;
}

bool capture_allocate_no_modifiers()
{
    return data.no_modifiers;
}

bool capture_allocate_linear()
{
    return data.linear;
}

bool capture_allocate_map_host()
{
    return data.map_host;
}

bool capture_compare_device_uuid(uint8_t uuid[16])
{
    return memcmp(data.device_uuid, uuid, 16) == 0;
}
