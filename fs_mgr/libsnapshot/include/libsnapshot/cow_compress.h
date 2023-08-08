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

#include <memory>
namespace android {
namespace snapshot {

class ICompressor {
  public:
    virtual ~ICompressor() {}
    // Factory methods for compression methods.
    static std::unique_ptr<ICompressor> Uncompressed();
    static std::unique_ptr<ICompressor> Gz();
    static std::unique_ptr<ICompressor> Brotli();
    static std::unique_ptr<ICompressor> Lz4();
    static std::unique_ptr<ICompressor> Zstd();

    virtual bool Init() = 0;
    // for backwards compatability, currently compression algorithm is enumerated
    virtual uint32_t Enumerate() = 0;

    void setCompressionLevel(uint32_t compression_level) {
        compression_level_ = compression_level;
    };

    virtual std::basic_string<uint8_t> Compress(const void* data, size_t length) = 0;

  private:
    uint32_t compression_level_;
};
}  // namespace snapshot
}  // namespace android