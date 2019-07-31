/*
 * Copyright (C) 2018 The Android Open Source Project
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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class Modprobe {
  public:
    Modprobe(const std::vector<std::string>&);

    bool LoadListedModules();
    bool LoadWithAliases(const std::string& module_name, bool strict,
                         const std::string& parms = "");
    bool Remove(const std::string& module_name);

  private:
    std::string MakeCanonical(const std::string& module_path);
    bool InsmodWithDeps(const std::string& module_name, const std::string& parms);
    bool Insmod(const std::string& path_name, const std::string& parms);
    bool Rmmod(const std::string& module_name);
    std::vector<std::string> GetDependencies(const std::string& module);
    bool ModuleExists(const std::string& module_name);

    bool ParseDepCallback(const std::string& base_path, const std::vector<std::string>& args);
    bool ParseAliasCallback(const std::vector<std::string>& args);
    bool ParseSoftdepCallback(const std::vector<std::string>& args);
    bool ParseLoadCallback(const std::vector<std::string>& args);
    bool ParseOptionsCallback(const std::vector<std::string>& args);
    void ParseCfg(const std::string& cfg, std::function<bool(const std::vector<std::string>&)> f);

    std::vector<std::pair<std::string, std::string>> module_aliases_;
    std::unordered_map<std::string, std::vector<std::string>> module_deps_;
    std::vector<std::pair<std::string, std::string>> module_pre_softdep_;
    std::vector<std::pair<std::string, std::string>> module_post_softdep_;
    std::vector<std::string> module_load_;
    std::unordered_map<std::string, std::string> module_options_;
};
