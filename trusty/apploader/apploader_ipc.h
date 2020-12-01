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

#pragma once

#include <stdint.h>

#define APPLOADER_PORT "com.android.trusty.apploader"

enum apploader_command : uint32_t {
    APPLOADER_REQ_SHIFT = 1,
    APPLOADER_RESP_BIT = 1,

    APPLOADER_CMD_LOAD_APPLICATION = (0 << APPLOADER_REQ_SHIFT),
    APPLOADER_CMD_GET_VERSION = (1 << APPLOADER_REQ_SHIFT),
    APPLOADER_CMD_UNLOAD_APPLICATION = (2 << APPLOADER_REQ_SHIFT),
};

enum apploader_error : uint32_t {
    APPLOADER_NO_ERROR = 0,
    APPLOADER_ERR_UNKNOWN_CMD,
    APPLOADER_ERR_INVALID_CMD,
    APPLOADER_ERR_NO_MEMORY,
    APPLOADER_ERR_VERIFICATION_FAILED,
    APPLOADER_ERR_LOADING_FAILED,
    APPLOADER_ERR_ALREADY_EXISTS,
    APPLOADER_ERR_ALREADY_STARTED,
    APPLOADER_ERR_INTERNAL,
};

/**
 * apploader_load_app_flag - flags for LOAD_APPLICATION command
 * @APPLOADER_LOAD_APP_START: start the application immediately after loading
 */
enum apploader_load_app_flag : uint32_t {
    APPLOADER_LOAD_APP_START = (1 << 0),
    APPLOADER_LOAD_APP_MASK = APPLOADER_LOAD_APP_START,
};

/**
 * apploader_header - Serial header for communicating with apploader
 * @cmd: the command; one of enum apploader_command values.
 */
struct apploader_header {
    uint32_t cmd;
};

/**
 * apploader_load_app_req - Serial arguments for LOAD_APPLICATION command
 * @package_size: size of the application package.
 * @flags: flags; one of enum apploader_load_app_flag flags.
 */
struct apploader_load_app_req {
    uint64_t package_size;
    uint32_t flags;
};

/**
 * apploader_resp - Common header for all apploader responses
 * @hdr - header with command value.
 * @error - error code returned by peer; one of enum apploader_error values.
 *
 * This structure is followed by the response-specific payload.
 */
struct apploader_resp {
    struct apploader_header hdr;
    uint32_t error;
};
