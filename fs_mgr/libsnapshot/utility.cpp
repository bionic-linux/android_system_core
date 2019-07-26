// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "utility.h"

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace android {
namespace snapshot {

bool UnmapImageIfExists(android::fiemap::IImageManager* manager, const std::string& name) {
    if (!manager->IsImageMapped(name)) {
        return true;
    }
    return manager->UnmapImageDevice(name);
}

AutoDevice::AutoDevice(AutoDevice&& other) {
    std::swap(name_, other.name_);
}

void AutoDevice::Release() {
    name_.clear();
}

AutoDevices::~AutoDevices() {
    // Destroy devices in the reverse order because newer devices may have dependencies
    // on older devices.
    for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
        it->reset();
    }
}

void AutoDevices::Release() {
    for (auto&& p : devices_) {
        p->Release();
    }
}

AutoUnmapDevice::~AutoUnmapDevice() {
    if (name_.empty()) return;
    if (!dm_->DeleteDeviceIfExists(name_)) {
        LOG(ERROR) << "Failed to auto unmap device " << name_;
    }
}

AutoUnmapImage::~AutoUnmapImage() {
    if (name_.empty()) return;
    if (!UnmapImageIfExists(images_, name_)) {
        LOG(ERROR) << "Failed to auto unmap cow image " << name_;
    }
}

bool ForEachPartition(fs_mgr::MetadataBuilder* builder, const std::string& suffix,
                      const std::function<LoopDirective(fs_mgr::Partition*)>& func) {
    for (const auto& group : builder->ListGroups()) {
        for (auto* partition : builder->ListPartitionsInGroup(group)) {
            if (!base::EndsWith(partition->name(), suffix)) {
                continue;
            }
            if (func(partition) == LoopDirective::BREAK) return false;
        }
    }
    return true;
}

AutoDeleteCowImage::~AutoDeleteCowImage() {
    if (!name_.empty() && !manager_->DeleteCowImage(lock_, name_)) {
        LOG(ERROR) << "Failed to auto delete COW image " << name_;
    }
}

AutoDeleteSnapshot::~AutoDeleteSnapshot() {
    if (!name_.empty() && !manager_->DeleteSnapshot(lock_, name_)) {
        LOG(ERROR) << "Failed to auto delete snapshot " << name_;
    }
}

}  // namespace snapshot
}  // namespace android
