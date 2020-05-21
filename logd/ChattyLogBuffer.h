/*
 * Copyright (C) 2012-2014 The Android Open Source Project
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

#pragma once

#include <sys/types.h>

#include <list>
#include <optional>
#include <string>

#include <android-base/thread_annotations.h>
#include <android/log.h>
#include <private/android_filesystem_config.h>
#include <sysutils/SocketClient.h>

#include "LogBuffer.h"
#include "LogBufferElement.h"
#include "LogReaderList.h"
#include "LogReaderThread.h"
#include "LogStatistics.h"
#include "LogTags.h"
#include "LogWhiteBlackList.h"
#include "LogWriter.h"
#include "rwlock.h"

typedef std::list<LogBufferElement*> LogBufferElementCollection;

class ChattyLogBuffer : public LogBuffer {
    LogBufferElementCollection mLogElements GUARDED_BY(lock_);

    // watermark of any worst/chatty uid processing
    typedef std::unordered_map<uid_t, LogBufferElementCollection::iterator> LogBufferIteratorMap;
    LogBufferIteratorMap mLastWorst[LOG_ID_MAX] GUARDED_BY(lock_);
    // watermark of any worst/chatty pid of system processing
    typedef std::unordered_map<pid_t, LogBufferElementCollection::iterator> LogBufferPidIteratorMap;
    LogBufferPidIteratorMap mLastWorstPidOfSystem[LOG_ID_MAX] GUARDED_BY(lock_);

    unsigned long mMaxSize[LOG_ID_MAX] GUARDED_BY(lock_);

    LogBufferElement* lastLoggedElements[LOG_ID_MAX] GUARDED_BY(lock_);
    LogBufferElement* droppedElements[LOG_ID_MAX] GUARDED_BY(lock_);
    void log(LogBufferElement* elem) REQUIRES(lock_);

  public:
    ChattyLogBuffer(LogReaderList* reader_list, LogTags* tags, PruneList* prune,
                    LogStatistics* stats);
    ~ChattyLogBuffer();
    void Init() override;

    int Log(log_id_t log_id, log_time realtime, uid_t uid, pid_t pid, pid_t tid, const char* msg,
            uint16_t len) override;
    uint64_t FlushTo(
            LogWriter* writer, uint64_t start, pid_t* lastTid,
            const std::function<FlushToResult(const LogBufferElement* element)>& filter) override;

    bool Clear(log_id_t id, uid_t uid = AID_ROOT) override;
    unsigned long GetSize(log_id_t id) override;
    int SetSize(log_id_t id, unsigned long size) override;

  private:
    void maybePrune(log_id_t id) REQUIRES(lock_);
    void kickMe(LogReaderThread* me, log_id_t id, unsigned long pruneRows) REQUIRES_SHARED(lock_);

    bool prune(log_id_t id, unsigned long pruneRows, uid_t uid = AID_ROOT) REQUIRES(lock_);
    LogBufferElementCollection::iterator erase(LogBufferElementCollection::iterator it,
                                               bool coalesce = false) REQUIRES(lock_);

    // Returns an iterator to the oldest element for a given log type, or mLogElements.end() if
    // there are no logs for the given log type. Requires mLogElementsLock to be held.
    LogBufferElementCollection::iterator GetOldest(log_id_t log_id) REQUIRES(lock_);

    LogReaderList* reader_list_;
    LogTags* tags_;
    PruneList* prune_;
    LogStatistics* stats_;

    // Keeps track of the iterator to the oldest log message of a given log type, as an
    // optimization when pruning logs.  Use GetOldest() to retrieve.
    std::optional<LogBufferElementCollection::iterator> oldest_[LOG_ID_MAX];

    static RwLock lock_;

    class SCOPED_CAPABILITY SharedLock {
      public:
        SharedLock() ACQUIRE_SHARED(ChattyLogBuffer::lock_) {
            ChattyLogBuffer::lock_.lock_shared();
        }
        ~SharedLock() RELEASE() { ChattyLogBuffer::lock_.unlock(); }

        void lock() ACQUIRE_SHARED(ChattyLogBuffer::lock_) { ChattyLogBuffer::lock_.lock_shared(); }
        void unlock() RELEASE_SHARED(ChattyLogBuffer::lock_) {
            ChattyLogBuffer::lock_.unlock_shared();
        }

        DISALLOW_COPY_AND_ASSIGN(SharedLock);
    };
};
