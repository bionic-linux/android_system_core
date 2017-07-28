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

#ifndef _INIT_SERVICE_H
#define _INIT_SERVICE_H

#include <sys/types.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <android-base/chrono_utils.h>
#include <cutils/iosched_policy.h>

#include "action.h"
#include "capabilities.h"
#include "descriptors.h"
#include "keyword_map.h"
#include "parser.h"

#define SVC_DISABLED 0x001        // do not autostart with class
#define SVC_ONESHOT 0x002         // do not restart on exit
#define SVC_RUNNING 0x004         // currently active
#define SVC_RESTARTING 0x008      // waiting to restart
#define SVC_CONSOLE 0x010         // requires console
#define SVC_CRITICAL 0x020        // will reboot into recovery if keeps crashing
#define SVC_RESET 0x040           // Use when stopping a process,
                                  // but not disabling so it can be restarted with its class.
#define SVC_RC_DISABLED 0x080     // Remember if the disabled flag was set in the rc script.
#define SVC_RESTART 0x100         // Use to safely restart (stop, wait, start) a service.
#define SVC_DISABLED_START 0x200  // A start was requested but it was disabled at the time.
#define SVC_EXEC 0x400  // This service was started by either 'exec' or 'exec_start' and stops
                        // init from processing more commands until it completes

#define SVC_SHUTDOWN_CRITICAL 0x800  // This service is critical for shutdown and
                                     // should not be killed during shutdown
#define SVC_TEMPORARY 0x1000  // This service was started by 'exec' and should be removed from the
                              // service list once it is reaped.

#define NR_SVC_SUPP_GIDS 12    // twelve supplementary groups

namespace android {
namespace init {

struct ServiceEnvironmentInfo {
    ServiceEnvironmentInfo();
    ServiceEnvironmentInfo(const std::string& name, const std::string& value);
    std::string name;
    std::string value;
};

class Service {
  public:
    Service(const std::string& name, const std::vector<std::string>& args);

    Service(const std::string& name, unsigned flags, uid_t uid, gid_t gid,
            const std::vector<gid_t>& supp_gids, const CapSet& capabilities,
            unsigned namespace_flags, const std::string& seclabel,
            const std::vector<std::string>& args);

    static std::unique_ptr<Service> MakeTemporaryService(const std::vector<std::string>& args);
    static bool IsWaitingForExec() { return is_waiting_for_exec_; }

    bool IsRunning() { return (flags_ & SVC_RUNNING) != 0; }
    bool ParseLine(const std::vector<std::string>& args, std::string* err);
    bool ExecStart();
    bool Start();
    bool StartIfNotDisabled();
    bool Enable();
    void Reset();
    void Stop();
    void Terminate();
    void Restart();
    void RestartIfNeeded(time_t* process_needs_restart_at);
    void Reap();
    void DumpState() const;
    void SetShutdownCritical() { flags_ |= SVC_SHUTDOWN_CRITICAL; }
    bool IsShutdownCritical() const { return (flags_ & SVC_SHUTDOWN_CRITICAL) != 0; }
    void SetExec() {
        flags_ |= SVC_EXEC;
        is_waiting_for_exec_ = true;
    }
    void UnSetExec() {
        flags_ &= ~SVC_EXEC;
        is_waiting_for_exec_ = false;
    }

    const std::string& name() const { return name_; }
    const std::set<std::string>& classnames() const { return classnames_; }
    unsigned flags() const { return flags_; }
    pid_t pid() const { return pid_; }
    android::base::boot_clock::time_point time_started() const { return time_started_; }
    int crash_count() const { return crash_count_; }
    uid_t uid() const { return uid_; }
    gid_t gid() const { return gid_; }
    unsigned namespace_flags() const { return namespace_flags_; }
    const std::vector<gid_t>& supp_gids() const { return supp_gids_; }
    const std::string& seclabel() const { return seclabel_; }
    const std::vector<int>& keycodes() const { return keycodes_; }
    int keychord_id() const { return keychord_id_; }
    void set_keychord_id(int keychord_id) { keychord_id_ = keychord_id; }
    IoSchedClass ioprio_class() const { return ioprio_class_; }
    int ioprio_pri() const { return ioprio_pri_; }
    int priority() const { return priority_; }
    int oom_score_adjust() const { return oom_score_adjust_; }
    bool process_cgroup_empty() const { return process_cgroup_empty_; }
    unsigned long start_order() const { return start_order_; }
    const std::vector<std::string>& args() const { return args_; }

  private:
    using OptionParser = bool (Service::*) (const std::vector<std::string>& args,
                                            std::string* err);
    class OptionParserMap;

    void NotifyStateChange(const std::string& new_state) const;
    void StopOrReset(int how);
    void ZapStdio() const;
    void OpenConsole() const;
    void KillProcessGroup(int signal);
    void SetProcessAttributes();

    bool ParseCapabilities(const std::vector<std::string>& args, std::string *err);
    bool ParseClass(const std::vector<std::string>& args, std::string* err);
    bool ParseConsole(const std::vector<std::string>& args, std::string* err);
    bool ParseCritical(const std::vector<std::string>& args, std::string* err);
    bool ParseDisabled(const std::vector<std::string>& args, std::string* err);
    bool ParseGroup(const std::vector<std::string>& args, std::string* err);
    bool ParsePriority(const std::vector<std::string>& args, std::string* err);
    bool ParseIoprio(const std::vector<std::string>& args, std::string* err);
    bool ParseKeycodes(const std::vector<std::string>& args, std::string* err);
    bool ParseOneshot(const std::vector<std::string>& args, std::string* err);
    bool ParseOnrestart(const std::vector<std::string>& args, std::string* err);
    bool ParseOomScoreAdjust(const std::vector<std::string>& args, std::string* err);
    bool ParseMemcgLimitInBytes(const std::vector<std::string>& args, std::string* err);
    bool ParseMemcgSoftLimitInBytes(const std::vector<std::string>& args, std::string* err);
    bool ParseMemcgSwappiness(const std::vector<std::string>& args, std::string* err);
    bool ParseNamespace(const std::vector<std::string>& args, std::string* err);
    bool ParseSeclabel(const std::vector<std::string>& args, std::string* err);
    bool ParseSetenv(const std::vector<std::string>& args, std::string* err);
    bool ParseShutdown(const std::vector<std::string>& args, std::string* err);
    bool ParseSocket(const std::vector<std::string>& args, std::string* err);
    bool ParseFile(const std::vector<std::string>& args, std::string* err);
    bool ParseUser(const std::vector<std::string>& args, std::string* err);
    bool ParseWritepid(const std::vector<std::string>& args, std::string* err);

    template <typename T>
    bool AddDescriptor(const std::vector<std::string>& args, std::string* err);

    static unsigned long next_start_order_;
    static bool is_waiting_for_exec_;

    std::string name_;
    std::set<std::string> classnames_;
    std::string console_;

    unsigned flags_;
    pid_t pid_;
    android::base::boot_clock::time_point time_started_;  // time of last start
    android::base::boot_clock::time_point time_crashed_;  // first crash within inspection window
    int crash_count_;                     // number of times crashed within window

    uid_t uid_;
    gid_t gid_;
    std::vector<gid_t> supp_gids_;
    CapSet capabilities_;
    unsigned namespace_flags_;

    std::string seclabel_;

    std::vector<std::unique_ptr<DescriptorInfo>> descriptors_;
    std::vector<ServiceEnvironmentInfo> envvars_;

    Action onrestart_;  // Commands to execute on restart.

    std::vector<std::string> writepid_files_;

    // keycodes for triggering this service via /dev/keychord
    std::vector<int> keycodes_;
    int keychord_id_;

    IoSchedClass ioprio_class_;
    int ioprio_pri_;
    int priority_;

    int oom_score_adjust_;

    int swappiness_;
    int soft_limit_in_bytes_;
    int limit_in_bytes_;

    bool process_cgroup_empty_ = false;

    unsigned long start_order_;

    std::vector<std::string> args_;
};

class ServiceManager {
  public:
    static ServiceManager& GetInstance();

    // Exposed for testing
    ServiceManager();

    void AddService(std::unique_ptr<Service> service);
    Service* FindServiceByName(const std::string& name) const;
    Service* FindServiceByPid(pid_t pid) const;
    Service* FindServiceByKeychord(int keychord_id) const;
    void ForEachService(const std::function<void(Service*)>& callback) const;
    void ForEachServiceShutdownOrder(const std::function<void(Service*)>& callback) const;
    void ForEachServiceInClass(const std::string& classname,
                               void (*func)(Service* svc)) const;
    void ForEachServiceWithFlags(unsigned matchflags,
                             void (*func)(Service* svc)) const;
    void ReapAnyOutstandingChildren();
    void RemoveService(const Service& svc);
    void DumpState() const;

  private:
    // Cleans up a child process that exited.
    // Returns true iff a children was cleaned up.
    bool ReapOneProcess();

    std::vector<std::unique_ptr<Service>> services_;
};

class ServiceParser : public SectionParser {
  public:
    ServiceParser(ServiceManager* service_manager)
        : service_manager_(service_manager), service_(nullptr) {}
    bool ParseSection(std::vector<std::string>&& args, const std::string& filename, int line,
                      std::string* err) override;
    bool ParseLineSection(std::vector<std::string>&& args, int line, std::string* err) override;
    void EndSection() override;

  private:
    bool IsValidName(const std::string& name) const;

    ServiceManager* service_manager_;
    std::unique_ptr<Service> service_;
};

}  // namespace init
}  // namespace android

#endif
