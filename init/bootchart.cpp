/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "bootchart.h"
#include "log.h"
#include "property_service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/stringprintf.h>

using android::base::StringPrintf;

static constexpr const char* LOG_STAT = "/data/bootchart/proc_stat.log";
static constexpr const char* LOG_PROCS = "/data/bootchart/proc_ps.log";
static constexpr const char* LOG_DISK = "/data/bootchart/proc_diskstats.log";
static constexpr const char* LOG_HEADER = "/data/bootchart/header";
static constexpr const char* LOG_ACCT = "/data/bootchart/kernel_pacct";

// Polling period in ms.
static constexpr int BOOTCHART_POLLING_MS = 200;

static long long g_last_bootchart_time;

static bool g_bootcharting = false;

static FILE* stat_log;
static FILE* procs_log;
static FILE* disks_log;

static long long get_uptime_jiffies() {
    std::string uptime;
    if (!android::base::ReadFileToString("/proc/uptime", &uptime)) {
        return 0;
    }
    return 100LL * strtod(uptime.c_str(), NULL);
}

static void log_header() {
    char date[32];
    time_t now_t = time(NULL);
    struct tm now = *localtime(&now_t);
    strftime(date, sizeof(date), "%F %T", &now);

    utsname uts;
    if (uname(&uts) == -1) {
        return;
    }

    std::string fingerprint = property_get("ro.build.fingerprint");
    if (fingerprint.empty()) {
        return;
    }

    std::string kernel_cmdline;
    android::base::ReadFileToString("/proc/cmdline", &kernel_cmdline);

    FILE* out = fopen(LOG_HEADER, "we");
    if (out == NULL) {
        return;
    }
    fprintf(out, "version = Android init 0.8\n");
    fprintf(out, "title = Boot chart for Android (%s)\n", date);
    fprintf(out, "system.uname = %s %s %s %s\n", uts.sysname, uts.release, uts.version, uts.machine);
    fprintf(out, "system.release = %s\n", fingerprint.c_str());
    // TODO: use /proc/cpuinfo "model name" line for x86, "Processor" line for arm.
    fprintf(out, "system.cpu = %s\n", uts.machine);
    fprintf(out, "system.kernel.options = %s\n", kernel_cmdline.c_str());
    fclose(out);
}

static void log_uptime(FILE* log) {
    fprintf(log, "%lld\n", get_uptime_jiffies());
}

static void log_file(FILE* log, const char* procfile) {
    log_uptime(log);

    std::string content;
    if (android::base::ReadFileToString(procfile, &content)) {
        fprintf(log, "%s\n", content.c_str());
    }
}

static void log_procs(FILE* log) {
    log_uptime(log);

    std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir("/proc"), closedir);
    struct dirent* entry;
    while ((entry = readdir(dir.get())) != NULL) {
        // Only match numeric values.
        char* end;
        int pid = strtol(entry->d_name, &end, 10);
        if (end != NULL && end > entry->d_name && *end == 0) {
            // /proc/<pid>/stat only has truncated task names, so get the full
            // name from /proc/<pid>/cmdline.
            std::string cmdline;
            android::base::ReadFileToString(StringPrintf("/proc/%d/cmdline", pid), &cmdline);
            const char* full_name = cmdline.c_str(); // So we stop at the first NUL.

            // Read process stat line.
            std::string stat;
            if (android::base::ReadFileToString(StringPrintf("/proc/%d/stat", pid), &stat)) {
                if (!cmdline.empty()) {
                    // Substitute the process name with its real name.
                    size_t open = stat.find('(');
                    size_t close = stat.find_last_of(')');
                    if (open != std::string::npos && close != std::string::npos) {
//                        LOG(INFO) << stat;
                        stat.replace(open + 1, close - open - 1, full_name);
                    }
                }
                fputs(stat.c_str(), log);
            }
        }
    }

    fputc('\n', log);
}

static int do_bootchart_start() {
    // We don't care about the content, but we do care that /data/bootchart/enabled actually exists.
    std::string start;
    g_bootcharting = android::base::ReadFileToString("/data/bootchart/enabled", &start);

    if (!g_bootcharting) {
        LOG(VERBOSE) << "Not bootcharting";
        return 0;
    }

    LOG(INFO) << "Bootcharting started";

    // Open log files.
    stat_log = fopen(LOG_STAT, "we");
    if (stat_log == NULL) {
        PLOG(ERROR) << "Bootcharting couldn't open " << LOG_STAT;
        return -1;
    }
    procs_log = fopen(LOG_PROCS, "we");
    if (procs_log == NULL) {
        PLOG(ERROR) << "Bootcharting couldn't open " << LOG_PROCS;
        fclose(stat_log);
        return -1;
    }
    disks_log = fopen(LOG_DISK, "we");
    if (disks_log == NULL) {
        PLOG(ERROR) << "Bootcharting couldn't open " << LOG_DISK;
        fclose(stat_log);
        fclose(procs_log);
        return -1;
    }

    // Create kernel process accounting file.
    close(open(LOG_ACCT, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    acct(LOG_ACCT);

    log_header();

    return 0;
}

static void do_bootchart_step() {
    log_file(stat_log,   "/proc/stat");
    log_file(disks_log,  "/proc/diskstats");
    log_procs(procs_log);
}

static int do_bootchart_stop() {
    return 0;
    if (!g_bootcharting) return 0;

    LOG(INFO) << "Bootcharting finished";
    g_bootcharting = false;
    fclose(stat_log);
    fclose(disks_log);
    fclose(procs_log);
    acct(NULL);
    return 0;
}

int do_bootchart(const std::vector<std::string>& args) {
    LOG(INFO) << "do_bootchart " << args[0] << " " << args[1];
    if (args[1] == "start") return do_bootchart_start();
    return do_bootchart_stop();
}

// Called to get time (in ms) used by bootchart.
static long long bootchart_gettime() {
    return 10LL*get_uptime_jiffies();
}

void bootchart_sample(int* timeout) {
    // Do we have any more bootcharting to do?
    if (!g_bootcharting) return;

    long long current_time = bootchart_gettime();
    int elapsed_time = current_time - g_last_bootchart_time;

    if (elapsed_time >= BOOTCHART_POLLING_MS) {
        while (elapsed_time >= BOOTCHART_POLLING_MS) {
            elapsed_time -= BOOTCHART_POLLING_MS;
        }

        g_last_bootchart_time = current_time;
        do_bootchart_step();
    }

    // Schedule another?
    if (g_bootcharting) {
        int remaining_time = BOOTCHART_POLLING_MS - elapsed_time;
        if (*timeout < 0 || *timeout > remaining_time) {
            *timeout = remaining_time;
        }
    }
}
