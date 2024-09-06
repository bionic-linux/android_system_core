# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

AIDL_DIR = system/core/trusty/utils/trusty-binder-test/aidl

MODULE_CRATE_NAME := android_trusty_test_commservice

MODULE_AIDL_LANGUAGE := rust

MODULE_AIDL_PACKAGE := android/trusty/test/commservice

MODULE_AIDL_INCLUDES := \
	-I $(AIDL_DIR) \

MODULE_AIDLS := \
    $(AIDL_DIR)/$(MODULE_AIDL_PACKAGE)/ICommService.aidl               \

include make/aidl.mk
