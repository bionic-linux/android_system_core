//
// Copyright (C) 2023 The Android Open Source Project
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

#include "task.h"
#include "fastboot.h"

#include <gtest/gtest.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include "android-base/strings.h"
using android::base::Split;

class ParseTest : public ::testing ::Test {
  protected:
    void SetUp() override {
        fp = std::make_unique<FlashingPlan>();
        fp->slot_override = "b";
        fp->secondary_slot = "a";
    }
    FlashingPlan* GetFP() { return fp.get(); }
    void TearDown() override {}

  private:
    std::unique_ptr<FlashingPlan> fp;
};

static std::vector<std::unique_ptr<Task>> collectTasks(FlashingPlan* fp,
                                                       const std::vector<std::string>& commands) {
    std::vector<std::vector<std::string>> vec_commands;
    for (auto& command : commands) {
        vec_commands.emplace_back(android::base::Split(command, " "));
    }
    std::vector<std::unique_ptr<Task>> tasks;
    for (auto& command : vec_commands) {
        tasks.emplace_back(ParseFastbootInfoLine(fp, command));
    }
    return tasks;
}

TEST_F(ParseTest, CORRECT_FlASH_TASK_FORMED) {
    std::vector<std::string> commands = {"flash dtbo", "flash --slot-other system system_other.img",
                                         "flash system", "flash --apply-vbmeta vbmeta"};

    std::vector<std::unique_ptr<Task>> tasks = collectTasks(GetFP(), commands);

    std::vector<std::vector<std::string>> expected_values{
            {"dtbo", "dtbo_b", "b", "dtbo.img"},
            {"system", "system_a", "a", "system_other.img"},
            {"system", "system_b", "b", "system.img"},
            {"vbmeta", "vbmeta_b", "b", "vbmeta.img"}

    };

    for (auto& task : tasks) {
        ASSERT_TRUE(task != nullptr);
    }

    for (size_t i = 0; i < tasks.size(); i++) {
        auto task = tasks[i]->AsFlashTask();
        ASSERT_TRUE(task != nullptr);
        ASSERT_EQ(task->GetPartition(), expected_values[i][0]);
        ASSERT_EQ(task->GetPartitionAndSlot(), expected_values[i][1]);
        ASSERT_EQ(task->GetSlot(), expected_values[i][2]);
        ASSERT_EQ(task->GetImageName(), expected_values[i][3]);
    }
}

TEST_F(ParseTest, VERSION_CHECK_CORRRECT) {
    std::vector<std::string> correct_versions = {
            "version 1.0",
            "version 22.00",
    };

    std::vector<std::string> bad_versions = {"version",        "version .01", "version x1",
                                             "version 1.0.1",  "version 1.",  "s 1.0",
                                             "version 1.0 2.0"};

    for (auto& version : correct_versions) {
        ASSERT_TRUE(CheckFastbootInfoRequirements(android::base::Split(version, " ")));
    }
    for (auto& version : bad_versions) {
        ASSERT_FALSE(CheckFastbootInfoRequirements(android::base::Split(version, " ")));
    }
}