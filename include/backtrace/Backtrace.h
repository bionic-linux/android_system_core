/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef _BACKTRACE_BACKTRACE_H
#define _BACKTRACE_BACKTRACE_H

#include <stdint.h>

#include <string>
#include <vector>

#include <backtrace/backtrace_consts.h>
#include <backtrace/BacktraceMap.h>

typedef struct {
  size_t num;             // The current fame number.
  uintptr_t pc;           // The absolute pc.
  uintptr_t sp;           // The top of the stack.
  size_t stack_size;      // The size of the stack, zero indicate an unknown stack size.
  const backtrace_map_t* map;   // The map associated with the given pc.
  std::string func_name;  // The function name associated with this pc, NULL if not found.
  uintptr_t func_offset;  // pc relative to the start of the function, only valid if func_name is not NULL.
} backtrace_frame_data_t;

typedef std::vector<backtrace_frame_data_t>::const_iterator backtrace_frame_const_iterator;

// Forward declarations.
class BacktraceImpl;

class Backtrace {
public:
  // Create the correct Backtrace object based on what is to be unwound.
  // If pid < 0 or equals the current pid, then the Backtrace object
  // corresponds to the current process.
  // If pid < 0 or equals the current pid and tid >= 0, then the Backtrace
  // object corresponds to a thread in the current process.
  // If pid >= 0 and tid < 0, then the Backtrace object corresponds to a
  // different process.
  // Tracing a thread in a different process is not supported.
  // If map is NULL, then create the map and manage it internally.
  // If map is not NULL, the map is still owned by the caller.
  static Backtrace* Create(pid_t pid, pid_t tid, BacktraceMap* map = NULL);

  virtual ~Backtrace();

  // Get the current stack trace and store in the backtrace_ structure.
  virtual bool Unwind(size_t num_ignore_frames);

  // Get the function name and offset into the function given the pc.
  // If the string is empty, then no valid function name was found.
  virtual std::string GetFunctionName(uintptr_t pc, uintptr_t* offset);

  // Find the map associated with the given pc.
  virtual const backtrace_map_t* FindMap(uintptr_t pc);

  // Take ownership of the BacktraceMap object associated with the backtrace.
  // If this is called, the caller must handle deleting the object themselves.
  virtual BacktraceMap* TakeMapOwnership();

  // Read the data at a specific address.
  virtual bool ReadWord(uintptr_t ptr, uint32_t* out_value) = 0;

  // Create a string representing the formatted line of backtrace information
  // for a single frame.
  virtual std::string FormatFrameData(size_t frame_num);
  virtual std::string FormatFrameData(const backtrace_frame_data_t* frame);

  pid_t Pid() { return pid_; }
  pid_t Tid() { return tid_; }
  size_t NumFrames() { return frames_.size(); }

  const backtrace_frame_data_t* GetFrame(size_t frame_num) {
    if (frame_num >= frames_.size()) {
      return NULL;
    }
    return &frames_[frame_num];
  }

  backtrace_frame_const_iterator begin() { return frames_.begin(); }
  backtrace_frame_const_iterator end() { return frames_.end(); }

  BacktraceMap* GetMap() { return map_; }

protected:
  Backtrace(BacktraceImpl* impl, pid_t pid, BacktraceMap* map);

  virtual bool VerifyReadWordArgs(uintptr_t ptr, uint32_t* out_value);

  bool BuildMap();

  pid_t pid_;
  pid_t tid_;

  BacktraceMap* map_;
  bool map_shared_;

  std::vector<backtrace_frame_data_t> frames_;

  BacktraceImpl* impl_;

  friend class BacktraceImpl;
};

#endif // _BACKTRACE_BACKTRACE_H
