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

#ifndef FASTDEPLOY_H
#define FASTDEPLOY_H

#include "adb.h"

typedef enum EFastDeploy_AgentUpdateStrategy {
    FastDeploy_AgentUpdateAlways,
    FastDeploy_AgentUpdateNewerTimeStamp,
    FastDeploy_AgentUpdateDifferentVersion
} FastDeploy_AgentUpdateStrategy;

bool update_agent(FastDeploy_AgentUpdateStrategy agentUpdateStrategy);
int extract_metadata(const char* apkPath, FILE* outputFp);
int create_patch(const char* apkPath, const char* metadatapath, const char* patchPath);
int install_patch(const char* apkPath, const char* patchPath);

#endif
