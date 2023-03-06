//
// Copyright (C) 2020 The Android Open Source Project
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
#pragma once

#include <sstream>
#include <string>

#include "fastboot.h"
#include "fastboot_driver.h"
#include "super_flash_helper.h"
#include "util.h"

class Task {
  public:
    Task() = default;
    virtual void Run() = 0;
    virtual ~Task() = default;
};

class FlashTask : public Task {
  public:
    FlashTask(const std::string& _slot, const std::string& _pname);
    FlashTask(const std::string& _slot, const std::string& _pname, const std::string& _fname);

    void Run() override;
    ~FlashTask() {}

  private:
    const std::string pname_;
    const std::string fname_;
    const std::string slot_;
};

class RebootTask : public Task {
  public:
    RebootTask(FlashingPlan* _fp);
    RebootTask(FlashingPlan* _fp, const std::string& _reboot_target);
    void Run() override;
    ~RebootTask() {}

  private:
    const std::string reboot_target_ = "";
    FlashingPlan* fp_;
};

class FlashSuperLayoutTask : public Task {
  public:
    FlashSuperLayoutTask(const std::string& _super_name, SuperFlashHelper* _helper);
    static std::unique_ptr<FlashSuperLayoutTask> Initialize(FlashingPlan* _fp,
                                                            std::vector<ImageEntry>& os_images);
    using ImageEntry = std::pair<const Image*, std::string>;
    void Run() override;
    ~FlashSuperLayoutTask(){};

  private:
    std::string super_name_;
    SuperFlashHelper* helper_ = nullptr;
};

class UpdateSuperTask : public Task {
  public:
    UpdateSuperTask(FlashingPlan* _fp, std::vector<ImageEntry>& _os_images);
    void Run() override;

  private:
    FlashingPlan* fp_;
    std::vector<ImageEntry>& os_images_;
};

class ResizeTask : public Task {
  public:
    ResizeTask(FlashingPlan* _fp, const std::string& _pname, const std::string& _size,
               const std::string& _slot);
    void Run() override;

  private:
    FlashingPlan* fp_;
    const std::string pname_;
    const std::string size_;
    const std::string slot_;
};
