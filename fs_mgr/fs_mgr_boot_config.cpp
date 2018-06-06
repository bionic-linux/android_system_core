/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <sys/cdefs.h>  // __predict_true() and _predict_false()

#include <string>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/properties.h>

#include "fs_mgr_priv.h"

std::vector<std::string> SplitWithQuotes(const std::string& s, const std::string& delimiters) {
    std::vector<std::string> result;

    if (__predict_false(delimiters.empty())) return result;

    static const std::string quotes = "\"'`";
    if (__predict_false(delimiters.find_first_of(quotes) != delimiters.npos)) return result;

    size_t base = 0;
    while (true) {
        // skip quoted spans
        auto found = base;
        while (((found = s.find_first_of(delimiters + quotes, found)) != s.npos) &&
               (quotes.find(s[found]) != quotes.npos)) {
            if ((found = s.find_first_of(s[found], found + 1)) == s.npos) break;
            ++found;
        }
        auto piece = s.substr(base, found - base);

        // strip quoted fragments from piece
        for (size_t begin = 0, end; ((begin = piece.find_first_of(quotes, begin)) != piece.npos) &&
                                    ((end = piece.find(piece[begin], begin + 1)) != piece.npos);
             begin = end - 1) {
            piece.erase(end, 1);
            piece.erase(begin, 1);
        }

        result.emplace_back(std::move(piece));
        if (found == s.npos) break;
        base = found + 1;
    }

    return result;
}

// Tries to get the given boot config value from kernel cmdline.
// Returns true if successfully found, false otherwise.
bool fs_mgr_get_boot_config_from_kernel_cmdline(const std::string& key, std::string* out_val) {
    FS_MGR_CHECK(out_val != nullptr);

    std::string cmdline;
    std::string cmdline_key("androidboot." + key);
    if (android::base::ReadFileToString("/proc/cmdline", &cmdline)) {
        for (const auto& entry : SplitWithQuotes(android::base::Trim(cmdline), " ")) {
            std::vector<std::string> pieces = SplitWithQuotes(entry, "=");
            if (pieces.size() == 2) {
                if (pieces[0] == cmdline_key) {
                    *out_val = pieces[1];
                    return true;
                }
            }
        }
    }

    return false;
}

// Tries to get the boot config value in properties, kernel cmdline and
// device tree (in that order).  returns 'true' if successfully found, 'false'
// otherwise
bool fs_mgr_get_boot_config(const std::string& key, std::string* out_val) {
    FS_MGR_CHECK(out_val != nullptr);

    // first check if we have "ro.boot" property already
    *out_val = android::base::GetProperty("ro.boot." + key, "");
    if (!out_val->empty()) {
        return true;
    }

    // fallback to kernel cmdline, properties may not be ready yet
    if (fs_mgr_get_boot_config_from_kernel_cmdline(key, out_val)) {
        return true;
    }

    // lastly, check the device tree
    if (is_dt_compatible()) {
        std::string file_name = get_android_dt_dir() + "/" + key;
        if (android::base::ReadFileToString(file_name, out_val)) {
            if (!out_val->empty()) {
                out_val->pop_back();  // Trims the trailing '\0' out.
                return true;
            }
        }
    }

    return false;
}
