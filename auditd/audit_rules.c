/*
 * Copyright 2013, Quark Security Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Written by Joshua Brindle <brindle@quarksecurity.com>
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

#define LOG_TAG "audit_rules"
#include <cutils/log.h>

#include "libaudit.h"

#define LINE_LEN 255

static int audit_rules_parse_and_add(int audit_fd, char *line)
{
    char *argv[AUDIT_MAX_FIELDS];
    int argc;
    int rc = 0, added_rule = 0;
    char p;
    int o;
    size_t len;
    struct audit_rule_data *rule;

    /* Strip crlf */
    line[strlen(line) -1] = '\0';

    argv[0] = "auditd";

    for (argc=1; argc < AUDIT_MAX_FIELDS - 1; argc++) {
        argv[argc] = strsep(&line, " \n\r");
        if (argv[argc] == NULL)
            break;
    }

    optind = 0;

    while ((o = getopt(argc, argv, "w:e:p:")) != -1) {
        switch(o) {
        case 'w':
            if (audit_add_dir(&rule, optarg)) {
                SLOGE("Error adding rule");
                return -1;
            }
            added_rule = 1;
            break;
        case 'e':
            if (audit_set_enabled(audit_fd, strtoul(optarg, NULL, 10)))
                return -1;
            break;
        case 'p':
            if (added_rule == 0) {
                SLOGE("Specify rule type before permissions");
                return -1;
            }
            uint32_t perms = 0;
            for (len=0; len < strlen(optarg); len++) {
                 switch(optarg[len]) {
                 case 'w':
                     perms |= AUDIT_PERM_WRITE;
                     break;
                 case 'e':
                     perms |= AUDIT_PERM_EXEC;
                     break;
                 case 'r':
                     perms |= AUDIT_PERM_READ;
                     break;
                 case 'a':
                     perms |= AUDIT_PERM_ATTR;
                     break;
                 default:
                     SLOGE("Unknown permission %c", optarg[len]);
                     break;
                 }
            }
            if (audit_update_watch_perms(rule, perms)) {
                SLOGE("Could not set perms on rule");
                return -1;
            }
            break;
        case '?':
            SLOGE("Unsupported option: %c", optopt);
            break;
        }
    }

    if (added_rule) {
        rc = audit_send(audit_fd, AUDIT_ADD_RULE, rule, sizeof(*rule) + rule->buflen);
        free(rule);
    }

    return rc;
}

int audit_rules_read_and_add(int audit_fd, const char *rulefile)
{
    int rc;
    struct stat s;
    char line[LINE_LEN];
    FILE *rules;

    rc = stat(rulefile, &s);
    if (rc < 0) {
        SLOGE("Could not read audit rules %s: %s", rulefile, strerror(errno));
        return 0;
    }

    rules = fopen(rulefile, "r");
    if (rules == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), rules)) {
        SLOGE(line);
        if (line[0] != '-')
            continue;
        if (audit_rules_parse_and_add(audit_fd, line) < 0) {
           SLOGE("Could not read audit rules");
           return -1;
        }
    }

    fclose(rules);
    return 0;
}
