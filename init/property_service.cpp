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

#include "property_service.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <memory>
#include <queue>
#include <vector>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <bootimg.h>
#include <fs_mgr.h>
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>

#include "init.h"
#include "util.h"

using android::base::Timer;

#define PERSISTENT_PROPERTY_DIR  "/data/property"
#define RECOVERY_MOUNT_POINT "/recovery"

namespace android {
namespace init {

static int persistent_properties_loaded = 0;

static int property_set_fd = -1;

static struct selabel_handle* sehandle_prop;

static void WritePersistentProperty(const std::string& name, const std::string& value);

void property_init() {
    if (__system_property_area_init()) {
        LOG(ERROR) << "Failed to initialize property area";
        exit(1);
    }
}

static bool check_mac_perms(const std::string& name, char* sctx, struct ucred* cr) {

    if (!sctx) {
      return false;
    }

    if (!sehandle_prop) {
      return false;
    }

    char* tctx = nullptr;
    if (selabel_lookup(sehandle_prop, &tctx, name.c_str(), 1) != 0) {
      return false;
    }

    property_audit_data audit_data;

    audit_data.name = name.c_str();
    audit_data.cr = cr;

    bool has_access = (selinux_check_access(sctx, tctx, "property_service", "set", &audit_data) == 0);

    freecon(tctx);
    return has_access;
}

static int check_control_mac_perms(const char *name, char *sctx, struct ucred *cr)
{
    /*
     *  Create a name prefix out of ctl.<service name>
     *  The new prefix allows the use of the existing
     *  property service backend labeling while avoiding
     *  mislabels based on true property prefixes.
     */
    char ctl_name[PROP_VALUE_MAX+4];
    int ret = snprintf(ctl_name, sizeof(ctl_name), "ctl.%s", name);

    if (ret < 0 || (size_t) ret >= sizeof(ctl_name))
        return 0;

    return check_mac_perms(ctl_name, sctx, cr);
}

bool is_legal_property_name(const std::string& name) {
    size_t namelen = name.size();

    if (namelen < 1) return false;
    if (name[0] == '.') return false;
    if (name[namelen - 1] == '.') return false;

    /* Only allow alphanumeric, plus '.', '-', '@', ':', or '_' */
    /* Don't allow ".." to appear in a property name */
    for (size_t i = 0; i < namelen; i++) {
        if (name[i] == '.') {
            // i=0 is guaranteed to never have a dot. See above.
            if (name[i-1] == '.') return false;
            continue;
        }
        if (name[i] == '_' || name[i] == '-' || name[i] == '@' || name[i] == ':') continue;
        if (name[i] >= 'a' && name[i] <= 'z') continue;
        if (name[i] >= 'A' && name[i] <= 'Z') continue;
        if (name[i] >= '0' && name[i] <= '9') continue;
        return false;
    }

    return true;
}

static uint32_t PropertySetImpl(const std::string& name, const std::string& value) {
    size_t valuelen = value.size();

    if (!is_legal_property_name(name)) {
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: bad name";
        return PROP_ERROR_INVALID_NAME;
    }

    if (valuelen >= PROP_VALUE_MAX) {
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                   << "value too long";
        return PROP_ERROR_INVALID_VALUE;
    }

    prop_info* pi = (prop_info*) __system_property_find(name.c_str());
    if (pi != nullptr) {
        // ro.* properties are actually "write-once".
        if (android::base::StartsWith(name, "ro.")) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "property already set";
            return PROP_ERROR_READ_ONLY_PROPERTY;
        }

        __system_property_update(pi, value.c_str(), valuelen);
    } else {
        int rc = __system_property_add(name.c_str(), name.size(), value.c_str(), valuelen);
        if (rc < 0) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "__system_property_add failed";
            return PROP_ERROR_SET_FAILED;
        }
    }

    // Don't write properties to disk until after we have read all default
    // properties to prevent them from being overwritten by default values.
    if (persistent_properties_loaded && android::base::StartsWith(name, "persist.")) {
        WritePersistentProperty(name, value);
    }
    property_changed(name, value);
    return PROP_SUCCESS;
}

typedef int (*PropertyAsyncFunc)(const std::string&, const std::string&);

struct PropertyChildInfo {
    pid_t pid;
    PropertyAsyncFunc func;
    std::string name;
    std::string value;
};

static std::queue<PropertyChildInfo> property_children;

static void PropertyChildLaunch() {
    auto& info = property_children.front();
    pid_t pid = fork();
    if (pid < 0) {
        LOG(ERROR) << "Failed to fork for property_set_async";
        while (!property_children.empty()) {
            property_children.pop();
        }
        return;
    }
    if (pid != 0) {
        info.pid = pid;
    } else {
        if (info.func(info.name, info.value) != 0) {
            LOG(ERROR) << "property_set_async(\"" << info.name << "\", \"" << info.value
                       << "\") failed";
        }
        exit(0);
    }
}

bool PropertyChildReap(pid_t pid) {
    if (property_children.empty()) {
        return false;
    }
    auto& info = property_children.front();
    if (info.pid != pid) {
        return false;
    }
    if (PropertySetImpl(info.name, info.value) != PROP_SUCCESS) {
        LOG(ERROR) << "Failed to set async property " << info.name;
    }
    property_children.pop();
    if (!property_children.empty()) {
        PropertyChildLaunch();
    }
    return true;
}

static uint32_t PropertySetAsync(const std::string& name, const std::string& value,
                                 PropertyAsyncFunc func) {
    if (value.empty()) {
        return PropertySetImpl(name, value);
    }

    PropertyChildInfo info;
    info.func = func;
    info.name = name;
    info.value = value;
    property_children.push(info);
    if (property_children.size() == 1) {
        PropertyChildLaunch();
    }
    return PROP_SUCCESS;
}

static int RestoreconRecursiveAsync(const std::string& name, const std::string& value) {
    return selinux_android_restorecon(value.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE);
}

uint32_t property_set(const std::string& name, const std::string& value) {
    if (name == "selinux.restorecon_recursive") {
        return PropertySetAsync(name, value, RestoreconRecursiveAsync);
    }

    return PropertySetImpl(name, value);
}

class SocketConnection {
 public:
  SocketConnection(int socket, const struct ucred& cred)
      : socket_(socket), cred_(cred) {}

  ~SocketConnection() {
    close(socket_);
  }

  bool RecvUint32(uint32_t* value, uint32_t* timeout_ms) {
    return RecvFully(value, sizeof(*value), timeout_ms);
  }

  bool RecvChars(char* chars, size_t size, uint32_t* timeout_ms) {
    return RecvFully(chars, size, timeout_ms);
  }

  bool RecvString(std::string* value, uint32_t* timeout_ms) {
    uint32_t len = 0;
    if (!RecvUint32(&len, timeout_ms)) {
      return false;
    }

    if (len == 0) {
      *value = "";
      return true;
    }

    // http://b/35166374: don't allow init to make arbitrarily large allocations.
    if (len > 0xffff) {
      LOG(ERROR) << "sys_prop: RecvString asked to read huge string: " << len;
      errno = ENOMEM;
      return false;
    }

    std::vector<char> chars(len);
    if (!RecvChars(&chars[0], len, timeout_ms)) {
      return false;
    }

    *value = std::string(&chars[0], len);
    return true;
  }

  bool SendUint32(uint32_t value) {
    int result = TEMP_FAILURE_RETRY(send(socket_, &value, sizeof(value), 0));
    return result == sizeof(value);
  }

  int socket() {
    return socket_;
  }

  const struct ucred& cred() {
    return cred_;
  }

 private:
  bool PollIn(uint32_t* timeout_ms) {
    struct pollfd ufds[1];
    ufds[0].fd = socket_;
    ufds[0].events = POLLIN;
    ufds[0].revents = 0;
    while (*timeout_ms > 0) {
      Timer timer;
      int nr = poll(ufds, 1, *timeout_ms);
      uint64_t millis = timer.duration().count();
      *timeout_ms = (millis > *timeout_ms) ? 0 : *timeout_ms - millis;

      if (nr > 0) {
        return true;
      }

      if (nr == 0) {
        // Timeout
        break;
      }

      if (nr < 0 && errno != EINTR) {
        PLOG(ERROR) << "sys_prop: error waiting for uid " << cred_.uid << " to send property message";
        return false;
      } else { // errno == EINTR
        // Timer rounds milliseconds down in case of EINTR we want it to be rounded up
        // to avoid slowing init down by causing EINTR with under millisecond timeout.
        if (*timeout_ms > 0) {
          --(*timeout_ms);
        }
      }
    }

    LOG(ERROR) << "sys_prop: timeout waiting for uid " << cred_.uid << " to send property message.";
    return false;
  }

  bool RecvFully(void* data_ptr, size_t size, uint32_t* timeout_ms) {
    size_t bytes_left = size;
    char* data = static_cast<char*>(data_ptr);
    while (*timeout_ms > 0 && bytes_left > 0) {
      if (!PollIn(timeout_ms)) {
        return false;
      }

      int result = TEMP_FAILURE_RETRY(recv(socket_, data, bytes_left, MSG_DONTWAIT));
      if (result <= 0) {
        return false;
      }

      bytes_left -= result;
      data += result;
    }

    return bytes_left == 0;
  }

  int socket_;
  struct ucred cred_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(SocketConnection);
};

static void handle_property_set(SocketConnection& socket,
                                const std::string& name,
                                const std::string& value,
                                bool legacy_protocol) {
  const char* cmd_name = legacy_protocol ? "PROP_MSG_SETPROP" : "PROP_MSG_SETPROP2";
  if (!is_legal_property_name(name)) {
    LOG(ERROR) << "sys_prop(" << cmd_name << "): illegal property name \"" << name << "\"";
    socket.SendUint32(PROP_ERROR_INVALID_NAME);
    return;
  }

  struct ucred cr = socket.cred();
  char* source_ctx = nullptr;
  getpeercon(socket.socket(), &source_ctx);

  if (android::base::StartsWith(name, "ctl.")) {
    if (check_control_mac_perms(value.c_str(), source_ctx, &cr)) {
      handle_control_message(name.c_str() + 4, value.c_str());
      if (!legacy_protocol) {
        socket.SendUint32(PROP_SUCCESS);
      }
    } else {
      LOG(ERROR) << "sys_prop(" << cmd_name << "): Unable to " << (name.c_str() + 4)
                 << " service ctl [" << value << "]"
                 << " uid:" << cr.uid
                 << " gid:" << cr.gid
                 << " pid:" << cr.pid;
      if (!legacy_protocol) {
        socket.SendUint32(PROP_ERROR_HANDLE_CONTROL_MESSAGE);
      }
    }
  } else {
    if (check_mac_perms(name, source_ctx, &cr)) {
      uint32_t result = property_set(name, value);
      if (!legacy_protocol) {
        socket.SendUint32(result);
      }
    } else {
      LOG(ERROR) << "sys_prop(" << cmd_name << "): permission denied uid:" << cr.uid << " name:" << name;
      if (!legacy_protocol) {
        socket.SendUint32(PROP_ERROR_PERMISSION_DENIED);
      }
    }
  }

  freecon(source_ctx);
}

static void handle_property_set_fd() {
    static constexpr uint32_t kDefaultSocketTimeout = 2000; /* ms */

    int s = accept4(property_set_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (s == -1) {
        return;
    }

    struct ucred cr;
    socklen_t cr_size = sizeof(cr);
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cr_size) < 0) {
        close(s);
        PLOG(ERROR) << "sys_prop: unable to get SO_PEERCRED";
        return;
    }

    SocketConnection socket(s, cr);
    uint32_t timeout_ms = kDefaultSocketTimeout;

    uint32_t cmd = 0;
    if (!socket.RecvUint32(&cmd, &timeout_ms)) {
        PLOG(ERROR) << "sys_prop: error while reading command from the socket";
        socket.SendUint32(PROP_ERROR_READ_CMD);
        return;
    }

    switch (cmd) {
    case PROP_MSG_SETPROP: {
        char prop_name[PROP_NAME_MAX];
        char prop_value[PROP_VALUE_MAX];

        if (!socket.RecvChars(prop_name, PROP_NAME_MAX, &timeout_ms) ||
            !socket.RecvChars(prop_value, PROP_VALUE_MAX, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP): error while reading name/value from the socket";
          return;
        }

        prop_name[PROP_NAME_MAX-1] = 0;
        prop_value[PROP_VALUE_MAX-1] = 0;

        handle_property_set(socket, prop_value, prop_value, true);
        break;
      }

    case PROP_MSG_SETPROP2: {
        std::string name;
        std::string value;
        if (!socket.RecvString(&name, &timeout_ms) ||
            !socket.RecvString(&value, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP2): error while reading name/value from the socket";
          socket.SendUint32(PROP_ERROR_READ_DATA);
          return;
        }

        handle_property_set(socket, name, value, false);
        break;
      }

    default:
        LOG(ERROR) << "sys_prop: invalid command " << cmd;
        socket.SendUint32(PROP_ERROR_INVALID_CMD);
        break;
    }
}

static void load_properties_from_file(const char *, const char *);

/*
 * Filter is used to decide which properties to load: NULL loads all keys,
 * "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
 */
static void load_properties(char *data, const char *filter)
{
    char *key, *value, *eol, *sol, *tmp, *fn;
    size_t flen = 0;

    if (filter) {
        flen = strlen(filter);
    }

    sol = data;
    while ((eol = strchr(sol, '\n'))) {
        key = sol;
        *eol++ = 0;
        sol = eol;

        while (isspace(*key)) key++;
        if (*key == '#') continue;

        tmp = eol - 2;
        while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

        if (!strncmp(key, "import ", 7) && flen == 0) {
            fn = key + 7;
            while (isspace(*fn)) fn++;

            key = strchr(fn, ' ');
            if (key) {
                *key++ = 0;
                while (isspace(*key)) key++;
            }

            load_properties_from_file(fn, key);

        } else {
            value = strchr(key, '=');
            if (!value) continue;
            *value++ = 0;

            tmp = value - 2;
            while ((tmp > key) && isspace(*tmp)) *tmp-- = 0;

            while (isspace(*value)) value++;

            if (flen > 0) {
                if (filter[flen - 1] == '*') {
                    if (strncmp(key, filter, flen - 1)) continue;
                } else {
                    if (strcmp(key, filter)) continue;
                }
            }

            property_set(key, value);
        }
    }
}

// Filter is used to decide which properties to load: NULL loads all keys,
// "ro.foo.*" is a prefix match, and "ro.foo.bar" is an exact match.
static void load_properties_from_file(const char* filename, const char* filter) {
    Timer t;
    auto file_contents = ReadFile(filename);
    if (!file_contents) {
        PLOG(WARNING) << "Couldn't load property file '" << filename
                      << "': " << file_contents.error();
        return;
    }
    file_contents->push_back('\n');
    load_properties(file_contents->data(), filter);
    LOG(VERBOSE) << "(Loading properties from " << filename << " took " << t << ".)";
}

static Result<std::vector<std::pair<std::string, std::string>>> LoadLegacyPersistentProperties() {
    std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir(PERSISTENT_PROPERTY_DIR), closedir);
    if (!dir) {
        return ErrnoError() << "Unable to open persistent property directory \""
                            << PERSISTENT_PROPERTY_DIR << "\"";
    }

    std::vector<std::pair<std::string, std::string>> persistent_properties;
    struct dirent* entry;
    while ((entry = readdir(dir.get())) != NULL) {
        if (strncmp("persist.", entry->d_name, strlen("persist."))) {
            continue;
        }
        if (entry->d_type != DT_REG) {
            continue;
        }

        // Open the file and read the property value.
        int fd = openat(dirfd(dir.get()), entry->d_name, O_RDONLY | O_NOFOLLOW);
        if (fd == -1) {
            PLOG(ERROR) << "Unable to open persistent property file \"" << entry->d_name << "\"";
            continue;
        }

        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            PLOG(ERROR) << "fstat on property file \"" << entry->d_name << "\" failed";
            close(fd);
            continue;
        }

        // File must not be accessible to others, be owned by root/root, and
        // not be a hard link to any other file.
        if (((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0) || sb.st_uid != 0 || sb.st_gid != 0 || sb.st_nlink != 1) {
            PLOG(ERROR) << "skipping insecure property file " << entry->d_name
                        << " (uid=" << sb.st_uid << " gid=" << sb.st_gid
                        << " nlink=" << sb.st_nlink << " mode=" << std::oct << sb.st_mode << ")";
            close(fd);
            continue;
        }

        char value[PROP_VALUE_MAX];
        int length = read(fd, value, sizeof(value) - 1);
        if (length >= 0) {
            value[length] = 0;
            persistent_properties.emplace_back(entry->d_name, value);
        } else {
            PLOG(ERROR) << "Unable to read persistent property file " << entry->d_name;
        }
        close(fd);
    }
    return persistent_properties;
}

class PersistentPropertyFileParser {
  public:
    PersistentPropertyFileParser(const std::string& contents) : contents_(contents), position_(0) {}
    Result<std::vector<std::pair<std::string, std::string>>> Parse();

  private:
    Result<std::string> ReadString();
    Result<uint32_t> ReadUint32();

    const std::string& contents_;
    size_t position_;
};

Result<std::vector<std::pair<std::string, std::string>>> PersistentPropertyFileParser::Parse() {
    std::vector<std::pair<std::string, std::string>> result;

    if (auto magic = ReadUint32(); magic) {
        if (*magic != PersistentPropertyFile::kMagic) {
            return Error() << "Magic value '0x" << std::hex << *magic
                           << "' does not match expected value '0x"
                           << PersistentPropertyFile::kMagic << "'";
        }
    } else {
        return Error() << "Could not read magic value: " << magic.error();
    }

    if (auto version = ReadUint32(); version) {
        if (*version != 1) {
            return Error() << "Version '" << *version
                           << "' does not match any compatible version: (1)";
        }
    } else {
        return Error() << "Could not read version: " << version.error();
    }

    auto num_properties = ReadUint32();
    if (!num_properties) {
        return Error() << "Could not read num_properties: " << num_properties.error();
    }

    while (position_ < contents_.size()) {
        auto key = ReadString();
        if (!key) {
            return Error() << "Could not read key: " << key.error();
        }
        if (!android::base::StartsWith(*key, "persist.")) {
            return Error() << "Property '" << *key << "' does not starts with 'persist.'";
        }
        auto value = ReadString();
        if (!value) {
            return Error() << "Could not read value: " << value.error();
        }
        result.emplace_back(*key, *value);
    }

    if (result.size() != *num_properties) {
        return Error() << "Mismatch of number of persistent properties read, " << result.size()
                       << " and number of persistent properties expected, " << *num_properties;
    }

    return result;
}

Result<std::string> PersistentPropertyFileParser::ReadString() {
    auto string_length = ReadUint32();
    if (!string_length) {
        return Error() << "Could not read size for string";
    }

    if (position_ + *string_length > contents_.size()) {
        return Error() << "String size would cause it to overflow the input buffer";
    }
    auto result = std::string(contents_, position_, *string_length);
    position_ += *string_length;
    return result;
}

Result<uint32_t> PersistentPropertyFileParser::ReadUint32() {
    if (position_ + 3 > contents_.size()) {
        return Error() << "Input buffer not large enough to read uint32_t";
    }
    uint32_t result = *reinterpret_cast<const uint32_t*>(&contents_[position_]);
    position_ += sizeof(uint32_t);
    return result;
}

Result<std::vector<std::pair<std::string, std::string>>> PersistentPropertyFile::Load() const {
    if (access(temp_filename_.c_str(), F_OK) == 0) {
        LOG(INFO)
            << "Found temporary property file while attempting to persistent system properties"
               " a previous persistent property write may have failed";
        unlink(temp_filename_.c_str());
    }
    auto file_contents = ReadFile(filename_);
    if (!file_contents) {
        return Error() << "Unable to read persistent property file: " << file_contents.error();
    }
    auto parsed_contents = PersistentPropertyFileParser(*file_contents).Parse();
    if (!parsed_contents) {
        // If the file cannot be parsed, then we don't have any recovery mechanisms, so we delete
        // it to allow for future writes to take place successfully.
        unlink(filename_.c_str());
        return Error() << "Unable to parse persistent property file: " << parsed_contents.error();
    }
    return parsed_contents;
}

std::string PersistentPropertyFile::GenerateFileContents(
    const std::vector<std::pair<std::string, std::string>>& persistent_properties) const {
    std::string result;

    uint32_t magic = kMagic;
    result.append(reinterpret_cast<char*>(&magic), sizeof(uint32_t));

    uint32_t version = 1;
    result.append(reinterpret_cast<char*>(&version), sizeof(uint32_t));

    uint32_t num_properties = persistent_properties.size();
    result.append(reinterpret_cast<char*>(&num_properties), sizeof(uint32_t));

    for (const auto& [key, value] : persistent_properties) {
        uint32_t key_length = key.length();
        result.append(reinterpret_cast<char*>(&key_length), sizeof(uint32_t));
        result.append(key);
        uint32_t value_length = value.length();
        result.append(reinterpret_cast<char*>(&value_length), sizeof(uint32_t));
        result.append(value);
    }
    return result;
}

Result<Success> PersistentPropertyFile::WriteFile(const std::string& file_contents) const {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(
        open(temp_filename_.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0600)));
    if (fd == -1) {
        return ErrnoError() << "Could not open temporary properties file";
    }
    if (!android::base::WriteStringToFd(file_contents, fd)) {
        return ErrnoError() << "Unable to write file contents";
    }
    fsync(fd);
    fd.reset();

    if (rename(temp_filename_.c_str(), filename_.c_str())) {
        int saved_errno = errno;
        unlink(temp_filename_.c_str());
        return Error(saved_errno) << "Unable to rename persistent property file";
    }
    return Success();
}

Result<Success> PersistentPropertyFile::Write(
    const std::vector<std::pair<std::string, std::string>>& persistent_properties) {
    auto file_contents = GenerateFileContents(persistent_properties);
    return WriteFile(file_contents);
}

// Persistent properties are not written often, so we rather not keep any data in memory and read
// then rewrite the persistent property file for each update.
static void WritePersistentProperty(const std::string& name, const std::string& value) {
    PersistentPropertyFile persistent_property_file;
    auto persistent_properties = persistent_property_file.Load();
    if (!persistent_properties) {
        if (persistent_properties.error_errno() == ENOENT) {
            persistent_properties = std::vector<std::pair<std::string, std::string>>();
        } else {
            LOG(ERROR) << "Could not store persistent property: " << persistent_properties.error();
            return;
        }
    }
    auto it = std::find_if(persistent_properties->begin(), persistent_properties->end(),
                           [&name](const auto& entry) { return entry.first == name; });
    if (it != persistent_properties->end()) {
        *it = {name, value};
    } else {
        persistent_properties->emplace_back(name, value);
    }

    if (auto result = persistent_property_file.Write(*persistent_properties); !result) {
        LOG(ERROR) << "Could not store persistent property: " << result.error();
    }
}

void RemoveLegacyPersistentPropertyFiles() {
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(PERSISTENT_PROPERTY_DIR), closedir);
    if (!dir) {
        PLOG(ERROR) << "Unable to open persistent property directory \"" << PERSISTENT_PROPERTY_DIR
                    << "\"";
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != NULL) {
        if (strncmp("persist.", entry->d_name, strlen("persist."))) {
            continue;
        }
        if (entry->d_type != DT_REG) {
            continue;
        }
        unlinkat(dirfd(dir.get()), entry->d_name, 0);
    }
}

static void LoadPersistentProperties() {
    PersistentPropertyFile persistent_property_file;
    auto persistent_properties = persistent_property_file.Load();

    if (!persistent_properties) {
        LOG(ERROR) << "Could not load single persistent property file, trying legacy directory: "
                   << persistent_properties.error();
        persistent_properties = LoadLegacyPersistentProperties();
        if (!persistent_properties) {
            LOG(ERROR) << "Unable to load legacy persistent properties: "
                       << persistent_properties.error();
            return;
        }
        if (auto result = persistent_property_file.Write(*persistent_properties); result) {
            RemoveLegacyPersistentPropertyFiles();
        } else {
            LOG(ERROR) << "Unable to write single persistent property file: " << result.error();
            // Fall through so that we still set the properties that we've read.
        }
    }

    for (const auto& [key, value] : *persistent_properties) {
        property_set(key, value);
    }
    persistent_properties_loaded = 1;
}

// persist.sys.usb.config values can't be combined on build-time when property
// files are split into each partition.
// So we need to apply the same rule of build/make/tools/post_process_props.py
// on runtime.
static void update_sys_usb_config() {
    bool is_debuggable = android::base::GetBoolProperty("ro.debuggable", false);
    std::string config = android::base::GetProperty("persist.sys.usb.config", "");
    if (config.empty()) {
        property_set("persist.sys.usb.config", is_debuggable ? "adb" : "none");
    } else if (is_debuggable && config.find("adb") == std::string::npos &&
               config.length() + 4 < PROP_VALUE_MAX) {
        config.append(",adb");
        property_set("persist.sys.usb.config", config);
    }
}

void property_load_boot_defaults() {
    load_properties_from_file("/default.prop", NULL);
    load_properties_from_file("/odm/default.prop", NULL);
    load_properties_from_file("/vendor/default.prop", NULL);

    update_sys_usb_config();
}

static void load_override_properties() {
    if (ALLOW_LOCAL_PROP_OVERRIDE) {
        load_properties_from_file("/data/local.prop", NULL);
    }
}

/* When booting an encrypted system, /data is not mounted when the
 * property service is started, so any properties stored there are
 * not loaded.  Vold triggers init to load these properties once it
 * has mounted /data.
 */
void load_persist_props(void) {
    // Devices with FDE have load_persist_props called twice; the first time when the temporary
    // /data partition is mounted and then again once /data is truly mounted.  We do not want to
    // read persistent properties from the temporary /data partition or mark persistent properties
    // as having been loaded during the first call, so we return in that case.
    std::string crypto_state = android::base::GetProperty("ro.crypto.state", "");
    std::string crypto_type = android::base::GetProperty("ro.crypto.type", "");
    if (crypto_state == "encrypted" && crypto_type == "block") {
        static size_t num_calls = 0;
        if (++num_calls == 1) return;
    }

    load_override_properties();
    /* Read persistent properties after all default values have been loaded. */
    LoadPersistentProperties();
    property_set("ro.persistent_properties.ready", "true");
}

void load_recovery_id_prop() {
    std::unique_ptr<fstab, decltype(&fs_mgr_free_fstab)> fstab(fs_mgr_read_fstab_default(),
                                                               fs_mgr_free_fstab);
    if (!fstab) {
        PLOG(ERROR) << "unable to read default fstab";
        return;
    }

    fstab_rec* rec = fs_mgr_get_entry_for_mount_point(fstab.get(), RECOVERY_MOUNT_POINT);
    if (rec == NULL) {
        LOG(ERROR) << "/recovery not specified in fstab";
        return;
    }

    int fd = open(rec->blk_device, O_RDONLY);
    if (fd == -1) {
        PLOG(ERROR) << "error opening block device " << rec->blk_device;
        return;
    }

    boot_img_hdr hdr;
    if (android::base::ReadFully(fd, &hdr, sizeof(hdr))) {
        std::string hex = bytes_to_hex(reinterpret_cast<uint8_t*>(hdr.id), sizeof(hdr.id));
        property_set("ro.recovery_id", hex);
    } else {
        PLOG(ERROR) << "error reading /recovery";
    }

    close(fd);
}

void load_system_props() {
    load_properties_from_file("/system/build.prop", NULL);
    load_properties_from_file("/odm/build.prop", NULL);
    load_properties_from_file("/vendor/build.prop", NULL);
    load_properties_from_file("/factory/factory.prop", "ro.*");
    load_recovery_id_prop();
}

static int SelinuxAuditCallback(void* data, security_class_t /*cls*/, char* buf, size_t len) {
    property_audit_data* d = reinterpret_cast<property_audit_data*>(data);

    if (!d || !d->name || !d->cr) {
        LOG(ERROR) << "AuditCallback invoked with null data arguments!";
        return 0;
    }

    snprintf(buf, len, "property=%s pid=%d uid=%d gid=%d", d->name, d->cr->pid, d->cr->uid,
             d->cr->gid);
    return 0;
}

void start_property_service() {
    sehandle_prop = selinux_android_prop_context_handle();

    selinux_callback cb;
    cb.func_audit = SelinuxAuditCallback;
    selinux_set_callback(SELINUX_CB_AUDIT, cb);

    property_set("ro.property_service.version", "2");

    property_set_fd = CreateSocket(PROP_SERVICE_NAME, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                   false, 0666, 0, 0, nullptr);
    if (property_set_fd == -1) {
        PLOG(ERROR) << "start_property_service socket creation failed";
        exit(1);
    }

    listen(property_set_fd, 8);

    register_epoll_handler(property_set_fd, handle_property_set_fd);
}

}  // namespace init
}  // namespace android
