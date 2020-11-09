/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <libdebuggerd/tombstone.h>

#include <inttypes.h>

#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>

#include "tombstone.pb.h"

using android::base::StringAppendF;
using android::base::StringPrintf;

#define CB(...) callback(StringPrintf(__VA_ARGS__), true)
#define CBS(...) callback(StringPrintf(__VA_ARGS__), false)
#define CB_LOG(log, ...) callback(StringPrintf(__VA_ARGS__), log)
using CallbackType = std::function<void(const std::string& line, bool should_log)>;

static const char* abi_string(const Tombstone& tombstone) {
  switch (tombstone.arch()) {
    case Architecture::ARM32:
      return "arm";
    case Architecture::ARM64:
      return "arm64";
    case Architecture::X86:
      return "x86";
    case Architecture::X86_64:
      return "x86_64";
    default:
      return "<unknown>";
  }
}

static int pointer_width(const Tombstone& tombstone) {
  switch (tombstone.arch()) {
    case Architecture::ARM32:
      return 4;
    case Architecture::ARM64:
      return 8;
    case Architecture::X86:
      return 4;
    case Architecture::X86_64:
      return 8;
    default:
      return 8;
  }
}

static void print_thread_header(CallbackType callback, const Tombstone& tombstone,
                                const Thread& thread, bool should_log) {
  CB_LOG(should_log, "pid: %d, tid: %d, name: %s  >>> %s <<<", tombstone.pid(), thread.id(),
         thread.name().c_str(), tombstone.process_name().c_str());
  CB_LOG(should_log, "uid: %d", tombstone.uid());
}

static void print_register_row(CallbackType callback, int word_size,
                               std::vector<std::pair<std::string, uint64_t>> row, bool should_log) {
  std::string output = "  ";
  for (const auto& [name, value] : row) {
    output += android::base::StringPrintf("  %-3s %0*" PRIx64, name.c_str(), 2 * word_size,
                                          static_cast<uint64_t>(value));
  }
  callback(output, should_log);
}

static void print_thread_registers(CallbackType callback, const Tombstone& tombstone,
                                   const Thread& thread, bool should_log) {
  static constexpr size_t column_count = 4;
  std::vector<std::pair<std::string, uint64_t>> current_row;
  std::vector<std::pair<std::string, uint64_t>> special_row;
  std::unordered_set<std::string> special_registers;

  int word_size = pointer_width(tombstone);

  switch (tombstone.arch()) {
    case Architecture::ARM32:
      special_registers = {"ip", "lr", "sp", "pc", "pst"};
      break;

    case Architecture::ARM64:
      special_registers = {"ip", "lr", "sp", "pc", "pst"};
      break;

    case Architecture::X86:
      special_registers = {"ebp", "esp", "eip"};
      break;

    case Architecture::X86_64:
      special_registers = {"rbp", "rsp", "rip"};
      break;

    default:
      LOG(FATAL) << "unknown architecture";
  }

  for (const auto& reg : thread.registers()) {
    auto row = &current_row;
    if (special_registers.count(reg.name()) == 1) {
      row = &special_row;
    }

    row->emplace_back(reg.name(), reg.u64());
    if (current_row.size() == column_count) {
      print_register_row(callback, word_size, current_row, should_log);
      current_row.clear();
    }
  }

  if (!current_row.empty()) {
    print_register_row(callback, word_size, current_row, should_log);
  }

  print_register_row(callback, word_size, special_row, should_log);
}

static void print_thread_backtrace(CallbackType callback, const Tombstone& tombstone,
                                   const Thread& thread, bool should_log) {
  CB_LOG(false, "");
  CB_LOG(should_log, "backtrace:");
  int index = 0;
  for (const auto& frame : thread.current_backtrace()) {
    std::string function;

    if (!frame.function_name().empty()) {
      function =
          StringPrintf(" (%s+%" PRId64 ")", frame.function_name().c_str(), frame.function_offset());
    }

    std::string build_id;
    if (!frame.build_id().empty()) {
      build_id = StringPrintf(" (BuildId: %s)", frame.build_id().c_str());
    }

    CB_LOG(should_log, "      #%02d pc %0*" PRIx64 "  %s%s%s", index++,
           pointer_width(tombstone) * 2, frame.rel_pc(), frame.file_name().c_str(),
           function.c_str(), build_id.c_str());
  }
}

static void print_thread_memory_dump(CallbackType callback, const Tombstone& tombstone,
                                     const Thread& thread) {
  static constexpr size_t bytes_per_line = 16;
  int word_size = pointer_width(tombstone);
  for (const auto& mem : thread.memory_dump()) {
    CBS("");
    CBS("memory near %s (%s):", mem.register_name().c_str(), mem.mapping_name().c_str());
    uint64_t addr = mem.begin_address();
    for (size_t offset = 0; offset < mem.memory().size(); offset += bytes_per_line) {
      std::string line = StringPrintf("    %0*" PRIx64, word_size * 2, addr + offset);

      size_t bytes = std::min(bytes_per_line, mem.memory().size() - offset);
      for (size_t i = 0; i < bytes; i += word_size) {
        uint64_t word = 0;

        // Assumes little-endian, but what doesn't?
        memcpy(&word, mem.memory().data() + offset + i, word_size);

        StringAppendF(&line, " %0*" PRIx64, word_size * 2, word);
      }

      char ascii[bytes_per_line + 1];

      memset(ascii, '.', sizeof(ascii));
      ascii[bytes_per_line] = '\0';

      for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte = mem.memory()[offset + i];
        if (byte >= 0x20 && byte < 0x7f) {
          ascii[i] = byte;
        }
      }

      CBS("%s  %s", line.c_str(), ascii);
    }
  }
}

static void print_thread(CallbackType callback, const Tombstone& tombstone, const Thread& thread) {
  print_thread_header(callback, tombstone, thread, false);
  print_thread_registers(callback, tombstone, thread, false);
  print_thread_backtrace(callback, tombstone, thread, false);
  print_thread_memory_dump(callback, tombstone, thread);
}

static void print_main_thread(CallbackType callback, const Tombstone& tombstone,
                              const Thread& thread) {
  print_thread_header(callback, tombstone, thread, true);

  const Signal& signal_info = tombstone.signal_info();
  std::string sender_desc;

  if (signal_info.has_sender()) {
    sender_desc =
        StringPrintf(" from pid %d, uid %d", signal_info.sender_pid(), signal_info.sender_uid());
  }

  if (!tombstone.has_signal_info()) {
    LOG(ERROR) << "signal info missing in tombstone";
    CB("signal information missing");
  } else {
    std::string fault_addr_desc;
    if (signal_info.has_fault_address()) {
      fault_addr_desc = StringPrintf("0x%" PRIx64, signal_info.fault_address());
    } else {
      fault_addr_desc = "--------";
    }

    CB("signal %d (%s), code %d (%s%s), fault addr %s", signal_info.number(),
       signal_info.name().c_str(), signal_info.code(), signal_info.code_name().c_str(),
       sender_desc.c_str(), fault_addr_desc.c_str());
  }

  if (tombstone.has_cause()) {
    const Cause& cause = tombstone.cause();
    CB("Cause: %s", cause.human_readable().c_str());
  }

  if (!tombstone.abort_message().empty()) {
    CB("Abort message: '%s'", tombstone.abort_message().c_str());
  }

  print_thread_registers(callback, tombstone, thread, true);
  print_thread_backtrace(callback, tombstone, thread, true);
  print_thread_memory_dump(callback, tombstone, thread);

  CBS("");
  CBS("memory map (%d %s):", tombstone.memory_mappings().size(),
      tombstone.memory_mappings().size() == 1 ? "entry" : "entries");
  int word_size = pointer_width(tombstone);
  const auto format_pointer = [word_size](uint64_t ptr) -> std::string {
    if (word_size == 8) {
      uint64_t top = ptr >> 32;
      uint64_t bottom = ptr & 0xFFFFFFFF;
      return StringPrintf("%08" PRIx64 "'%08" PRIx64, top, bottom);
    }

    return StringPrintf("%0*" PRIx64, word_size * 2, ptr);
  };

  for (const auto& map : tombstone.memory_mappings()) {
    std::string line = "    ";
    StringAppendF(&line, "%s-%s", format_pointer(map.begin_address()).c_str(),
                  format_pointer(map.end_address() - 1).c_str());
    StringAppendF(&line, " %s%s%s", map.read() ? "r" : "-", map.write() ? "w" : "-",
                  map.execute() ? "x" : "-");
    StringAppendF(&line, "  %8" PRIx64 "  %8" PRIx64, map.offset(),
                  map.end_address() - map.begin_address());

    if (!map.mapping_name().empty()) {
      StringAppendF(&line, "  %s", map.mapping_name().c_str());

      if (!map.build_id().empty()) {
        StringAppendF(&line, " (BuildId: %s)", map.build_id().c_str());
      }

      if (map.load_bias() != 0) {
        StringAppendF(&line, " (load bias 0x%" PRIx64 ")", map.load_bias());
      }
    }

    CBS("%s", line.c_str());
  }
}

void print_logs(CallbackType callback, const Tombstone& tombstone, int tail) {
  for (const auto& buffer : tombstone.log_buffers()) {
    if (tail) {
      CBS("--------- tail end of log %s", buffer.name().c_str());
    } else {
      CBS("--------- log %s", buffer.name().c_str());
    }

    int begin = 0;
    if (tail != 0) {
      begin = std::max(0, buffer.logs().size() - tail);
    }

    for (int i = begin; i < buffer.logs().size(); ++i) {
      const LogMessage& msg = buffer.logs(i);

      static const char* kPrioChars = "!.VDIWEFS";
      char priority = (msg.priority() < strlen(kPrioChars) ? kPrioChars[msg.priority()] : '?');
      CBS("%s %5u %5u %c %-8s: %s", msg.timestamp().c_str(), msg.pid(), msg.tid(), priority,
          msg.tag().c_str(), msg.message().c_str());
    }
  }
}

bool proto_tombstone_to_text(const Tombstone& tombstone, CallbackType callback) {
  CB("*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***");
  CB("Build fingerprint: '%s'", tombstone.build_fingerprint().c_str());
  CB("Revision: '%s'", tombstone.revision().c_str());
  CB("ABI: '%s'", abi_string(tombstone));
  CB("Timestamp: %s", tombstone.timestamp().c_str());

  // Process header
  const auto& threads = tombstone.threads();
  auto main_thread_it = threads.find(tombstone.tid());
  if (main_thread_it == threads.end()) {
    LOG(ERROR) << "failed to find entry for main thread in tombstone";
    return false;
  }

  const auto& main_thread = main_thread_it->second;

  print_main_thread(callback, tombstone, main_thread);

  print_logs(callback, tombstone, 50);

  // protobuf's map is unordered, so sort the keys first.
  std::set<int> thread_ids;
  for (const auto& [tid, _] : threads) {
    if (tid != tombstone.tid()) {
      thread_ids.insert(tid);
    }
  }

  for (const auto& tid : thread_ids) {
    CB_LOG(false, "--- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---");
    print_thread(callback, tombstone, threads.find(tid)->second);
  }

  if (tombstone.open_fds().size() > 0) {
    CB_LOG(false, "");
    CB_LOG(false, "open files:");
    for (const auto& fd : tombstone.open_fds()) {
      std::optional<std::string> owner;
      if (!fd.owner().empty()) {
        owner = StringPrintf("owned by %s 0x%" PRIx64, fd.owner().c_str(), fd.tag());
      }

      CB_LOG(false, "    fd %d: %s (%s)", fd.fd(), fd.path().c_str(),
             owner ? owner->c_str() : "unowned");
    }
  }

  print_logs(callback, tombstone, 0);

  return true;
}
