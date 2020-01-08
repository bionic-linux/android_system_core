/*
 * Copyright (C) 2007-2016 The Android Open Source Project
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

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef __BIONIC__
#include <android/set_abort_message.h>
#endif

#include <shared_mutex>

#include <android-base/macros.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>

#include "android/log.h"
#include "logger.h"
#include "rwlock.h"
#include "uio.h"

#if (FAKE_LOG_DEVICE == 0)
#include "logd_writer.h"
#include "pmsg_writer.h"
#else
#include "fake_log_device.h"
#endif

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <syscall.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#define LOG_BUF_SIZE 1024

#if defined(__ANDROID__)
static int check_log_uid_permissions() {
  uid_t uid = getuid();

  /* Matches clientHasLogCredentials() in logd */
  if ((uid != AID_SYSTEM) && (uid != AID_ROOT) && (uid != AID_LOG)) {
    uid = geteuid();
    if ((uid != AID_SYSTEM) && (uid != AID_ROOT) && (uid != AID_LOG)) {
      gid_t gid = getgid();
      if ((gid != AID_SYSTEM) && (gid != AID_ROOT) && (gid != AID_LOG)) {
        gid = getegid();
        if ((gid != AID_SYSTEM) && (gid != AID_ROOT) && (gid != AID_LOG)) {
          int num_groups;
          gid_t* groups;

          num_groups = getgroups(0, NULL);
          if (num_groups <= 0) {
            return -EPERM;
          }
          groups = static_cast<gid_t*>(calloc(num_groups, sizeof(gid_t)));
          if (!groups) {
            return -ENOMEM;
          }
          num_groups = getgroups(num_groups, groups);
          while (num_groups > 0) {
            if (groups[num_groups - 1] == AID_LOG) {
              break;
            }
            --num_groups;
          }
          free(groups);
          if (num_groups <= 0) {
            return -EPERM;
          }
        }
      }
    }
  }
  return 0;
}
#endif

/*
 * Release any logger resources. A new log write will immediately re-acquire.
 */
void __android_log_close() {
#if (FAKE_LOG_DEVICE == 0)
  LogdClose();
  PmsgClose();
#else
  FakeClose();
#endif
}

#ifdef __ANDROID__
static __android_logger_function logger_function = __android_log_logd_logger;
#else
static __android_logger_function logger_function = __android_log_stderr_logger;
#endif
static void* logger_aux;
static __android_releaser_function logger_releaser;
static RwLock logger_function_lock;

void __android_log_set_logger(__android_logger_function logger, void* aux,
                              __android_releaser_function releaser) {
  auto lock = std::unique_lock{logger_function_lock};
  if (logger_releaser != nullptr) {
    logger_releaser(logger_aux);
  }
  logger_function = logger;
  logger_aux = aux;
  logger_releaser = releaser;
}

void __android_log_default_aborter(const char* abort_message, void*) {
#ifdef __ANDROID__
  android_set_abort_message(abort_message);
#else
  UNUSED(abort_message);
#endif
  abort();
}

static __android_aborter_function aborter_function = __android_log_default_aborter;
static void* aborter_aux;
static __android_releaser_function aborter_releaser;
static RwLock aborter_function_lock;

void __android_log_set_aborter(__android_aborter_function aborter, void* aux,
                               __android_releaser_function releaser) {
  auto lock = std::unique_lock{aborter_function_lock};
  if (aborter_releaser != nullptr) {
    aborter_releaser(aborter_aux);
  }
  aborter_function = aborter;
  aborter_aux = aux;
  aborter_releaser = releaser;
}
void __android_log_call_aborter(const char* abort_message) {
  auto lock = std::shared_lock{aborter_function_lock};
  aborter_function(abort_message, aborter_aux);
}

#ifdef __ANDROID__
static int write_to_log(log_id_t log_id, struct iovec* vec, size_t nr) {
  int ret, save_errno;
  struct timespec ts;

  save_errno = errno;

  if (log_id == LOG_ID_KERNEL) {
    return -EINVAL;
  }

  clock_gettime(android_log_clockid(), &ts);

  if (log_id == LOG_ID_SECURITY) {
    if (vec[0].iov_len < 4) {
      errno = save_errno;
      return -EINVAL;
    }

    ret = check_log_uid_permissions();
    if (ret < 0) {
      errno = save_errno;
      return ret;
    }
    if (!__android_log_security()) {
      /* If only we could reset downstream logd counter */
      errno = save_errno;
      return -EPERM;
    }
  } else if (log_id == LOG_ID_EVENTS || log_id == LOG_ID_STATS) {
    if (vec[0].iov_len < 4) {
      errno = save_errno;
      return -EINVAL;
    }
  } else {
    int prio = *static_cast<int*>(vec[0].iov_base);
    const char* tag = static_cast<const char*>(vec[1].iov_base);
    size_t len = vec[1].iov_len;

    if (!__android_log_is_loggable_len(prio, tag, len - 1, ANDROID_LOG_VERBOSE)) {
      errno = save_errno;
      return -EPERM;
    }
  }

  ret = LogdWrite(log_id, &ts, vec, nr);
  PmsgWrite(log_id, &ts, vec, nr);

  errno = save_errno;
  return ret;
}
#else
static int write_to_log(log_id_t, struct iovec*, size_t) {
  // Non-Android text logs should go to __android_log_stderr_logger, not here.
  // Non-Android binary logs are always dropped.
  return 0;
}
#endif

// Copied from base/threads.cpp
static uint64_t GetThreadId() {
#if defined(__BIONIC__)
  return gettid();
#elif defined(__APPLE__)
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return tid;
#elif defined(__linux__)
  return syscall(__NR_gettid);
#elif defined(_WIN32)
  return GetCurrentThreadId();
#endif
}

void __android_log_stderr_logger(const struct __android_logger_data* logger_data,
                                 const char* message, void*) {
  struct tm now;
  time_t t = time(nullptr);

#if defined(_WIN32)
  localtime_s(&now, &t);
#else
  localtime_r(&t, &now);
#endif

  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%m-%d %H:%M:%S", &now);

  static const char log_characters[] = "XXVDIWEF";
  static_assert(arraysize(log_characters) - 1 == ANDROID_LOG_SILENT,
                "Mismatch in size of log_characters and values in android_LogPriority");
  int priority =
      logger_data->priority > ANDROID_LOG_SILENT ? ANDROID_LOG_FATAL : logger_data->priority;
  char priority_char = log_characters[priority];
  uint64_t tid = GetThreadId();

  if (logger_data->file != nullptr) {
    fprintf(stderr, "%s %c %s %5d %5" PRIu64 " %s:%u] %s\n",
            logger_data->tag ? logger_data->tag : "nullptr", priority_char, timestamp, getpid(),
            tid, logger_data->file, logger_data->line, message);
  } else {
    fprintf(stderr, "%s %c %s %5d %5" PRIu64 " %s\n",
            logger_data->tag ? logger_data->tag : "nullptr", priority_char, timestamp, getpid(),
            tid, message);
  }
}

void __android_log_logd_logger(const struct __android_logger_data* logger_data, const char* message,
                               void*) {
  int buffer_id = logger_data->buffer_id == LOG_ID_DEFAULT ? LOG_ID_MAIN : logger_data->buffer_id;

  struct iovec vec[3];
  vec[0].iov_base = (unsigned char*)&logger_data->priority;
  vec[0].iov_len = 1;
  vec[1].iov_base = (void*)logger_data->tag;
  vec[1].iov_len = strlen(logger_data->tag) + 1;
  vec[2].iov_base = (void*)message;
  vec[2].iov_len = strlen(message) + 1;

  write_to_log(static_cast<log_id_t>(buffer_id), vec, 3);
}

int __android_log_write(int prio, const char* tag, const char* msg) {
  return __android_log_buf_write(LOG_ID_MAIN, prio, tag, msg);
}

void __android_log_write_logger_data(__android_logger_data logger_data, const char* msg) {
  if (logger_data.tag == nullptr) logger_data.tag = "";

#if __BIONIC__
  if (logger_data.priority == ANDROID_LOG_FATAL) {
    android_set_abort_message(msg);
  }
#endif

  auto lock = std::shared_lock{logger_function_lock};
  logger_function(&logger_data, msg, logger_aux);
}

int __android_log_buf_write(int bufID, int prio, const char* tag, const char* msg) {
  __android_log_write_logger_data({sizeof(__android_logger_data), bufID,
                                   static_cast<android_LogPriority>(prio), tag, nullptr, 0},
                                  msg);
  return 1;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  char buf[LOG_BUF_SIZE];

  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

  return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  char buf[LOG_BUF_SIZE];

  va_start(ap, fmt);
  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
  va_end(ap);

  return __android_log_write(prio, tag, buf);
}

int __android_log_buf_print(int bufID, int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  char buf[LOG_BUF_SIZE];

  va_start(ap, fmt);
  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
  va_end(ap);

  return __android_log_buf_write(bufID, prio, tag, buf);
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  char buf[LOG_BUF_SIZE];

  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);
  } else {
    /* Msg not provided, log condition.  N.B. Do not use cond directly as
     * format string as it could contain spurious '%' syntax (e.g.
     * "%d" in "blocks%devs == 0").
     */
    if (cond)
      snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
    else
      strcpy(buf, "Unspecified assertion failed");
  }

  // Log assertion failures to stderr for the benefit of "adb shell" users
  // and gtests (http://b/23675822).
  TEMP_FAILURE_RETRY(write(2, buf, strlen(buf)));
  TEMP_FAILURE_RETRY(write(2, "\n", 1));

  __android_log_write(ANDROID_LOG_FATAL, tag, buf);
  __android_log_call_aborter(buf);
  abort();
}

int __android_log_bwrite(int32_t tag, const void* payload, size_t len) {
  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 2);
}

int __android_log_stats_bwrite(int32_t tag, const void* payload, size_t len) {
  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_STATS, vec, 2);
}

int __android_log_security_bwrite(int32_t tag, const void* payload, size_t len) {
  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_SECURITY, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void* payload, size_t len) {
  struct iovec vec[3];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = (void*)payload;
  vec[2].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 3);
}

/*
 * Like __android_log_bwrite, but used for writing strings to the
 * event log.
 */
int __android_log_bswrite(int32_t tag, const char* payload) {
  struct iovec vec[4];
  char type = EVENT_TYPE_STRING;
  uint32_t len = strlen(payload);

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = &len;
  vec[2].iov_len = sizeof(len);
  vec[3].iov_base = (void*)payload;
  vec[3].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 4);
}

/*
 * Like __android_log_security_bwrite, but used for writing strings to the
 * security log.
 */
int __android_log_security_bswrite(int32_t tag, const char* payload) {
  struct iovec vec[4];
  char type = EVENT_TYPE_STRING;
  uint32_t len = strlen(payload);

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = &len;
  vec[2].iov_len = sizeof(len);
  vec[3].iov_base = (void*)payload;
  vec[3].iov_len = len;

  return write_to_log(LOG_ID_SECURITY, vec, 4);
}
