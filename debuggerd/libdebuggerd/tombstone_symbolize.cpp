/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "libdebuggerd/tombstone_symbolize.h"
#include "tombstone.pb.h"

#include "android-base/stringprintf.h"

#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

using android::base::StringPrintf;

class Symbolizer {
  int in_fd = -1, out_fd = -1;

  std::string read_response();

 public:
  bool start();

  ~Symbolizer() {
    if (in_fd != -1) {
      close(in_fd);
    }
    if (out_fd != -1) {
      close(out_fd);
    }
  }

  struct Frame {
    std::string function_name, file;
    uint64_t line, column;
  };

  std::vector<Frame> symbolize_code(std::string path, uint64_t rel_pc);
};

bool Symbolizer::start() {
  int in_pipe[2], out_pipe[2];

  if (pipe(in_pipe) != 0) {
    return false;
  }
  if (pipe(out_pipe) != 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    return false;
  }

  int pid = fork();
  if (pid == 0) {
    close(in_pipe[0]);
    close(out_pipe[1]);

    dup2(out_pipe[0], STDIN_FILENO);
    dup2(in_pipe[1], STDOUT_FILENO);

    execlp("llvm-symbolizer", "llvm-symbolizer", NULL);

    fprintf(stderr, "unable to start llvm-symbolizer: %s\n", strerror(errno));
    _exit(1);
  } else {
    close(in_pipe[1]);
    close(out_pipe[0]);

    if (write(out_pipe[1], "\n", 1) != 1) {
      close(in_pipe[0]);
      close(out_pipe[1]);
      return false;
    }

    char buf;
    if (read(in_pipe[0], &buf, 1) != 1 || buf != '\n') {
      close(in_pipe[0]);
      close(out_pipe[1]);
      return false;
    }

    in_fd = in_pipe[0];
    out_fd = out_pipe[1];
    return true;
  }
}

std::string Symbolizer::read_response() {
  std::string resp;

  while (resp.size() < 2 || resp[resp.size() - 2] != '\n' || resp[resp.size() - 1] != '\n') {
    char buf[4096];
    ssize_t size = read(in_fd, buf, 4096);
    if (size <= 0) {
      return "";
    }
    resp.append(buf, size);
  }

  return resp;
}

std::vector<Symbolizer::Frame> Symbolizer::symbolize_code(std::string path, uint64_t rel_pc) {
  std::string request = StringPrintf("CODE %s 0x%" PRIx64 "\n", path.c_str(), rel_pc);
  write(out_fd, request.c_str(), request.size());

  std::string response = read_response();
  if (response.empty()) {
    return {};
  }

  std::vector<Symbolizer::Frame> frames;

  size_t frame_start = 0;
  while (frame_start < response.size() - 1) {
    Symbolizer::Frame frame;

    size_t second_line_start = response.find('\n', frame_start) + 1;
    if (second_line_start == std::string::npos + 1) {
      return {};
    }

    size_t third_line_start = response.find('\n', second_line_start) + 1;
    if (third_line_start == std::string::npos + 1) {
      return {};
    }

    frame.function_name = response.substr(frame_start, second_line_start - frame_start - 1);

    size_t column_number_start = response.rfind(':', third_line_start);
    if (column_number_start == std::string::npos) {
      return {};
    }

    size_t line_number_start = response.rfind(':', column_number_start - 1);
    if (line_number_start == std::string::npos) {
      return {};
    }

    frame.file = response.substr(second_line_start, line_number_start - second_line_start);

    errno = 0;
    frame.line = strtoull(response.c_str() + line_number_start + 1, 0, 10);
    frame.column = strtoull(response.c_str() + column_number_start + 1, 0, 10);
    if (errno != 0) {
      return {};
    }

    frames.push_back(frame);

    frame_start = third_line_start;
  }

  if (frames.size() == 1 && frames[0].file == "??") {
    return {};
  }

  return frames;
}

static std::string find_symbols_path(BacktraceFrame& frame) {
  // If this is a lunched environment, try using the local symbols tree first.
  if (const char *product_out = getenv("ANDROID_PRODUCT_OUT")) {
    // The symbols directory doesn't always match the device contents. Handle some special cases.
    static struct {
      std::string dev, syms;
    } alternative_paths[] = {
        // First just check the default location.
        {"", ""},
        // When using the ART debug APEX.
        {"/apex/com.android.art/", "/apex/com.android.art.debug/"},
    };

    for (auto& alt : alternative_paths) {
      if (frame.file_name().substr(0, alt.dev.size()) == alt.dev) {
        std::string new_path = alt.syms + frame.file_name().substr(alt.dev.size());
        std::string symbols_path = StringPrintf("%s/symbols%s", product_out, new_path.c_str());
        if (access(symbols_path.c_str(), O_RDONLY) == 0) {
          return "\"" + symbols_path + "\"";
        }
      }
    }
  }

  // Otherwise, pass a build id, which will cause recent versions of llvm-symbolizer to query a
  // user-provided debuginfod symbol server.
  return "BUILDID:" + frame.build_id();
}

static void symbolize_backtrace_frame(BacktraceFrame& frame, Symbolizer& sym) {
  std::string symbols_path = find_symbols_path(frame);
  if (symbols_path.empty()) {
    return;
  }

  for (Symbolizer::Frame f : sym.symbolize_code(symbols_path, frame.rel_pc())) {
    SymbolizedFrame* sym_frame = frame.add_symbolized_frames();
    sym_frame->set_function_name(f.function_name);
    sym_frame->set_file(f.file);
    sym_frame->set_line(f.line);
    sym_frame->set_column(f.column);
  }
}

void tombstone_symbolize(Tombstone& tombstone) {
  Symbolizer sym;
  sym.start();

  for (auto &thread : *tombstone.mutable_threads()) {
    for (auto &frame : *thread.second.mutable_current_backtrace()) {
      symbolize_backtrace_frame(frame, sym);
    }
  }

  for (auto &cause : *tombstone.mutable_causes()) {
    if (cause.has_memory_error() && cause.memory_error().has_heap()) {
      HeapObject* heap_object = cause.mutable_memory_error()->mutable_heap();

      for (auto &frame : *heap_object->mutable_allocation_backtrace()) {
        symbolize_backtrace_frame(frame, sym);
      }
      for (auto &frame : *heap_object->mutable_deallocation_backtrace()) {
        symbolize_backtrace_frame(frame, sym);
      }
    }
  }
}
