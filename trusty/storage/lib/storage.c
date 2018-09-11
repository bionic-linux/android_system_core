/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "trusty_storage_client"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <log/log.h>
#include <trusty/tipc.h>
#include <trusty/lib/storage.h>

static inline file_handle_t make_file_handle(storage_session_t s, uint32_t fid)
{
    return ((uint64_t)s << 32) | fid;
}

static inline storage_session_t _to_session(file_handle_t fh)
{
    return (storage_session_t)(fh >> 32);
}

static inline uint32_t _to_handle(file_handle_t fh)
{
    return (uint32_t) fh;
}

static inline uint32_t _to_msg_flags(uint32_t opflags)
{
    uint32_t msg_flags = 0;

    if (opflags & STORAGE_OP_COMPLETE)
        msg_flags |= STORAGE_MSG_FLAG_TRANSACT_COMPLETE;

    return msg_flags;
}

static ssize_t check_response(struct storage_msg *msg, ssize_t res)
{
    if (res < 0)
        return res;

    if ((size_t)res < sizeof(*msg)) {
        ALOGE("invalid msg length (%zd < %zd)\n", res, sizeof(*msg));
        return -EIO;
    }

    ALOGV("cmd 0x%x: server returned %u\n", msg->cmd, msg->result);

    switch(msg->result) {
        case STORAGE_NO_ERROR:
            return res - sizeof(*msg);

        case STORAGE_ERR_NOT_FOUND:
            return -ENOENT;

        case STORAGE_ERR_EXIST:
            return -EEXIST;

        case STORAGE_ERR_NOT_VALID:
            return -EINVAL;

        case STORAGE_ERR_UNIMPLEMENTED:
            ALOGE("cmd 0x%x: is unhandles command\n", msg->cmd);
            return -EINVAL;

        case STORAGE_ERR_ACCESS:
             return -EACCES;

        case STORAGE_ERR_TRANSACT:
             return -EBUSY;

        case STORAGE_ERR_GENERIC:
            ALOGE("cmd 0x%x: internal server error\n", msg->cmd);
            return -EIO;

        default:
            ALOGE("cmd 0x%x: unhandled server response %u\n",
                   msg->cmd, msg->result);
    }

    return -EIO;
}

static ssize_t send_reqv(storage_session_t session,
                         const struct iovec *tx_iovs, uint tx_iovcnt,
                         const struct iovec *rx_iovs, uint rx_iovcnt)
{
    ssize_t rc;

    rc = writev(session, tx_iovs, tx_iovcnt);
    if (rc < 0) {
        rc = -errno;
        ALOGE("failed to send request: %s\n", strerror(errno));
        return rc;
    }

    rc = readv(session, rx_iovs, rx_iovcnt);
    if (rc < 0) {
        rc = -errno;
        ALOGE("failed to recv response: %s\n", strerror(errno));
        return rc;
    }

    return rc;
}

int storage_open_session(const char *device, storage_session_t *session_p,
                         const char *port)
{
    int rc = tipc_connect(device, port);
    if (rc < 0)
        return rc;
    *session_p = (storage_session_t) rc;
    return 0;
}

void storage_close_session(storage_session_t session)
{
    tipc_close(session);
}


int storage_open_file(storage_session_t session, file_handle_t *handle_p, const char *name,
                      uint32_t flags, uint32_t opflags)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_OPEN, .flags = _to_msg_flags(opflags)};
    struct storage_file_open_req req = { .flags = flags };
    struct iovec tx[3] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}, {(void *)name, strlen(name)}};
    struct storage_file_open_resp rsp = { 0 };
    struct iovec rx[2] = {{&msg, sizeof(msg)}, {&rsp, sizeof(rsp)}};

    ssize_t rc = send_reqv(session, tx, 3, rx, 2);
    rc = check_response(&msg, rc);
    if (rc < 0)
        return rc;

    if ((size_t)rc != sizeof(rsp)) {
        ALOGE("%s: invalid response length (%zd != %zd)\n", __func__, rc, sizeof(rsp));
        return -EIO;
    }

    *handle_p = make_file_handle(session, rsp.handle);
    return 0;
}

void storage_close_file(file_handle_t fh)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_CLOSE };
    struct storage_file_close_req req = { .handle = _to_handle(fh)};
    struct iovec tx[2] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}};
    struct iovec rx[1] = {{&msg, sizeof(msg)}};

    ssize_t rc = send_reqv(_to_session(fh), tx, 2, rx, 1);
    rc = check_response(&msg, rc);
    if (rc < 0) {
        ALOGE("close file failed (%d)\n", (int)rc);
    }
}

int storage_move_file(storage_session_t session, file_handle_t handle,
                      const char *old_name, const char *new_name,
                      uint32_t flags, uint32_t opflags)
{
    size_t old_name_len = strlen(old_name);
    size_t new_name_len = strlen(new_name);
    struct storage_msg msg = {
        .cmd = STORAGE_FILE_MOVE,
        .flags = _to_msg_flags(opflags),
    };
    struct storage_file_move_req req = {
        .flags = flags,
        .handle = handle,
        .old_name_len = old_name_len,
    };
    struct iovec tx[4] = {
        {&msg, sizeof(msg)},
        {&req, sizeof(req)},
        {(void *)old_name, old_name_len},
        {(void *)new_name, new_name_len},
    };
    struct iovec rx[1] = {{&msg, sizeof(msg)}};

    ssize_t rc = send_reqv(session, tx, 4, rx, 1);
    return (int)check_response(&msg, rc);
}

int storage_open_dir(const char *path, struct storage_open_dir_state **state)
{
    struct storage_file_list_resp *resp;

    if (path && strlen(path)) {
        return -ENOENT; /* current server does not support directories */
    }
    *state = malloc(sizeof(**state));
    if (*state == NULL) {
        return -ENOMEM;
    }
    resp = (void *)(*state)->buf;
    resp->flags = STORAGE_FILE_LIST_START;
    (*state)->buf_size = sizeof(*resp);
    (*state)->buf_last_read = 0;
    (*state)->buf_read = (*state)->buf_size;

    return 0;
}

void storage_close_dir(struct storage_open_dir_state *state)
{
    free(state);
}

static int storage_read_dir_send_message(storage_session_t session,
                                         struct storage_open_dir_state *state)
{
    struct storage_file_list_resp *last_item = (void *)(state->buf + state->buf_last_read);
    struct storage_msg msg = { .cmd = STORAGE_FILE_LIST };
    struct storage_file_list_req req = { .flags = last_item->flags };
    struct iovec tx[3] = {
        {&msg, sizeof(msg)},
        {&req, sizeof(req)},
    };
    uint32_t tx_count = 2;
    struct iovec rx[2] = {
        {&msg, sizeof(msg)},
        {state->buf, sizeof(state->buf)}
    };
    ssize_t rc;

    if (last_item->flags != STORAGE_FILE_LIST_START) {
        tx[2].iov_base = last_item->name;
        tx[2].iov_len = strlen(last_item->name);
        tx_count = 3;
    }

    rc = send_reqv(session, tx, tx_count, rx, 2);
    rc = check_response(&msg, rc);

    state->buf_size = (rc > 0) ? rc : 0;
    state->buf_last_read = 0;
    state->buf_read = 0;

    if (rc < 0)
        return rc;

    return 0;
}

int storage_read_dir(storage_session_t session,
                     struct storage_open_dir_state *state,
                     uint8_t *flags,
                     char *name, size_t name_out_size)
{
    int ret;
    size_t rem;
    size_t name_size;
    struct storage_file_list_resp *item;

    if (state->buf_size == 0) {
        return -EIO;
    }

    if (state->buf_read >= state->buf_size) {
        ret = storage_read_dir_send_message(session, state);
        if (ret) {
            return ret;
        }
    }
    rem = state->buf_size - state->buf_read;
    if (rem < sizeof(*item)) {
        return -EIO;
    }
    item = (void *)(state->buf + state->buf_read);
    rem -= sizeof(*item);

    *flags = item->flags;
    if ((item->flags & STORAGE_FILE_LIST_STATE_MASK) == STORAGE_FILE_LIST_END) {
        state->buf_size = 0;
        name_size = 0;
    } else {
        name_size = strnlen(item->name, rem) + 1;
        if (name_size > rem) {
            ALOGE("got invalid filename size %zd >= %zd\n", name_size, rem);
            return -EIO;
        }
        if (name_size >= name_out_size) {
            return -ENOMEM;
        }
        strcpy(name, item->name);
    }

    state->buf_last_read = state->buf_read;
    state->buf_read +=  sizeof(*item) + name_size;

    return 0;
}

int storage_delete_file(storage_session_t session, const char *name, uint32_t opflags)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_DELETE, .flags = _to_msg_flags(opflags)};
    struct storage_file_delete_req req = { .flags = 0, };
    struct iovec tx[3] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}, {(void *)name, strlen(name)}};
    struct iovec rx[1] = {{&msg, sizeof(msg)}};

    ssize_t rc = send_reqv(session, tx, 3, rx, 1);
    return check_response(&msg, rc);
}

static int _read_chunk(file_handle_t fh, storage_off_t off, void *buf, size_t size)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_READ };
    struct storage_file_read_req req = { .handle = _to_handle(fh), .size = size, .offset = off };
    struct iovec tx[2] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}};
    struct iovec rx[2] = {{&msg, sizeof(msg)}, {buf, size}};

    ssize_t rc = send_reqv(_to_session(fh), tx, 2, rx, 2);
    return check_response(&msg, rc);
}

ssize_t storage_read(file_handle_t fh, storage_off_t off, void *buf, size_t size)
{
    int rc;
    size_t bytes_read = 0;
    size_t chunk = MAX_CHUNK_SIZE;
    uint8_t *ptr = buf;

    while (size) {
        if (chunk > size)
            chunk = size;
        rc = _read_chunk(fh, off, ptr, chunk);
        if (rc < 0)
            return rc;
        if (rc == 0)
            break;
        off += rc;
        ptr += rc;
        bytes_read += rc;
        size -= rc;
    }
    return bytes_read;
}

static int _write_req(file_handle_t fh, storage_off_t off,
                      const void *buf, size_t size, uint32_t msg_flags)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_WRITE, .flags = msg_flags, };
    struct storage_file_write_req req = { .handle = _to_handle(fh), .offset = off, };
    struct iovec tx[3] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}, {(void *)buf, size}};
    struct iovec rx[1] = {{&msg, sizeof(msg)}};

    ssize_t rc = send_reqv(_to_session(fh), tx, 3, rx, 1);
    rc = check_response(&msg, rc);
    return rc < 0 ? rc : size;
}

ssize_t storage_write(file_handle_t fh, storage_off_t off,
                      const void *buf, size_t size, uint32_t opflags)
{
    int rc;
    size_t bytes_written = 0;
    size_t chunk = MAX_CHUNK_SIZE;
    const uint8_t *ptr = buf;
    uint32_t msg_flags = _to_msg_flags(opflags & ~STORAGE_OP_COMPLETE);

    while (size) {
        if (chunk >= size) {
            /* last chunk in sequence */
            chunk = size;
            msg_flags = _to_msg_flags(opflags);
        }
        rc = _write_req(fh, off, ptr, chunk, msg_flags);
        if (rc < 0)
            return rc;
        if ((size_t)rc != chunk) {
            ALOGE("got partial write (%d)\n", (int)rc);
            return -EIO;
        }
        off += chunk;
        ptr += chunk;
        bytes_written += chunk;
        size -= chunk;
    }
    return bytes_written;
}

int storage_set_file_size(file_handle_t fh, storage_off_t file_size, uint32_t opflags)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_SET_SIZE, .flags = _to_msg_flags(opflags)};
    struct storage_file_set_size_req req = { .handle = _to_handle(fh), .size = file_size, };
    struct iovec tx[2] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}};
    struct iovec rx[1] = {{&msg, sizeof(msg)}};

    ssize_t rc = send_reqv(_to_session(fh), tx, 2, rx, 1);
    return check_response(&msg, rc);
}

int storage_get_file_size(file_handle_t fh, storage_off_t *size_p)
{
    struct storage_msg msg = { .cmd = STORAGE_FILE_GET_SIZE };
    struct storage_file_get_size_req  req = { .handle = _to_handle(fh), };
    struct iovec tx[2] = {{&msg, sizeof(msg)}, {&req, sizeof(req)}};
    struct storage_file_get_size_resp rsp;
    struct iovec rx[2] = {{&msg, sizeof(msg)}, {&rsp, sizeof(rsp)}};

    ssize_t rc = send_reqv(_to_session(fh), tx, 2, rx, 2);
    rc = check_response(&msg, rc);
    if (rc < 0)
        return rc;

    if ((size_t)rc != sizeof(rsp)) {
        ALOGE("%s: invalid response length (%zd != %zd)\n", __func__, rc, sizeof(rsp));
        return -EIO;
    }

    *size_p = rsp.size;
    return 0;
}

int storage_end_transaction(storage_session_t session, bool complete)
{
    struct storage_msg msg = {
        .cmd = STORAGE_END_TRANSACTION,
        .flags = complete ? STORAGE_MSG_FLAG_TRANSACT_COMPLETE : 0,
    };
    struct iovec iov = {&msg, sizeof(msg)};

    ssize_t rc = send_reqv(session, &iov, 1, &iov, 1);
    return check_response(&msg, rc);
}
