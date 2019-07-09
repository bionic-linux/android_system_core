/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "service_parser.h"

#include <linux/input.h>

#include <algorithm>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <hidl-util/FQName.h>
#include <system/thread_defs.h>

#include "rlimit_parser.h"
#include "util.h"

#if defined(__ANDROID__)
#include <android/api-level.h>
#include <sys/system_properties.h>

#include "selinux.h"
#else
#include "host_init_stubs.h"
#endif

using android::base::ParseInt;
using android::base::Split;
using android::base::StartsWith;

namespace android {
namespace init {

Result<void> ServiceParser::ParseCapabilities(std::vector<std::string>&& args) {
    service_->capabilities_ = 0;

    if (!CapAmbientSupported()) {
        return Error()
               << "capabilities requested but the kernel does not support ambient capabilities";
    }

    unsigned int last_valid_cap = GetLastValidCap();
    if (last_valid_cap >= service_->capabilities_->size()) {
        LOG(WARNING) << "last valid run-time capability is larger than CAP_LAST_CAP";
    }

    for (size_t i = 1; i < args.size(); i++) {
        const std::string& arg = args[i];
        int res = LookupCap(arg);
        if (res < 0) {
            return Errorf("invalid capability '{}'", arg);
        }
        unsigned int cap = static_cast<unsigned int>(res);  // |res| is >= 0.
        if (cap > last_valid_cap) {
            return Errorf("capability '{}' not supported by the kernel", arg);
        }
        (*service_->capabilities_)[cap] = true;
    }
    return {};
}

Result<void> ServiceParser::ParseClass(std::vector<std::string>&& args) {
    service_->classnames_ = std::set<std::string>(args.begin() + 1, args.end());
    return {};
}

Result<void> ServiceParser::ParseConsole(std::vector<std::string>&& args) {
    service_->flags_ |= SVC_CONSOLE;
    service_->proc_attr_.console = args.size() > 1 ? "/dev/" + args[1] : "";
    return {};
}

Result<void> ServiceParser::ParseCritical(std::vector<std::string>&& args) {
    service_->flags_ |= SVC_CRITICAL;
    return {};
}

Result<void> ServiceParser::ParseDisabled(std::vector<std::string>&& args) {
    service_->flags_ |= SVC_DISABLED;
    service_->flags_ |= SVC_RC_DISABLED;
    return {};
}

Result<void> ServiceParser::ParseEnterNamespace(std::vector<std::string>&& args) {
    if (args[1] != "net") {
        return Error() << "Init only supports entering network namespaces";
    }
    if (!service_->namespaces_.namespaces_to_enter.empty()) {
        return Error() << "Only one network namespace may be entered";
    }
    // Network namespaces require that /sys is remounted, otherwise the old adapters will still be
    // present. Therefore, they also require mount namespaces.
    service_->namespaces_.flags |= CLONE_NEWNS;
    service_->namespaces_.namespaces_to_enter.emplace_back(CLONE_NEWNET, std::move(args[2]));
    return {};
}

Result<void> ServiceParser::ParseGroup(std::vector<std::string>&& args) {
    auto gid = DecodeUid(args[1]);
    if (!gid) {
        return Error() << "Unable to decode GID for '" << args[1] << "': " << gid.error();
    }
    service_->proc_attr_.gid = *gid;

    for (std::size_t n = 2; n < args.size(); n++) {
        gid = DecodeUid(args[n]);
        if (!gid) {
            return Error() << "Unable to decode GID for '" << args[n] << "': " << gid.error();
        }
        service_->proc_attr_.supp_gids.emplace_back(*gid);
    }
    return {};
}

Result<void> ServiceParser::ParsePriority(std::vector<std::string>&& args) {
    service_->proc_attr_.priority = 0;
    if (!ParseInt(args[1], &service_->proc_attr_.priority,
                  static_cast<int>(ANDROID_PRIORITY_HIGHEST),  // highest is negative
                  static_cast<int>(ANDROID_PRIORITY_LOWEST))) {
        return Errorf("process priority value must be range {} - {}", ANDROID_PRIORITY_HIGHEST,
                      ANDROID_PRIORITY_LOWEST);
    }
    return {};
}

Result<void> ServiceParser::ParseInterface(std::vector<std::string>&& args) {
    const std::string& interface_name = args[1];
    const std::string& instance_name = args[2];

    FQName fq_name;
    if (!FQName::parse(interface_name, &fq_name)) {
        return Error() << "Invalid fully-qualified name for interface '" << interface_name << "'";
    }

    if (!fq_name.isFullyQualified()) {
        return Error() << "Interface name not fully-qualified '" << interface_name << "'";
    }

    if (fq_name.isValidValueName()) {
        return Error() << "Interface name must not be a value name '" << interface_name << "'";
    }

    const std::string fullname = interface_name + "/" + instance_name;

    for (const auto& svc : *service_list_) {
        if (svc->interfaces().count(fullname) > 0) {
            return Error() << "Interface '" << fullname << "' redefined in " << service_->name()
                           << " but is already defined by " << svc->name();
        }
    }

    service_->interfaces_.insert(fullname);

    return {};
}

Result<void> ServiceParser::ParseIoprio(std::vector<std::string>&& args) {
    if (!ParseInt(args[2], &service_->proc_attr_.ioprio_pri, 0, 7)) {
        return Error() << "priority value must be range 0 - 7";
    }

    if (args[1] == "rt") {
        service_->proc_attr_.ioprio_class = IoSchedClass_RT;
    } else if (args[1] == "be") {
        service_->proc_attr_.ioprio_class = IoSchedClass_BE;
    } else if (args[1] == "idle") {
        service_->proc_attr_.ioprio_class = IoSchedClass_IDLE;
    } else {
        return Error() << "ioprio option usage: ioprio <rt|be|idle> <0-7>";
    }

    return {};
}

Result<void> ServiceParser::ParseKeycodes(std::vector<std::string>&& args) {
    auto it = args.begin() + 1;
    if (args.size() == 2 && StartsWith(args[1], "$")) {
        std::string expanded;
        if (!expand_props(args[1], &expanded)) {
            return Error() << "Could not expand property '" << args[1] << "'";
        }

        // If the property is not set, it defaults to none, in which case there are no keycodes
        // for this service.
        if (expanded == "none") {
            return {};
        }

        args = Split(expanded, ",");
        it = args.begin();
    }

    for (; it != args.end(); ++it) {
        int code;
        if (ParseInt(*it, &code, 0, KEY_MAX)) {
            for (auto& key : service_->keycodes_) {
                if (key == code) return Error() << "duplicate keycode: " << *it;
            }
            service_->keycodes_.insert(
                    std::upper_bound(service_->keycodes_.begin(), service_->keycodes_.end(), code),
                    code);
        } else {
            return Error() << "invalid keycode: " << *it;
        }
    }
    return {};
}

Result<void> ServiceParser::ParseOneshot(std::vector<std::string>&& args) {
    service_->flags_ |= SVC_ONESHOT;
    return {};
}

Result<void> ServiceParser::ParseOnrestart(std::vector<std::string>&& args) {
    args.erase(args.begin());
    int line = service_->onrestart_.NumCommands() + 1;
    if (auto result = service_->onrestart_.AddCommand(std::move(args), line); !result) {
        return Error() << "cannot add Onrestart command: " << result.error();
    }
    return {};
}

Result<void> ServiceParser::ParseNamespace(std::vector<std::string>&& args) {
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "pid") {
            service_->namespaces_.flags |= CLONE_NEWPID;
            // PID namespaces require mount namespaces.
            service_->namespaces_.flags |= CLONE_NEWNS;
        } else if (args[i] == "mnt") {
            service_->namespaces_.flags |= CLONE_NEWNS;
        } else {
            return Error() << "namespace must be 'pid' or 'mnt'";
        }
    }
    return {};
}

Result<void> ServiceParser::ParseOomScoreAdjust(std::vector<std::string>&& args) {
    if (!ParseInt(args[1], &service_->oom_score_adjust_, -1000, 1000)) {
        return Error() << "oom_score_adjust value must be in range -1000 - +1000";
    }
    return {};
}

Result<void> ServiceParser::ParseOverride(std::vector<std::string>&& args) {
    service_->override_ = true;
    return {};
}

Result<void> ServiceParser::ParseMemcgSwappiness(std::vector<std::string>&& args) {
    if (!ParseInt(args[1], &service_->swappiness_, 0)) {
        return Error() << "swappiness value must be equal or greater than 0";
    }
    return {};
}

Result<void> ServiceParser::ParseMemcgLimitInBytes(std::vector<std::string>&& args) {
    if (!ParseInt(args[1], &service_->limit_in_bytes_, 0)) {
        return Error() << "limit_in_bytes value must be equal or greater than 0";
    }
    return {};
}

Result<void> ServiceParser::ParseMemcgLimitPercent(std::vector<std::string>&& args) {
    if (!ParseInt(args[1], &service_->limit_percent_, 0)) {
        return Error() << "limit_percent value must be equal or greater than 0";
    }
    return {};
}

Result<void> ServiceParser::ParseMemcgLimitProperty(std::vector<std::string>&& args) {
    service_->limit_property_ = std::move(args[1]);
    return {};
}

Result<void> ServiceParser::ParseMemcgSoftLimitInBytes(std::vector<std::string>&& args) {
    if (!ParseInt(args[1], &service_->soft_limit_in_bytes_, 0)) {
        return Error() << "soft_limit_in_bytes value must be equal or greater than 0";
    }
    return {};
}

Result<void> ServiceParser::ParseProcessRlimit(std::vector<std::string>&& args) {
    auto rlimit = ParseRlimit(args);
    if (!rlimit) return rlimit.error();

    service_->proc_attr_.rlimits.emplace_back(*rlimit);
    return {};
}

Result<void> ServiceParser::ParseRestartPeriod(std::vector<std::string>&& args) {
    int period;
    if (!ParseInt(args[1], &period, 5)) {
        return Error() << "restart_period value must be an integer >= 5";
    }
    service_->restart_period_ = std::chrono::seconds(period);
    return {};
}

Result<void> ServiceParser::ParseSeclabel(std::vector<std::string>&& args) {
    service_->seclabel_ = std::move(args[1]);
    return {};
}

Result<void> ServiceParser::ParseSigstop(std::vector<std::string>&& args) {
    service_->sigstop_ = true;
    return {};
}

Result<void> ServiceParser::ParseSetenv(std::vector<std::string>&& args) {
    service_->environment_vars_.emplace_back(std::move(args[1]), std::move(args[2]));
    return {};
}

Result<void> ServiceParser::ParseShutdown(std::vector<std::string>&& args) {
    if (args[1] == "critical") {
        service_->flags_ |= SVC_SHUTDOWN_CRITICAL;
        return {};
    }
    return Error() << "Invalid shutdown option";
}

Result<void> ServiceParser::ParseTimeoutPeriod(std::vector<std::string>&& args) {
    int period;
    if (!ParseInt(args[1], &period, 1)) {
        return Error() << "timeout_period value must be an integer >= 1";
    }
    service_->timeout_period_ = std::chrono::seconds(period);
    return {};
}

template <typename T>
Result<void> ServiceParser::AddDescriptor(std::vector<std::string>&& args) {
    int perm = args.size() > 3 ? std::strtoul(args[3].c_str(), 0, 8) : -1;
    Result<uid_t> uid = 0;
    Result<gid_t> gid = 0;
    std::string context = args.size() > 6 ? args[6] : "";

    if (args.size() > 4) {
        uid = DecodeUid(args[4]);
        if (!uid) {
            return Error() << "Unable to find UID for '" << args[4] << "': " << uid.error();
        }
    }

    if (args.size() > 5) {
        gid = DecodeUid(args[5]);
        if (!gid) {
            return Error() << "Unable to find GID for '" << args[5] << "': " << gid.error();
        }
    }

    auto descriptor = std::make_unique<T>(args[1], args[2], *uid, *gid, perm, context);

    auto old = std::find_if(
            service_->descriptors_.begin(), service_->descriptors_.end(),
            [&descriptor](const auto& other) { return descriptor.get() == other.get(); });

    if (old != service_->descriptors_.end()) {
        return Error() << "duplicate descriptor " << args[1] << " " << args[2];
    }

    service_->descriptors_.emplace_back(std::move(descriptor));
    return {};
}

// name type perm [ uid gid context ]
Result<void> ServiceParser::ParseSocket(std::vector<std::string>&& args) {
    if (!StartsWith(args[2], "dgram") && !StartsWith(args[2], "stream") &&
        !StartsWith(args[2], "seqpacket")) {
        return Error() << "socket type must be 'dgram', 'stream' or 'seqpacket'";
    }
    return AddDescriptor<SocketInfo>(std::move(args));
}

// name type perm [ uid gid context ]
Result<void> ServiceParser::ParseFile(std::vector<std::string>&& args) {
    if (args[2] != "r" && args[2] != "w" && args[2] != "rw") {
        return Error() << "file type must be 'r', 'w' or 'rw'";
    }
    std::string expanded;
    if (!expand_props(args[1], &expanded)) {
        return Error() << "Could not expand property in file path '" << args[1] << "'";
    }
    args[1] = std::move(expanded);
    if ((args[1][0] != '/') || (args[1].find("../") != std::string::npos)) {
        return Error() << "file name must not be relative";
    }
    return AddDescriptor<FileInfo>(std::move(args));
}

Result<void> ServiceParser::ParseUser(std::vector<std::string>&& args) {
    auto uid = DecodeUid(args[1]);
    if (!uid) {
        return Error() << "Unable to find UID for '" << args[1] << "': " << uid.error();
    }
    service_->proc_attr_.uid = *uid;
    return {};
}

Result<void> ServiceParser::ParseWritepid(std::vector<std::string>&& args) {
    args.erase(args.begin());
    service_->writepid_files_ = std::move(args);
    return {};
}

Result<void> ServiceParser::ParseUpdatable(std::vector<std::string>&& args) {
    service_->updatable_ = true;
    return {};
}

class ServiceParser::OptionParserMap : public KeywordMap<OptionParser> {
  public:
    OptionParserMap() {}

  private:
    const Map& map() const override;
};

const ServiceParser::OptionParserMap::Map& ServiceParser::OptionParserMap::map() const {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    // clang-format off
    static const Map option_parsers = {
        {"capabilities",
                        {0,     kMax, &ServiceParser::ParseCapabilities}},
        {"class",       {1,     kMax, &ServiceParser::ParseClass}},
        {"console",     {0,     1,    &ServiceParser::ParseConsole}},
        {"critical",    {0,     0,    &ServiceParser::ParseCritical}},
        {"disabled",    {0,     0,    &ServiceParser::ParseDisabled}},
        {"enter_namespace",
                        {2,     2,    &ServiceParser::ParseEnterNamespace}},
        {"file",        {2,     2,    &ServiceParser::ParseFile}},
        {"group",       {1,     NR_SVC_SUPP_GIDS + 1, &ServiceParser::ParseGroup}},
        {"interface",   {2,     2,    &ServiceParser::ParseInterface}},
        {"ioprio",      {2,     2,    &ServiceParser::ParseIoprio}},
        {"keycodes",    {1,     kMax, &ServiceParser::ParseKeycodes}},
        {"memcg.limit_in_bytes",
                        {1,     1,    &ServiceParser::ParseMemcgLimitInBytes}},
        {"memcg.limit_percent",
                        {1,     1,    &ServiceParser::ParseMemcgLimitPercent}},
        {"memcg.limit_property",
                        {1,     1,    &ServiceParser::ParseMemcgLimitProperty}},
        {"memcg.soft_limit_in_bytes",
                        {1,     1,    &ServiceParser::ParseMemcgSoftLimitInBytes}},
        {"memcg.swappiness",
                        {1,     1,    &ServiceParser::ParseMemcgSwappiness}},
        {"namespace",   {1,     2,    &ServiceParser::ParseNamespace}},
        {"oneshot",     {0,     0,    &ServiceParser::ParseOneshot}},
        {"onrestart",   {1,     kMax, &ServiceParser::ParseOnrestart}},
        {"oom_score_adjust",
                        {1,     1,    &ServiceParser::ParseOomScoreAdjust}},
        {"override",    {0,     0,    &ServiceParser::ParseOverride}},
        {"priority",    {1,     1,    &ServiceParser::ParsePriority}},
        {"restart_period",
                        {1,     1,    &ServiceParser::ParseRestartPeriod}},
        {"rlimit",      {3,     3,    &ServiceParser::ParseProcessRlimit}},
        {"seclabel",    {1,     1,    &ServiceParser::ParseSeclabel}},
        {"setenv",      {2,     2,    &ServiceParser::ParseSetenv}},
        {"shutdown",    {1,     1,    &ServiceParser::ParseShutdown}},
        {"sigstop",     {0,     0,    &ServiceParser::ParseSigstop}},
        {"socket",      {3,     6,    &ServiceParser::ParseSocket}},
        {"timeout_period",
                        {1,     1,    &ServiceParser::ParseTimeoutPeriod}},
        {"updatable",   {0,     0,    &ServiceParser::ParseUpdatable}},
        {"user",        {1,     1,    &ServiceParser::ParseUser}},
        {"writepid",    {1,     kMax, &ServiceParser::ParseWritepid}},
    };
    // clang-format on
    return option_parsers;
}

Result<void> ServiceParser::ParseSection(std::vector<std::string>&& args,
                                         const std::string& filename, int line) {
    if (args.size() < 3) {
        return Error() << "services must have a name and a program";
    }

    const std::string& name = args[1];
    if (!IsValidName(name)) {
        return Error() << "invalid service name '" << name << "'";
    }

    filename_ = filename;

    Subcontext* restart_action_subcontext = nullptr;
    if (subcontexts_) {
        for (auto& subcontext : *subcontexts_) {
            if (StartsWith(filename, subcontext.path_prefix())) {
                restart_action_subcontext = &subcontext;
                break;
            }
        }
    }

    std::vector<std::string> str_args(args.begin() + 2, args.end());

    if (SelinuxGetVendorAndroidVersion() <= __ANDROID_API_P__) {
        if (str_args[0] == "/sbin/watchdogd") {
            str_args[0] = "/system/bin/watchdogd";
        }
    }

    service_ = std::make_unique<Service>(name, restart_action_subcontext, str_args);
    return {};
}

Result<void> ServiceParser::ParseLineSection(std::vector<std::string>&& args, int line) {
    if (!service_) {
        return {};
    }

    static const OptionParserMap parser_map;
    auto parser = parser_map.FindFunction(args);

    if (!parser) return parser.error();

    return std::invoke(*parser, this, std::move(args));
}

Result<void> ServiceParser::EndSection() {
    if (!service_) {
        return {};
    }

    if (interface_inheritance_hierarchy_) {
        std::set<std::string> interface_names;
        for (const std::string& intf : service_->interfaces()) {
            interface_names.insert(Split(intf, "/")[0]);
        }
        for (const std::string& intf : interface_names) {
            if (interface_inheritance_hierarchy_->count(intf) == 0) {
                return Error() << "Interface is not in the known set of hidl_interfaces: '" << intf
                               << "'. Please ensure the interface is spelled correctly and built "
                               << "by a hidl_interface target.";
            }
            const std::set<std::string>& required_interfaces =
                    (*interface_inheritance_hierarchy_)[intf];
            std::set<std::string> diff;
            std::set_difference(required_interfaces.begin(), required_interfaces.end(),
                                interface_names.begin(), interface_names.end(),
                                std::inserter(diff, diff.begin()));
            if (!diff.empty()) {
                return Error() << "Interface '" << intf << "' requires its full inheritance "
                               << "hierarchy to be listed in this init_rc file. Missing "
                               << "interfaces: [" << base::Join(diff, " ") << "]";
            }
        }
    }

    Service* old_service = service_list_->FindService(service_->name());
    if (old_service) {
        if (!service_->is_override()) {
            return Error() << "ignored duplicate definition of service '" << service_->name()
                           << "'";
        }

        if (StartsWith(filename_, "/apex/") && !old_service->is_updatable()) {
            return Error() << "cannot update a non-updatable service '" << service_->name()
                           << "' with a config in APEX";
        }

        service_list_->RemoveService(*old_service);
        old_service = nullptr;
    }

    service_list_->AddService(std::move(service_));

    return {};
}

bool ServiceParser::IsValidName(const std::string& name) const {
    // Property names can be any length, but may only contain certain characters.
    // Property values can contain any characters, but may only be a certain length.
    // (The latter restriction is needed because `start` and `stop` work by writing
    // the service name to the "ctl.start" and "ctl.stop" properties.)
    return IsLegalPropertyName("init.svc." + name) && name.size() <= PROP_VALUE_MAX;
}

}  // namespace init
}  // namespace android
