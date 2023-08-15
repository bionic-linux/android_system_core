//
// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include <cstdint>
#include <memory>
#include "libsnapshot/cow_format.h"

namespace android {
namespace snapshot {

class ICompressor {
  public:
    explicit ICompressor(int32_t compression_level) : compression_level_(compression_level) {}

    virtual ~ICompressor() {}
    // Factory methods for compression methods.
    static std::unique_ptr<ICompressor> Gz(int32_t compression_level);
    static std::unique_ptr<ICompressor> Brotli(int32_t compression_level);
    static std::unique_ptr<ICompressor> Lz4(int32_t compression_level);
    static std::unique_ptr<ICompressor> Zstd(int32_t compression_level);

    static std::unique_ptr<ICompressor> Create(CowCompression compression);

    int32_t GetCompressionLevel() const { return compression_level_; }

    [[nodiscard]] virtual std::basic_string<uint8_t> Compress(const void* data,
                                                              size_t length) const = 0;

  private:
    int32_t compression_level_;
};
}  // namespace snapshot
}  // namespace android