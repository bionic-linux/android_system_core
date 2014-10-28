/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "NativeBridgeTest.h"

#include <sys/stat.h>
#include <unistd.h>

namespace android {

// Tests that the bridge is initialized without errors if the code_cache already
// exists.
TEST_F(NativeBridgeTest, CompleteFlow) {
    // Make sure that code_cache does not exists
    struct stat st;
    ASSERT_TRUE((stat(kCodeCache, &st) == -1) && (errno == ENOENT));

    // Create the code_cache
    ASSERT_TRUE(mkdir(kCodeCache, S_IRWXU | S_IRWXG | S_IXOTH) == 0);

    // Init
    ASSERT_TRUE(LoadNativeBridge(kNativeBridgeLibrary, nullptr));
    ASSERT_TRUE(PreInitializeNativeBridge(".", "isa"));
    ASSERT_TRUE(InitializeNativeBridge(nullptr, nullptr));
    ASSERT_TRUE(NativeBridgeAvailable());
    ASSERT_FALSE(NativeBridgeError());

    // Check that the code cache is still there
    ASSERT_TRUE((stat(kCodeCache, &st) == 0) && S_ISDIR(st.st_mode));

    // Clean up
    ASSERT_TRUE(rmdir(kCodeCache) == 0);
    UnloadNativeBridge();

    ASSERT_FALSE(NativeBridgeError());
}

}  // namespace android
