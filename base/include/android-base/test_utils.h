/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_BASE_TEST_UTILS_H
#define ANDROID_BASE_TEST_UTILS_H

#include <string>

#include <android-base/macros.h>

class TemporaryFile {
 public:
  TemporaryFile();
  ~TemporaryFile();
  TemporaryFile(TemporaryFile const &) = delete;
  void operator=(TemporaryFile const &) = delete;

  int fd;
  char path[1024];

 private:
  void init(const std::string& tmp_dir);
};

class TemporaryDir {
 public:
  TemporaryDir();
  ~TemporaryDir();
  TemporaryDir(TemporaryDir const &) = delete;
  void operator=(TemporaryDir const &) = delete;

  char path[1024];

 private:
  bool init(const std::string& tmp_dir);
};

class CapturedStderr {
 public:
    CapturedStderr();
    ~CapturedStderr();
    CapturedStderr(CapturedStderr const &) = delete;
    void operator=(CapturedStderr const &) = delete;

    int fd() const;
private:
    void init();
    void reset();

    TemporaryFile temp_file_;
    int old_stderr_;

};

#endif  // ANDROID_BASE_TEST_UTILS_H
