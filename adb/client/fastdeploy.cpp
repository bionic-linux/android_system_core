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

#include "fastdeploy.h"

#include <string.h>
#include <algorithm>
#include <array>
#include <memory>

#include "android-base/file.h"
#include "android-base/strings.h"
#include "androidfw/ResourceTypes.h"
#include "androidfw/ZipFileRO.h"
#include "client/file_sync_client.h"
#include "commandline.h"
#include "deployagent.inc"        // Generated include via build rule.
#include "deployagentscript.inc"  // Generated include via build rule.
#include "fastdeploy/deploypatchgenerator/deploy_patch_generator.h"
#include "fastdeploy/deploypatchgenerator/patch_utils.h"
#include "fastdeploy/proto/ApkEntry.pb.h"
#include "fastdeploycallbacks.h"
#include "sysdeps.h"

#include "adb_utils.h"

static constexpr long kRequiredAgentVersion = 0x00000003;

static constexpr int kPackageMissing = 3;
static constexpr int kInvalidAgentVersion = 4;

static constexpr const char* kDeviceAgentFile = "/data/local/tmp/deployagent.jar";
static constexpr const char* kDeviceAgentScript = "/data/local/tmp/deployagent";

static constexpr bool g_verbose_timings = true;
static FastDeploy_AgentUpdateStrategy g_agent_update_strategy =
        FastDeploy_AgentUpdateDifferentVersion;

using APKMetaData = com::android::fastdeploy::APKMetaData;

namespace {

struct TimeReporter {
    TimeReporter(const char* label) : label_(label) {}
    ~TimeReporter() {
        if (g_verbose_timings) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_);
            fprintf(stderr, "%s finished in %lldms\n", label_, (long long)duration.count());
        }
    }

  private:
    const char* label_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace

int get_device_api_level() {
    TimeReporter reporter(__func__);
    std::vector<char> sdkVersionOutputBuffer;
    std::vector<char> sdkVersionErrorBuffer;
    int api_level = -1;

    int statusCode = capture_shell_command("getprop ro.build.version.sdk", &sdkVersionOutputBuffer,
                                           &sdkVersionErrorBuffer);
    if (statusCode == 0 && sdkVersionOutputBuffer.size() > 0) {
        api_level = strtol((char*)sdkVersionOutputBuffer.data(), NULL, 10);
    }

    return api_level;
}

void fastdeploy_agent_update_strategy(FastDeploy_AgentUpdateStrategy agentUpdateStrategy) {
    g_agent_update_strategy = agentUpdateStrategy;
}

static bool deploy_agent(bool checkTimeStamps) {
    TimeReporter reporter(__func__);
    std::vector<const char*> srcs;
    // TODO: Deploy agent from bin2c directly instead of writing to disk first.
    TemporaryFile tempAgent;
    android::base::WriteFully(tempAgent.fd, kDeployAgent, sizeof(kDeployAgent));
    srcs.push_back(tempAgent.path);
    if (!do_sync_push(srcs, kDeviceAgentFile, checkTimeStamps)) {
        error_exit("Failed to push fastdeploy agent to device.");
    }
    srcs.clear();
    // TODO: Deploy agent from bin2c directly instead of writing to disk first.
    TemporaryFile tempAgentScript;
    android::base::WriteFully(tempAgentScript.fd, kDeployAgentScript, sizeof(kDeployAgentScript));
    srcs.push_back(tempAgentScript.path);
    if (!do_sync_push(srcs, kDeviceAgentScript, checkTimeStamps)) {
        error_exit("Failed to push fastdeploy agent script to device.");
    }
    srcs.clear();
    // on windows the shell script might have lost execute permission
    // so need to set this explicitly
    const char* kChmodCommandPattern = "chmod 777 %s";
    std::string chmodCommand =
            android::base::StringPrintf(kChmodCommandPattern, kDeviceAgentScript);
    int ret = send_shell_command(chmodCommand);
    if (ret != 0) {
        error_exit("Error executing %s returncode: %d", chmodCommand.c_str(), ret);
    }

    return true;
}

static void update_agent_if_necessary() {
    switch (g_agent_update_strategy) {
        case FastDeploy_AgentUpdateAlways:
            deploy_agent(/*checkTimeStamps=*/false);
            break;
        case FastDeploy_AgentUpdateNewerTimeStamp:
            deploy_agent(/*checkTimeStamps=*/true);
            break;
        default:
            break;
    }
}

static long parse_agent_version(const std::vector<char>& versionBuffer) {
    long version = -1;
    if (!versionBuffer.empty()) {
        version = strtol((char*)versionBuffer.data(), NULL, 16);
    }
    return version;
}

static std::string get_string_from_utf16(const char16_t* input, int input_len) {
    ssize_t utf8_length = utf16_to_utf8_length(input, input_len);
    if (utf8_length <= 0) {
        return {};
    }
    std::string utf8;
    utf8.resize(utf8_length);
    utf16_to_utf8(input, input_len, &*utf8.begin(), utf8_length + 1);
    return utf8;
}

std::string get_packagename_from_apk(const char* apkPath) {
#undef open
    std::unique_ptr<android::ZipFileRO> zipFile(android::ZipFileRO::open(apkPath));
#define open ___xxx_unix_open
    if (zipFile == nullptr) {
        perror_exit("Could not open %s", apkPath);
    }
    android::ZipEntryRO entry = zipFile->findEntryByName("AndroidManifest.xml");
    if (entry == nullptr) {
        error_exit("Could not find AndroidManifest.xml inside %s", apkPath);
    }
    uint32_t manifest_len = 0;
    if (!zipFile->getEntryInfo(entry, NULL, &manifest_len, NULL, NULL, NULL, NULL)) {
        error_exit("Could not read AndroidManifest.xml inside %s", apkPath);
    }
    std::vector<char> manifest_data(manifest_len);
    if (!zipFile->uncompressEntry(entry, manifest_data.data(), manifest_len)) {
        error_exit("Could not uncompress AndroidManifest.xml inside %s", apkPath);
    }
    android::ResXMLTree tree;
    android::status_t setto_status = tree.setTo(manifest_data.data(), manifest_len, true);
    if (setto_status != android::OK) {
        error_exit("Could not parse AndroidManifest.xml inside %s", apkPath);
    }
    android::ResXMLParser::event_code_t code;
    while ((code = tree.next()) != android::ResXMLParser::BAD_DOCUMENT &&
           code != android::ResXMLParser::END_DOCUMENT) {
        switch (code) {
            case android::ResXMLParser::START_TAG: {
                size_t element_name_length;
                const char16_t* element_name = tree.getElementName(&element_name_length);
                if (element_name == nullptr) {
                    continue;
                }
                std::u16string element_name_string(element_name, element_name_length);
                if (element_name_string == u"manifest") {
                    for (size_t i = 0; i < tree.getAttributeCount(); i++) {
                        size_t attribute_name_length;
                        const char16_t* attribute_name_text =
                                tree.getAttributeName(i, &attribute_name_length);
                        if (attribute_name_text == nullptr) {
                            continue;
                        }
                        std::u16string attribute_name_string(attribute_name_text,
                                                             attribute_name_length);
                        if (attribute_name_string == u"package") {
                            size_t attribute_value_length;
                            const char16_t* attribute_value_text =
                                    tree.getAttributeStringValue(i, &attribute_value_length);
                            if (attribute_value_text == nullptr) {
                                continue;
                            }
                            return get_string_from_utf16(attribute_value_text,
                                                         attribute_value_length);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    error_exit("Could not find package name tag in AndroidManifest.xml inside %s", apkPath);
}

std::optional<APKMetaData> extract_metadata(const char* apkPath) {
    update_agent_if_necessary();

    TimeReporter reporter(__func__);

    std::string packageName = get_packagename_from_apk(apkPath);

    // Dump apk command checks the required vs current agent version and if they match then returns
    // the APK dump for package. Doing this in a single call saves round-trip and agent launch time.
    constexpr const char* kAgentDumpCommandPattern = "/data/local/tmp/deployagent dump %ld %s";
    std::string dumpCommand = android::base::StringPrintf(
            kAgentDumpCommandPattern, kRequiredAgentVersion, packageName.c_str());

    std::vector<char> dumpOutBuffer;
    std::vector<char> dumpErrorBuffer;
    int returnCode = capture_shell_command(dumpCommand.c_str(), &dumpOutBuffer, &dumpErrorBuffer);
    if (returnCode >= kInvalidAgentVersion) {
        // Agent has wrong version or missing.
        long agent_version = parse_agent_version(dumpOutBuffer);
        if (agent_version < 0) {
            printf("Could not detect agent on device, deploying\n");
        } else {
            printf("Device agent version is (%ld), (%ld) is required, re-deploying\n",
                   agent_version, kRequiredAgentVersion);
        }
        deploy_agent(/*checkTimeStamps=*/false);

        // Retry with new agent.
        dumpOutBuffer.clear();
        dumpErrorBuffer.clear();
        returnCode = capture_shell_command(dumpCommand.c_str(), &dumpOutBuffer, &dumpErrorBuffer);
    }
    if (returnCode != 0) {
        if (returnCode == kInvalidAgentVersion) {
            long agent_version = parse_agent_version(dumpOutBuffer);
            error_exit(
                    "After update agent version remains incorrect! Expected %ld but version is %ld",
                    kRequiredAgentVersion, agent_version);
        }
        if (returnCode == kPackageMissing) {
            fprintf(stderr, "Package %s not found, falling back to install\n", packageName.c_str());
            return {};
        }
        fprintf(stderr, "Executing %s returned %d\n", dumpCommand.c_str(), returnCode);
        fprintf(stderr, "%*s\n", int(dumpErrorBuffer.size()), dumpErrorBuffer.data());
        error_exit("Aborting");
    }

    com::android::fastdeploy::APKDump dump;
    if (!dump.ParseFromArray(dumpOutBuffer.data(), dumpOutBuffer.size())) {
        fprintf(stderr, "Can't parse output of %s\n", dumpCommand.c_str());
        error_exit("Aborting");
    }

    return PatchUtils::GetDeviceAPKMetaData(dump);
}

unique_fd install_patch(int argc, const char** argv) {
    TimeReporter reporter(__func__);
    constexpr char kAgentApplyServicePattern[] = "shell:/data/local/tmp/deployagent apply - -pm %s";

    std::vector<unsigned char> applyOutputBuffer;
    std::vector<unsigned char> applyErrorBuffer;
    std::string argsString;

    bool rSwitchPresent = false;
    for (int i = 0; i < argc; i++) {
        argsString.append(argv[i]);
        argsString.append(" ");
        if (!strcmp(argv[i], "-r")) {
            rSwitchPresent = true;
        }
    }
    if (!rSwitchPresent) {
        argsString.append("-r");
    }

    std::string error;
    std::string applyPatchServiceString =
            android::base::StringPrintf(kAgentApplyServicePattern, argsString.c_str());
    unique_fd fd{adb_connect(applyPatchServiceString, &error)};
    if (fd < 0) {
        error_exit("Executing %s returned %s", applyPatchServiceString.c_str(), error.c_str());
    }
    return fd;
}

unique_fd apply_patch_on_device(const char* outputPath) {
    TimeReporter reporter(__func__);
    constexpr char kAgentApplyServicePattern[] = "shell:/data/local/tmp/deployagent apply - -o %s";

    std::string error;
    std::string applyPatchServiceString =
            android::base::StringPrintf(kAgentApplyServicePattern, outputPath);
    unique_fd fd{adb_connect(applyPatchServiceString, &error)};
    if (fd < 0) {
        error_exit("Executing %s returned %s", applyPatchServiceString.c_str(), error.c_str());
    }
    return fd;
}

int stream_patch(const char* apkPath, APKMetaData metadata, unique_fd patchFd) {
    TimeReporter reporter(__func__);
    DeployPatchGenerator generator(/*is_verbose=*/false);
    bool success = generator.CreatePatch(apkPath, std::move(metadata), patchFd);
    if (!success) {
        error_exit("Failed to create patch for %s", apkPath);
    }
    return read_and_dump(patchFd.get());
}
