/*
 * Copyright (C) 2019 The Android Open Source Project


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
#define __printflike(x, y) __attribute__((__format__(printf, x, y)))

#include <ctype.h>
#include <endian.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/user.h>
#include <syslog.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/macros.h>
#include <android-base/stringprintf.h>
#include <cutils/sockets.h>
#include <log/event_tag_map.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>
#ifdef __ANDROID__
#include <selinux/selinux.h>
#endif

#include "../LogBuffer.h"
#include "../LogKlog.h"
#include "../LogTimes.h"
#include "../LogUtils.h"

#define KMSG_PRIORITY(PRI)                                 \
    '<', '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) / 10, \
            '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) % 10, '>'

namespace android {
struct LogInput {
  public:
    log_id_t log_id;  // char
    log_time realtime;
    uid_t uid;
    pid_t pid;
    pid_t tid;
};

// Because system/core/logd/main.cpp redefines this.
void prdebug(char const* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // We want a random tag length and a random remaining message length
    if (data == nullptr || size < sizeof(LogInput) + 2 * sizeof(uint8_t)) {
        return 0;
    }

    LastLogTimes* times = new LastLogTimes();
    LogBuffer* logBuf = new LogBuffer(times);
    size_t data_left = size;
    const LogInput* logInput = nullptr;

    logBuf->enableStatistics();
    while (data_left >= sizeof(LogInput) + 2 * sizeof(uint8_t)) {
        logInput = reinterpret_cast<const LogInput*>(data);
        data += sizeof(LogInput);
        data_left -= sizeof(LogInput);

        uint8_t tag_length = data[0] % 32;
        uint8_t msg_length = data[1] % 32;
        if (tag_length < 2 || msg_length < 2) {
            // Not enough data for tag and message
            return 0;
        }

        data += 2 * sizeof(uint8_t);
        data_left -= 2 * sizeof(uint8_t);

        if (data_left < tag_length + msg_length) {
            // Not enough data for tag and message
            return 0;
        }

        // We need nullterm'd strings
        char* msg = new char[tag_length + msg_length + 2];
        char* msg_only = msg + tag_length + 1;
        memcpy(msg, data, tag_length);
        msg[tag_length] = '\0';
        memcpy(msg_only, data, msg_length);
        msg_only[msg_length] = '\0';
        data += tag_length + msg_length;
        data_left -= tag_length + msg_length;

        // Other elements not in enum.
        log_id_t log_id = static_cast<log_id_t>(unsigned(logInput->log_id) % (LOG_ID_MAX + 1));
        logBuf->log(log_id, logInput->realtime, logInput->uid, logInput->pid, logInput->tid, msg,
                    tag_length + msg_length + 2);
        delete[] msg;
    }

    log_id_for_each(i) { logBuf->clear(i); }
    delete logBuf;
    delete times;
    return 0;
}
}  // namespace android
