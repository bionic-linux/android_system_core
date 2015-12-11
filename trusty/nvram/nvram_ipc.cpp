/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "nvram_ipc.h"

extern "C" {
#include <string.h>

#define LOG_TAG "TrustyNvram"
#include <log/log.h>
#include <trusty/tipc.h>
}  // extern "C"

#include <nvram/proto.h>

namespace nvram {

static const size_t kResponseBufferSize = 4096;
static const char kTrustyDeviceName[] = "/dev/trusty-ipc-dev0";
static const char kTrustyNvramAppId[] = "com.android.trusty.nvram";

static int g_nvram_fd = -1;

bool Connect() {
  int rc = tipc_connect(kTrustyDeviceName, kTrustyNvramAppId);
  if (rc < 0) {
    ALOGE("Failed to connect to Trusty NVRAM app: %s\n", strerror(-rc));
    return false;
  }

  g_nvram_fd = rc;
  return true;
}

bool ExecuteRequest(const nvram::Request& request, nvram::Response* response) {
  nvram::Blob request_buffer;
  if (!request_buffer.Resize(nvram::proto::GetSize(request))) {
    ALOGE("Failed to allocate request buffer.\n");
    return false;
  }

  nvram::OutputStream request_stream(request_buffer.data(),
                                     request_buffer.size());
  if (!nvram::proto::Encode(request, &request_stream)) {
    ALOGE("Failed to encode nvram request.\n");
    return false;
  }

  ssize_t rc = write(g_nvram_fd, request_buffer.data(), request_buffer.size());
  if (rc < 0) {
    ALOGE("Failed to send nvram request: %s\n", strerror(-rc));
    return false;
  }
  if (static_cast<size_t>(rc) != request_buffer.size()) {
    ALOGE("Failed to send full request buffer: %ld\n", rc);
    return false;
  }

  nvram::Blob response_buffer;
  if (!response_buffer.Resize(kResponseBufferSize)) {
    ALOGE("Failed to allocate response buffer\n.");
    return false;
  }

  rc = read(g_nvram_fd, response_buffer.data(), response_buffer.size());
  if (rc < 0) {
    ALOGE("Failed to send nvram request: %s\n", strerror(-rc));
    return false;
  }

  size_t bytes_read = static_cast<size_t>(rc);
  nvram::InputStream response_stream(response_buffer.data(),
                                     bytes_read < response_buffer.size()
                                         ? bytes_read
                                         : response_buffer.size());
  if (!nvram::proto::Decode(response, &response_stream) ||
      !response_stream.done()) {
    ALOGE("Failed to decode nvram response.\n");
    return false;
  }

  return true;
}

bool Disconnect() {
  if (g_nvram_fd != -1) {
    tipc_close(g_nvram_fd);
  }

  return true;
}

}  // namespace nvram
