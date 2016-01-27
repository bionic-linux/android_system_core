/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "android-base/errors.h"

#include <gtest/gtest.h>

namespace android {
namespace base {

// Error strings aren't consistent enough across systems to test the output,
// just make sure we can compile correctly and nothing crashes even if we send
// it possibly bogus error codes.
TEST(ErrorsTest, TestSystemErrorString) {
  SystemErrorString(-1);
  SystemErrorString(0);
  SystemErrorString(1);
}

}  // namespace base
}  // namespace android
