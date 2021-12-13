/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <private/android_filesystem_config.h>
#include <private/canned_fs_config.h>
#include <private/fs_config.h>

#include <android-base/strings.h>

#include <errno.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using android::base::ConsumePrefix;
using android::base::Tokenize;

struct Entry {
    std::string path_pattern;
    unsigned uid;
    unsigned gid;
    unsigned mode;
    uint64_t capabilities;
};

static std::vector<Entry> canned_data;

int load_canned_fs_config(const char* fn) {
    std::ifstream input(fn);
    for (std::string line; std::getline(input, line);) {
        // Historical: the root dir can be represented as a space character.
        // e.g. " 1000 1000 0755" is parsed as
        // path = " ", uid = 1000, gid = 1000, mode = 0755.
        // But at the same time, we also have accepted
        // "/ 1000 1000 0755".
        if (line.front() == ' ') {
            line.insert(line.begin(), '/');
        }

        std::vector<std::string> tokens = Tokenize(line, " ");
        if (tokens.size() < 4) {
            std::cerr << "Ill-formed line: " << line << " in " << fn << std::endl;
            return -1;
        }

        // Historical: remove the leading '/' if exists.
        std::string path = tokens[0].front() == '/' ? std::string(tokens[0], 1) : tokens[0];

        Entry e{
                .path_pattern = path,
                .uid = static_cast<unsigned int>(atoi(tokens[1].c_str())),
                .gid = static_cast<unsigned int>(atoi(tokens[2].c_str())),
                // mode is in octal
                .mode = static_cast<unsigned int>(strtol(tokens[3].c_str(), nullptr, 8)),
                .capabilities = 0,
        };

        for (size_t i = 4; i < tokens.size(); i++) {
            std::string_view sv = tokens[i];
            if (ConsumePrefix(&sv, "capabilities=")) {
                e.capabilities = strtoll(std::string(sv).c_str(), nullptr, 0);
                break;
            }
        }

        canned_data.emplace_back(std::move(e));
    }

    // Note: we used to sort the entries by path names. This was to improve the lookup performance
    // by doing binary search. However, this is no longer the case. The lookup performance is not
    // critical because this tool runs on the host, not on the device. Now, a path can be a glob
    // pattern, and it is assumed that a more specific glob pattern (or an expect path) comes after
    // a less specific glob pattern. e.g. lib/* followed by lib/libfoo.so.
    //
    // To implement the search strategy, reverse the entries order
    std::reverse(canned_data.begin(), canned_data.end());

    std::cout << "loaded " << canned_data.size() << " fs_config entries" << std::endl;
    return 0;
}

void canned_fs_config(const char* path, [[maybe_unused]] int dir,
                      [[maybe_unused]] const char* target_out_path, unsigned* uid, unsigned* gid,
                      unsigned* mode, uint64_t* capabilities) {
    if (path != nullptr && path[0] == '/') path++;  // canned paths lack the leading '/'

    const Entry* found = nullptr;
    // More specific pattern comes first
    for (const auto& entry : canned_data) {
        switch (fnmatch(entry.path_pattern.c_str(), path, 0)) {
            case 0:
                found = &entry;
                break;
            case FNM_NOMATCH:
                break;
            default:
                std::cerr << "Error while matching " << path << " against " << entry.path_pattern
                          << std::endl;
                break;
        }
        if (found != nullptr) break;
    }

    if (found == nullptr) {
        std::cerr << "failed to find " << path << " in canned fs_config" << std::endl;
        exit(1);
    }

    *uid = found->uid;
    *gid = found->gid;
    *mode = found->mode;
    *capabilities = found->capabilities;
}
