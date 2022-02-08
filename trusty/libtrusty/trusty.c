/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "libtrusty"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <log/log.h>

#include <trusty/ipc.h>

#define ATRACE_TAG ATRACE_TAG_HAL
#include <cutils/trace.h>

#define EVENT_BUFFER_LEN 256

int tipc_connect(const char* dev_name, const char* srv_name) {
    char buffer[EVENT_BUFFER_LEN];
    if (ATRACE_ENABLED()) {
        snprintf(buffer, EVENT_BUFFER_LEN, "%s_%s", __func__, srv_name);
    } else {
        snprintf(buffer, EVENT_BUFFER_LEN, "");
    }

    ATRACE_BEGIN(buffer);
    int fd;
    int rc;

    fd = TEMP_FAILURE_RETRY(open(dev_name, O_RDWR));
    if (fd < 0) {
        rc = -errno;
        ALOGE("%s: cannot open tipc device \"%s\": %s\n", __func__, dev_name, strerror(errno));
        fd = rc < 0 ? rc : -1;
        goto err_cleanup;
    }

    rc = TEMP_FAILURE_RETRY(ioctl(fd, TIPC_IOC_CONNECT, srv_name));
    if (rc < 0) {
        rc = -errno;
        ALOGE("%s: can't connect to tipc service \"%s\" (err=%d)\n", __func__, srv_name, errno);
        close(fd);
        fd = rc < 0 ? rc : -1;
        goto err_cleanup;
    }

    ATRACE_INT("tipc_connect_fd", fd);
    ALOGV("%s: connected to \"%s\" fd %d\n", __func__, srv_name, fd);

err_cleanup:
    ATRACE_END();
    return fd;
}

ssize_t tipc_send(int fd, const struct iovec* iov, int iovcnt, struct trusty_shm* shms,
                  int shmcnt) {
    char buffer[EVENT_BUFFER_LEN];
    if (ATRACE_ENABLED()) {
        snprintf(buffer, EVENT_BUFFER_LEN, "%s_%d_%08x", __func__, fd,
                 (iovcnt > 0 && iov[0].iov_len > 0 ? *(uint32_t*)(iov[0].iov_base) : 0));
    } else {
        snprintf(buffer, EVENT_BUFFER_LEN, "");
    }

    ATRACE_BEGIN(buffer);

    struct tipc_send_msg_req req;
    req.iov = (__u64)iov;
    req.iov_cnt = (__u64)iovcnt;
    req.shm = (__u64)shms;
    req.shm_cnt = (__u64)shmcnt;

    int rc = TEMP_FAILURE_RETRY(ioctl(fd, TIPC_IOC_SEND_MSG, &req));
    if (rc < 0) {
        ALOGE("%s: failed to send message (err=%d)\n", __func__, rc);
    }

    ATRACE_END();
    return rc;
}

void tipc_close(int fd) {
    close(fd);
}
