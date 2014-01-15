/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef _BACKTRACE_BACKTRACE_MAP_H
#define _BACKTRACE_BACKTRACE_MAP_H

#include <stdint.h>
#include <sys/mman.h>

#include <string>
#include <vector>

typedef struct {
  uintptr_t start;
  uintptr_t end;
  int flags;
  std::string name;
} backtrace_map_t;

typedef std::vector<backtrace_map_t>::const_iterator backtrace_map_const_iterator;

class BacktraceMap {
public:
  BacktraceMap(pid_t pid);
  virtual ~BacktraceMap();

  // Get the map data structure for the given address.
  const backtrace_map_t* Find(uintptr_t addr);

  // The flags returned are the same flags as used by the mmap call.
  // The values are PROT_*.
  int GetFlags(uintptr_t pc) {
    const backtrace_map_t* map = Find(pc);
    if (map) {
      return map->flags;
    }
    return PROT_NONE;
  }

  bool IsReadable(uintptr_t pc) { return GetFlags(pc) & PROT_READ; }
  bool IsWritable(uintptr_t pc) { return GetFlags(pc) & PROT_WRITE; }
  bool IsExecutable(uintptr_t pc) { return GetFlags(pc) & PROT_EXEC; }

  backtrace_map_const_iterator begin() { return maps_.begin(); }
  backtrace_map_const_iterator end() { return maps_.end(); }

  virtual bool Build();

protected:
  virtual bool ParseLine(const char* line, backtrace_map_t* map);

  std::vector<backtrace_map_t> maps_;
  pid_t pid_;
};

#endif // _BACKTRACE_BACKTRACE_MAP_H
