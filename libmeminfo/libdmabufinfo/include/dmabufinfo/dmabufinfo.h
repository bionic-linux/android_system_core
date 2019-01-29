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

#pragma once

#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace android {
namespace dmabufinfo {

struct DmaBuffer {
  public:
    DmaBuffer(ino_t inode, uint64_t size, uint64_t count, const std::string& exporter,
              const std::string& name)
        : inode_(inode), size_(size), count_(count), exporter_(exporter), name_(name) {}
    ~DmaBuffer() = default;

    // Adds one file descriptor reference for the given pid
    void AddFdRef(pid_t pid) {
        AddRefToPidMap(fdrefs_, pid);
    }

    // Adds one map reference for the given pid
    void AddMapRef(pid_t pid) {
        AddRefToPidMap(maprefs_, pid);
    }

    // Getters for each property
    uint64_t size() { return size_; }
    const std::unordered_map<pid_t, int>& fdrefs() const { return fdrefs_; }
    const std::unordered_map<pid_t, int>& maprefs() const { return maprefs_; }
    ino_t inode() const { return inode_; }
    uint64_t total_refs() const { return fdrefs_.size() + maprefs_.size(); }
    uint64_t count() const { return count_; };
    const std::string& name() const { return name_; }
    const std::string& exporter() const { return exporter_; }
    void SetName(const std::string& name) { name_ = name; }
    void SetExporter(const std::string& exporter) { exporter_ = exporter; }
    void SetCount(uint64_t count) { count_ = count; }

  private:
    ino_t inode_;
    uint64_t size_;
    uint64_t count_;
    std::string exporter_;
    std::string name_;
    std::unordered_map<pid_t, int> fdrefs_;
    std::unordered_map<pid_t, int> maprefs_;
    void AddRefToPidMap(std::unordered_map<pid_t, int>& map, pid_t pid) {
        auto [it, inserted] = map.insert(std::make_pair(pid, 1));
        if (!inserted) {
            auto& existing = it->second;
            existing++;
        }
    }
};

// Read and return current dma buf objects from
// DEBUGFS/dma_buf/bufinfo. The references to each dma buffer are not
// populated here and will return an empty vector.
// The vector will be cleared before parsing.
//
// Returns false if something went wrong with the function, true otherwise.
bool ReadDmaBufInfo(std::vector<DmaBuffer>* dmabufs,
                    const std::string& path = "/sys/kernel/debug/dma_buf/bufinfo");

// Read and return dmabuf objects for a given process without the help
// of DEBUGFS
// Any references found will be appended to the supplied vector in order to
// allow incremental parsing (For example, to read multiple PIDs)
//
// Returns false if something went wrong with the function, true otherwise.
bool ReadDmaBufInfo(pid_t pid, std::vector<DmaBuffer>* dmabufs);

}  // namespace dmabufinfo
}  // namespace android
