/*
 * Copyright (C) 2007 The Android Open Source Project
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

/* This file is used to define the properties of the filesystem
** images generated by build tools (mkbootfs and mkyaffs2image) and
** by the device side of adb.
*/

#ifndef _ANDROID_FILESYSTEM_CONFIG_H_
#define _ANDROID_FILESYSTEM_CONFIG_H_

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* This is the master Users and Groups config for the platform.
** DO NOT EVER RENUMBER.
*/

#define AID_ROOT             0  /* traditional unix root user */

#define AID_SYSTEM        1000  /* system server */

#define AID_RADIO         1001  /* telephony subsystem, RIL */
#define AID_BLUETOOTH     1002  /* bluetooth subsystem */
#define AID_GRAPHICS      1003  /* graphics devices */
#define AID_INPUT         1004  /* input devices */
#define AID_AUDIO         1005  /* audio devices */
#define AID_CAMERA        1006  /* camera devices */
#define AID_LOG           1007  /* log devices */
#define AID_COMPASS       1008  /* compass device */
#define AID_MOUNT         1009  /* mountd socket */
#define AID_WIFI          1010  /* wifi subsystem */
#define AID_ADB           1011  /* android debug bridge (adbd) */
#define AID_INSTALL       1012  /* group for installing packages */
#define AID_MEDIA         1013  /* mediaserver process */
#define AID_DHCP          1014  /* dhcp client */

#define AID_SHELL         2000  /* adb and debug shell user */
#define AID_CACHE         2001  /* cache access */
#define AID_DIAG          2002  /* access to diagnostic resources */

/* The 3000 series are intended for use as supplemental group id's only.
 * They indicate special Android capabilities that the kernel is aware of. */
#define AID_NET_BT_ADMIN  3001  /* bluetooth: create any socket */
#define AID_NET_BT        3002  /* bluetooth: create sco, rfcomm or l2cap sockets */
#define AID_INET          3003  /* can create AF_INET and AF_INET6 sockets */
#define AID_NET_RAW       3004  /* can create raw INET sockets */

#define AID_MISC          9998  /* access to misc storage */
#define AID_NOBODY        9999

#define AID_APP          10000 /* first app user */

#if !defined(EXCLUDE_FS_CONFIG_STRUCTURES)
struct android_id_info {
    const char *name;
    unsigned aid;
};

static struct android_id_info android_ids[] = {
    { "root",      AID_ROOT, },
    { "system",    AID_SYSTEM, },
    { "radio",     AID_RADIO, },
    { "bluetooth", AID_BLUETOOTH, },
    { "graphics",  AID_GRAPHICS, },
    { "input",     AID_INPUT, },
    { "audio",     AID_AUDIO, },
    { "camera",    AID_CAMERA, },
    { "log",       AID_LOG, },
    { "compass",   AID_COMPASS, },
    { "mount",     AID_MOUNT, },
    { "wifi",      AID_WIFI, },
    { "dhcp",      AID_DHCP, },
    { "adb",       AID_ADB, },
    { "install",   AID_INSTALL, },
    { "media",     AID_MEDIA, },
    { "shell",     AID_SHELL, },
    { "cache",     AID_CACHE, },
    { "diag",      AID_DIAG, },
    { "net_bt_admin", AID_NET_BT_ADMIN, },
    { "net_bt",    AID_NET_BT, },
    { "inet",      AID_INET, }, 
    { "net_raw",   AID_NET_RAW, },
    { "misc",      AID_MISC, },
    { "nobody",    AID_NOBODY, },
};

#define android_id_count \
    (sizeof(android_ids) / sizeof(android_ids[0]))
    
struct fs_path_config {
    unsigned mode;
    unsigned uid;
    unsigned gid;
    const char *prefix;
};

/* Rules for directories.
** These rules are applied based on "first match", so they
** should start with the most specific path and work their
** way up to the root.
*/

static struct fs_path_config android_dirs[] = {
    { 00770, AID_SYSTEM, AID_CACHE,  "cache" },
    { 00771, AID_SYSTEM, AID_SYSTEM, "data/app" },
    { 00771, AID_SYSTEM, AID_SYSTEM, "data/app-private" },
    { 00771, AID_SYSTEM, AID_SYSTEM, "data/dalvik-cache" },
    { 00771, AID_SYSTEM, AID_SYSTEM, "data/data" },
    { 00771, AID_SHELL,  AID_SHELL,  "data/local/tmp" },
    { 00771, AID_SHELL,  AID_SHELL,  "data/local" },
    { 01771, AID_SYSTEM, AID_MISC,   "data/misc" },
    { 00770, AID_DHCP,   AID_DHCP,   "data/misc/dhcp" },
    { 00771, AID_SYSTEM, AID_SYSTEM, "data" },
    { 00750, AID_ROOT,   AID_SHELL,  "sbin" },
    { 00755, AID_ROOT,   AID_SHELL,  "system/bin" },
    { 00755, AID_ROOT,   AID_SHELL,  "system/xbin" },
    { 00777, AID_ROOT,   AID_ROOT,   "system/etc/ppp" }, /* REMOVE */
    { 00777, AID_ROOT,   AID_ROOT,   "sdcard" },
    { 00755, AID_ROOT,   AID_ROOT,   0 },
};

/* Rules for files.
** These rules are applied based on "first match", so they
** should start with the most specific path and work their
** way up to the root. Prefixes ending in * denotes wildcard
** and will allow partial matches.
*/
static struct fs_path_config android_files[] = {
    { 00555, AID_ROOT,      AID_ROOT,      "system/etc/ppp/ip-up" },
    { 00555, AID_ROOT,      AID_ROOT,      "system/etc/ppp/ip-down" },
    { 00440, AID_ROOT,      AID_SHELL,     "system/etc/init.goldfish.rc" },
    { 00550, AID_ROOT,      AID_SHELL,     "system/etc/init.goldfish.sh" },
    { 00440, AID_ROOT,      AID_SHELL,     "system/etc/init.trout.rc" },
    { 00550, AID_ROOT,      AID_SHELL,     "system/etc/init.ril" },
    { 00550, AID_ROOT,      AID_SHELL,     "system/etc/init.testmenu" },
    { 00550, AID_ROOT,      AID_SHELL,     "system/etc/init.gprs-pppd" },
    { 00550, AID_DHCP,      AID_SHELL,     "system/etc/dhcpcd/dhcpcd-run-hooks" },
    { 00440, AID_BLUETOOTH, AID_BLUETOOTH, "system/etc/dbus.conf" },
    { 00440, AID_BLUETOOTH, AID_BLUETOOTH, "system/etc/bluez/hcid.conf" },
    { 00440, AID_BLUETOOTH, AID_BLUETOOTH, "system/etc/bluez/input.conf" },
    { 00440, AID_BLUETOOTH, AID_BLUETOOTH, "system/etc/bluez/audio.conf" },
    { 00440, AID_RADIO,     AID_AUDIO,     "/system/etc/AudioPara4.csv" },
    { 00644, AID_SYSTEM,    AID_SYSTEM,    "data/app/*" },
    { 00644, AID_SYSTEM,    AID_SYSTEM,    "data/app-private/*" },
    { 00644, AID_APP,       AID_APP,       "data/data/*" },
        /* the following two files are INTENTIONALLY set-gid and not set-uid.
         * Do not change. */
    { 02755, AID_ROOT,      AID_NET_RAW,   "system/bin/ping" },
    { 02755, AID_ROOT,      AID_INET,      "system/bin/netcfg" },
    	/* the following four files are INTENTIONALLY set-uid, but they
	 * are NOT included on user builds. */
    { 06755, AID_ROOT,      AID_ROOT,      "system/xbin/su" },
    { 06755, AID_ROOT,      AID_ROOT,      "system/xbin/librank" },
    { 06755, AID_ROOT,      AID_ROOT,      "system/xbin/procrank" },
    { 06755, AID_ROOT,      AID_ROOT,      "system/xbin/procmem" },
    { 00755, AID_ROOT,      AID_SHELL,     "system/bin/*" },
    { 00755, AID_ROOT,      AID_SHELL,     "system/xbin/*" },
    { 00750, AID_ROOT,      AID_SHELL,     "sbin/*" },
    { 00755, AID_ROOT,      AID_ROOT,      "bin/*" },
    { 00750, AID_ROOT,      AID_SHELL,     "init*" },
    { 00644, AID_ROOT,      AID_ROOT,       0 },
};

static inline void fs_config(const char *path, int dir,
                             unsigned *uid, unsigned *gid, unsigned *mode)
{
    struct fs_path_config *pc;
    int plen;
    
    pc = dir ? android_dirs : android_files;
    plen = strlen(path);
    for(; pc->prefix; pc++){
        int len = strlen(pc->prefix);
        if (dir) {
            if(plen < len) continue;
            if(!strncmp(pc->prefix, path, len)) break;
            continue;
        }
        /* If name ends in * then allow partial matches. */
        if (pc->prefix[len -1] == '*') {
            if(!strncmp(pc->prefix, path, len - 1)) break;
        } else if (plen == len){
            if(!strncmp(pc->prefix, path, len)) break;
        }
    }
    *uid = pc->uid;
    *gid = pc->gid;
    *mode = (*mode & (~07777)) | pc->mode;
    
#if 0
    fprintf(stderr,"< '%s' '%s' %d %d %o >\n", 
            path, pc->prefix ? pc->prefix : "", *uid, *gid, *mode);
#endif
}
#endif
#endif
