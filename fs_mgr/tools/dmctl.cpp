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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/dm-ioctl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/unique_fd.h>
#include <dm.h>

#include <functional>
#include <iomanip>
#include <ios>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using DeviceMapper = ::android::dm::DeviceMapper;
using DmTarget = ::android::dm::DmTarget;

static int Usage(void) {
    std::cerr << "usage: dmctl <command> [command options]";
    std::cerr << "commands:";
    std::cerr << "  create <dm-name> [dm-target> [-lo <filename>] <dm-target-args>]";
    std::cerr, "  delete <dm-name>";
    std::cerr, "  list";
    std::cerr, "  help";
    return -EINVAL;
}

static int DmCreateCmdHandler(int argc, char** argv) {
    if (argc < 1) {
        std::cerr << "DmCreateCmdHandler: atleast 'name' MUST be provided for target device";
        return -EINVAL;
    }

    std::string name = argv[0];
    DeviceMapper& dm = DeviceMapper::Instance();
    if (!dm.CreateDevice(name)) {
        std::cerr << "DmCreateCmdHandler: Failed to create " << name << " device";
        return -EIO;
    }

    // if we also have target specified
    if (argc > 1) {
        // fall through for now. This will eventually create a DmTarget() based on the target name
        // passing it the table that is specified at the command line
    }

    return 0;
}

static int DmDeleteCmdHandler(int argc, char** argv) {
    if (argc < 1) {
        std::cerr << "DmCreateCmdHandler: atleast 'name' MUST be provided for target device";
        return -EINVAL;
    }

    std::string name = argv[0];
    DeviceMapper& dm = DeviceMapper::Instance();
    if (!dm.DeleteDevice(name)) {
        std::cerr << "DmCreateCmdHandler: Failed to create " << name << " device";
        return -EIO;
    }

    return 0;
}

static int DmListCmdHandler(int /* argc */, char** /* argv */) {
    std::cout << "Available Device Mapper Targets:" << std::endl;

    DeviceMapper& dm = DeviceMapper::Instance();
    std::vector<DmTarget> targets;
    if (!dm.GetAvailableTargets(&targets)) {
        std::cerr << "Failed to read available device mapper targets" << std::endl;
        return -errno;
    }

    if (targets.empty()) {
        std::cout << "  <empty>" << std::endl;
        return 0;
    }

    for (const auto& target : targets) {
        std::cout << std::left << std::setw(20) << target.name() << " : " << target.version()
                  << std::endl;
    }

    return 0;
}

static int HelpCmdHandler(int /* argc */, char** /* argv */) {
    Usage();
    return 0;
}

// clang-format off
static std::map<std::string, std::function<int(int, char**)>> cmdmap = {
    { "create", DmCreateCmdHandler },
    { "delete", DmDeleteCmdHandler },
    { "list", DmListCmdHandler },
    { "help", HelpCmdHandler },
};
// clang-format on

int main(int argc, char** argv) {
    android::base::InitLogging(argv, &android::base::StderrLogger);
    if (argc < 2) {
        return Usage();
    }

    for (const auto& cmd : cmdmap) {
        if (cmd.first == argv[1]) {
            return cmd.second(argc - 2, argv + 2);
        }
    }

    return Usage();
}
