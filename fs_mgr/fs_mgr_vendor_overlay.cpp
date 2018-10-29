/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <dirent.h>
#include <selinux/selinux.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <fs_mgr_vendor_overlay.h>
#include <fstab/fstab.h>

#include "fs_mgr_priv.h"

using namespace std::literals;

namespace {

const auto kVendorOverlaySourceDirFormat = "/system/vendor_overlay/%s";
const auto kVndkVersionPropertyName = "ro.vndk.version";
const auto kVendorTopDir = "/vendor/"s;
const auto kLowerdirOption = "lowerdir="s;

// return true if system supports overlayfs
bool fs_mgr_wants_overlayfs() {
    // Overlayfs available in the kernel, and patched for override_creds?
    auto save_errno = errno;
    auto ret = access("/sys/module/overlay/parameters/override_creds", F_OK) == 0;
    errno = save_errno;
    return ret;
}

std::string fs_mgr_get_vendor_overlay_top_dir() {
    std::string vndk_version = android::base::GetProperty(kVndkVersionPropertyName, "");
    if (vndk_version.empty()) {
        return "";
    }
    return android::base::StringPrintf(kVendorOverlaySourceDirFormat, vndk_version.c_str());
}

std::vector<std::string> fs_mgr_get_vendor_overlay_dirs(const std::string& overlay_top) {
    std::vector<std::string> vendor_overlay_dirs;
    LINFO << "vendor overlay root: " << overlay_top;
    DIR* vendor_overlay_top = opendir(overlay_top.c_str());

    struct dirent* dp;
    while ((dp = readdir(vendor_overlay_top)) != NULL) {
        if (dp->d_type != DT_DIR || strncmp(dp->d_name, ".", 1) == 0) {
            continue;
        }
        vendor_overlay_dirs.push_back(dp->d_name);
    }

    return vendor_overlay_dirs;
}

std::string fs_mgr_get_context(const std::string& mount_point) {
    char* ctx = nullptr;
    auto len = getfilecon(mount_point.c_str(), &ctx);
    if ((len > 0) && ctx) {
        std::string context(ctx, len);
        free(ctx);
        return context;
    }
    return "";
}

bool fs_mgr_vendor_overlay_mount(const std::string& overlay_top, const std::string& mount_point) {
    if (mount_point.empty()) return false;

    const auto vendor_mount_point = kVendorTopDir + mount_point;
    LINFO << "vendor overlay mount on " << vendor_mount_point;

    auto context = fs_mgr_get_context(vendor_mount_point);
    if (!context.empty()) {
        context = ",rootcontext="s + context;
    } else {
        PERROR << " result: cannot find the mount point";
        return false;
    }

    auto options = "override_creds=off,"s + kLowerdirOption + overlay_top + "/" + mount_point +
                   ":" + vendor_mount_point + context;
    auto report = "__mount(source=overlay,target="s + vendor_mount_point + ",type=overlay," +
                  options + ")";
    auto ret = mount("overlay", vendor_mount_point.c_str(), "overlay", MS_RDONLY | MS_RELATIME,
                     options.c_str());
    if (ret) {
        PERROR << report;
        PERROR << " result: " << ret;
        return false;
    } else {
        LINFO << report;
        LINFO << " result: " << ret;
        return true;
    }
}

}  // namespace

// Public functions
// ----------------
bool fs_mgr_vendor_overlay_mount_all() {
    if (!fs_mgr_wants_overlayfs()) {
        // kernel does not support overlayfs
        return false;
    }

    // Mount each directory in /system/vendor_overlay/<ver> on /vendor
    auto ret = true;

    const auto overlay_top = fs_mgr_get_vendor_overlay_top_dir();
    std::vector<std::string> vendor_overlay_dirs = fs_mgr_get_vendor_overlay_dirs(overlay_top);
    for (auto vendor_overlay_dir : vendor_overlay_dirs) {
        if (!fs_mgr_vendor_overlay_mount(overlay_top, vendor_overlay_dir)) {
            ret = false;
        }
    }
    return ret;
}