/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <log/logcat.h>

static std::vector<std::string> __android_logcat_parse(const char* command) {
    std::vector<std::string> args;

    for(const char* delim, *cp = command; cp && *cp; cp = delim) {
        while(isspace(*cp)) ++cp;
        if ((*cp == '\'') || (*cp == '"')) {
            // KISS: Simple quotes. Do not handle the case
            //       of concatenation like "blah"foo'bar'
            char quote = *cp++;
            delim = strchr(cp, quote);
            if (!delim) delim = cp + strlen(cp);
            args.push_back(std::string(cp, delim));
            if (*delim) ++delim;
            continue;
        }
        delim = strpbrk(cp, " \t\f\r\n");
        if (!delim) delim = cp + strlen(cp);
        args.push_back(std::string(cp, delim));
    }
    if (args.size() == 0) return args;
    if ((args[0] != "logcat") && (args[0] != "/system/bin/logcat")) args.clear();
    return args;
}

FILE* android_logcat_popen(android_logcat_context* ctx, const char* command) {
    *ctx = NULL;

    std::vector<std::string> args = __android_logcat_parse(command);
    if (args.size() == 0) return NULL;

    std::vector<const char*> argv;
    for(auto &str : args) {
        argv.push_back(str.c_str());
    }
    argv.push_back(NULL);

    *ctx = create_android_logcat();
    if (!*ctx) return NULL;

    int fd = android_logcat_run_command_thread(*ctx,
                                               argv.size() - 1,
                                               (char* const*)&argv[0]);
    argv.clear();
    args.clear();
    if (fd < 0) {
        android_logcat_destroy(ctx);
        return NULL;
    }

    FILE* retval = fdopen(fd, "reb");
    if (!retval) android_logcat_destroy(ctx);
    return retval;
}

int android_logcat_pclose(android_logcat_context* ctx, FILE* output) {
    if (*ctx) {
        static const useconds_t wait_sample = 20000;
        // Wait two seconds maximum
        for (size_t retry = ((2 * 1000000) + wait_sample - 1) / wait_sample;
             android_logcat_run_command_thread_running(*ctx) && retry;
             --retry) {
            usleep(wait_sample);
        }
    }

    if (output) fclose(output);
    return android_logcat_destroy(ctx);
}

int android_logcat_system(const char* command) {
    std::vector<std::string> args = __android_logcat_parse(command);
    if (args.size() == 0) return -1;

    std::vector<const char*> argv;
    for(auto &str : args) {
        argv.push_back(str.c_str());
    }
    argv.push_back(NULL);

    android_logcat_context ctx = create_android_logcat();
    if (!ctx) return -1;
    int retval = android_logcat_run_command(ctx, -1, -1, argv.size() - 1,
                                            (char* const*)&argv[0]);
    int ret = android_logcat_destroy(&ctx);
    if (!ret) ret = retval;
    return ret;
}
