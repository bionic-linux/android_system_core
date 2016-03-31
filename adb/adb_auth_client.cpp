/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define TRACE_TAG AUTH

#include "adb_auth.h"
#include "sysdeps.h"

#include <resolv.h>
#include <stdio.h>
#include <string.h>

#include <openssl/obj_mac.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <crypto_utils/android_pubkey.h>

#include "cutils/list.h"
#include "cutils/sockets.h"

#include "adb.h"
#include "fdevent.h"
#include "transport.h"

struct adb_public_key {
    struct listnode node;
    RSA* key;
};

static const char *key_paths[] = {
    "/adb_keys",
    "/data/misc/adb/adb_keys",
    NULL
};

static fdevent listener_fde;
static fdevent framework_fde;
static int framework_fd = -1;

static void usb_disconnected(void* unused, atransport* t);
static struct adisconnect usb_disconnect = { usb_disconnected, nullptr};
static atransport* usb_transport;
static bool needs_retry = false;

static void read_keys(const char *file, struct listnode *list)
{
    FILE *f;
    char buf[MAX_PAYLOAD_V1];
    uint8_t keybuf[ANDROID_PUBKEY_ENCODED_SIZE(2048)];
    char *sep;
    int ret;

    f = fopen(file, "re");
    if (!f) {
        D("Can't open '%s'", file);
        return;
    }

    while (fgets(buf, sizeof(buf), f)) {
        auto key = reinterpret_cast<adb_public_key*>(
            calloc(1, sizeof(adb_public_key)));
        if (key == nullptr) {
            D("Can't malloc key");
            break;
        }

        sep = strpbrk(buf, " \t");
        if (sep)
            *sep = '\0';

        ret = __b64_pton(buf, keybuf, sizeof(keybuf));
        if (ret != sizeof(keybuf)) {
            D("%s: Invalid base64 data ret=%d", file, ret);
            free(key);
            continue;
        }

        if (!android_pubkey_decode(keybuf, sizeof(keybuf), &key->key)) {
            D("%s: Failed to parse key", file);
            free(key);
            continue;
        }

        list_add_tail(list, &key->node);
    }

    fclose(f);
}

static void free_keys(struct listnode *list)
{
    struct listnode *item;

    while (!list_empty(list)) {
        item = list_head(list);
        list_remove(item);
        adb_public_key* key = node_to_item(item, struct adb_public_key, node);
        RSA_free(key->key);
        free(key);
    }
}

static void load_keys(struct listnode *list)
{
    const char* path;
    const char** paths = key_paths;
    struct stat buf;

    list_init(list);

    while ((path = *paths++)) {
        if (!stat(path, &buf)) {
            D("Loading keys from '%s'", path);
            read_keys(path, list);
        }
    }
}

int adb_auth_generate_token(void *token, size_t token_size)
{
    FILE *f;
    int ret;

    f = fopen("/dev/urandom", "re");
    if (!f)
        return 0;

    ret = fread(token, token_size, 1, f);

    fclose(f);
    return ret * token_size;
}

int adb_auth_verify(uint8_t* token, uint8_t* sig, int siglen)
{
    struct listnode *item;
    struct listnode key_list;
    int ret = 0;

    load_keys(&key_list);

    list_for_each(item, &key_list) {
        adb_public_key* key = node_to_item(item, struct adb_public_key, node);
        ret = RSA_verify(NID_sha, token, SHA_DIGEST_LENGTH, sig, siglen,
                         key->key);
        if (ret)
            break;
    }

    free_keys(&key_list);

    return ret;
}

static void usb_disconnected(void* unused, atransport* t) {
    D("USB disconnect");
    usb_transport = NULL;
    needs_retry = false;
}

static void framework_disconnected() {
    D("Framework disconnect");
    fdevent_remove(&framework_fde);
    framework_fd = -1;
}

static void adb_auth_event(int fd, unsigned events, void*) {
    char response[2];
    int ret;

    if (events & FDE_READ) {
        ret = unix_read(fd, response, sizeof(response));
        if (ret <= 0) {
            framework_disconnected();
        } else if (ret == 2 && response[0] == 'O' && response[1] == 'K') {
            if (usb_transport) {
                adb_auth_verified(usb_transport);
            }
        }
    }
}

void adb_auth_confirm_key(unsigned char *key, size_t len, atransport *t)
{
    char msg[MAX_PAYLOAD_V1];
    int ret;

    if (!usb_transport) {
        usb_transport = t;
        t->AddDisconnect(&usb_disconnect);
    }

    if (framework_fd < 0) {
        D("Client not connected");
        needs_retry = true;
        return;
    }

    if (key[len - 1] != '\0') {
        D("Key must be a null-terminated string");
        return;
    }

    ret = snprintf(msg, sizeof(msg), "PK%s", key);
    if (ret >= (signed)sizeof(msg)) {
        D("Key too long. ret=%d", ret);
        return;
    }
    D("Sending '%s'", msg);

    ret = unix_write(framework_fd, msg, ret);
    if (ret < 0) {
        D("Failed to write PK, errno=%d", errno);
        return;
    }
}

static void adb_auth_listener(int fd, unsigned events, void* data) {
    sockaddr_storage addr;
    socklen_t alen;
    int s;

    alen = sizeof(addr);

    s = adb_socket_accept(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    if (s < 0) {
        D("Failed to accept: errno=%d", errno);
        return;
    }

    if (framework_fd >= 0) {
        LOG(WARNING) << "adb received framework auth socket connection again";
        framework_disconnected();
    }

    framework_fd = s;
    fdevent_install(&framework_fde, framework_fd, adb_auth_event, nullptr);
    fdevent_add(&framework_fde, FDE_READ);

    if (needs_retry) {
        needs_retry = false;
        send_auth_request(usb_transport);
    }
}

void adbd_cloexec_auth_socket() {
    int fd = android_get_control_socket("adbd");
    if (fd == -1) {
        D("Failed to get adbd socket");
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void adbd_auth_init(void) {
    int fd = android_get_control_socket("adbd");
    if (fd == -1) {
        D("Failed to get adbd socket");
        return;
    }

    if (listen(fd, 4) == -1) {
        D("Failed to listen on '%d'", fd);
        return;
    }

    fdevent_install(&listener_fde, fd, adb_auth_listener, NULL);
    fdevent_add(&listener_fde, FDE_READ);
}
