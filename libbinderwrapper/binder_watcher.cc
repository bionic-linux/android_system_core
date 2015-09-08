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

#include "binder_watcher.h"

#include <base/logging.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>

namespace android {

BinderWatcher::BinderWatcher() = default;

BinderWatcher::~BinderWatcher() = default;

bool BinderWatcher::Init() {
  int binder_fd = -1;
  ProcessState::self()->setThreadPoolMaxThreadCount(0);
  IPCThreadState::self()->disableBackgroundScheduling(true);
  IPCThreadState::self()->setupPolling(&binder_fd);
  LOG(INFO) << "Got binder FD " << binder_fd;
  if (binder_fd < 0)
    return false;

  if (!base::MessageLoopForIO::current()->WatchFileDescriptor(
          binder_fd, true /* persistent */, base::MessageLoopForIO::WATCH_READ,
          &watcher_, this)) {
    LOG(ERROR) << "Failed to watch binder FD";
    return false;
  }
  return true;
}

void BinderWatcher::OnFileCanReadWithoutBlocking(int fd) {
  IPCThreadState::self()->handlePolledCommands();
}

void BinderWatcher::OnFileCanWriteWithoutBlocking(int fd) {
  NOTREACHED() << "Unexpected writable notification for FD " << fd;
}

}  // namespace android
