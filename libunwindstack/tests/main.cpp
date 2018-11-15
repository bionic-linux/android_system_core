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

#ifdef LIBDEXFILE
#include <dex/unwindstack_hooks.h>
#endif

#include <gtest/gtest.h>
#include <unwindstack/DexFileHooks.h>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
#ifdef LIBDEXFILE
  unwindstack::SetDexFileHooks(::GetDexFileHooks());
#endif
  return RUN_ALL_TESTS();
}
