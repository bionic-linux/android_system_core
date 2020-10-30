/*
 * Copyright (C) 2020 The Android Open Sourete Project
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

#define LOG_TAG "trusty-fuzz-utils"

#include <trusty/fuzz/utils.h>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/uio.h>
#include <log/log_read.h>
#include <time.h>
#include <iostream>

using namespace std;
using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::base::unique_fd;

#define TIPC_IOC_MAGIC 'r'
#define TIPC_IOC_CONNECT _IOW(TIPC_IOC_MAGIC, 0x80, char*)

namespace {
const size_t kTimeoutSeconds = 5;
const string kTrustyLogTag = "trusty-log";

class InitialTime {
  public:
    InitialTime() { seconds_ = time(nullptr); }

    uint32_t seconds() { return seconds_; }

  private:
    time_t seconds_;
};

InitialTime initialTime;

void print_trusty_log() {
    auto logger_list = android_logger_list_open(LOG_ID_KERNEL, ANDROID_LOG_NONBLOCK, 1000, 0);
    if (logger_list == nullptr) {
        cerr << "Could not open android kernel log\n";
        return;
    }

    while (true) {
        log_msg log_msg;
        int rc = android_logger_list_read(logger_list, &log_msg);
        if (rc < 0) {
            break;
        }
        if (log_msg.entry.sec < initialTime.seconds()) {
            continue;
        }
        char* msg = log_msg.msg();
        if (msg) {
            string line(msg, log_msg.entry.len);
            if (line.find(kTrustyLogTag) != string::npos) {
                cerr << line.substr(kTrustyLogTag.length() + 2) << endl;
            }
        }
    }

    android_logger_list_free(logger_list);
}
}  // namespace

namespace android {
namespace trusty {
namespace fuzz {

TrustyApp::TrustyApp(std::string tipc_dev, std::string ta_port)
    : tipc_dev_(tipc_dev), ta_port_(ta_port), ta_fd_(-1) {}

Result<void> TrustyApp::Connect() {
    /*
     * TODO: We can't use libtrusty because (yet)
     * (1) cc_fuzz can't deal with vendor components (b/170753563)
     * (2) We need non-blocking behavior to detect Trusty going down.
     * (we could implement the timeout in the fuzzing code though, as
     * it needs to be around the call to read())
     */
    alarm(kTimeoutSeconds);
    int fd = open(tipc_dev_.c_str(), O_RDWR);
    alarm(0);
    if (fd < 0) {
        return ErrnoError() << "failed to open TIPC device: ";
    }
    ta_fd_.reset(fd);

    // This ioctl will time out in the kernel if it can't connect.
    int rc = TEMP_FAILURE_RETRY(ioctl(ta_fd_, TIPC_IOC_CONNECT, ta_port_.c_str()));
    if (rc < 0) {
        return ErrnoError() << "failed to connect to TIPC service: ";
    }

    return {};
}

Result<void> TrustyApp::Read(void* buf, size_t len) {
    if (ta_fd_ == -1) {
        return Error() << "TA is not connected to yet: ";
    }

    alarm(kTimeoutSeconds);
    int rc = read(ta_fd_, buf, len);
    alarm(0);
    if (rc < 0) {
        return Error() << "failed to read TIPC message from TA: ";
    }

    return {};
}

Result<void> TrustyApp::Write(const void* buf, size_t len) {
    if (ta_fd_ == -1) {
        return Error() << "TA is not connected to yet: ";
    }

    alarm(kTimeoutSeconds);
    int rc = write(ta_fd_, buf, len);
    alarm(0);
    if (rc < 0) {
        return Error() << "failed to read TIPC message from TA: ";
    }

    return {};
}

Result<int> TrustyApp::GetRawFd() {
    if (ta_fd_ == -1) {
        return Error() << "TA is not connected to yet: ";
    }

    return ta_fd_;
}

void Abort() {
    print_trusty_log();
    exit(-1);
}

}  // namespace fuzz
}  // namespace trusty
}  // namespace android
