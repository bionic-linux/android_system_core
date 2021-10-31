//
// Copyright (C) 2021 The Android Open Source Project
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
//

#include <cstdio>

#include <fstab/fstab.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::unique_ptr<FILE, decltype(&fclose)> fstab_file(
            fmemopen(static_cast<void*>(const_cast<uint8_t*>(data)), size, "r"), fclose);
    if (fstab_file == nullptr) {
        return 0;
    }
    android::fs_mgr::Fstab fstab;
    android::fs_mgr::ReadFstabFromFp(fstab_file.get(), /* proc_mounts= */ false, &fstab);
    return 0;
}
