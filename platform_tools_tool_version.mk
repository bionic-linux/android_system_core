# Copyright (C) 2017 Google Inc.
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

platform_tools_version := $(shell sed \
    's/$${PLATFORM_SDK_VERSION}/$(PLATFORM_SDK_VERSION)/ ; s/Pkg.Revision=\(.*\)/\1/p ; d' \
    $(ANDROID_BUILD_TOP)/development/sdk/plat_tools_source.prop_template \
  )
ifneq ($(filter-out %.0.0, $(platform_tools_version)),)
  tool_version := $(platform_tools_version)
else
  tool_version := $(BUILD_NUMBER_FROM_FILE)
endif
