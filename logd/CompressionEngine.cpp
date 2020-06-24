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

#include "CompressionEngine.h"

#include <limits>

#include <android-base/logging.h>
#include <zlib.h>
#include <zstd.h>

CompressionEngine& CompressionEngine::GetInstance() {
    static CompressionEngine* engine = new ZstdCompressionEngine();
    return *engine;
}

bool ZlibCompressionEngine::Compress(uint8_t* in, size_t in_size, std::unique_ptr<uint8_t[]>& out,
                                     size_t& out_size) {
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK) {
        LOG(FATAL) << "deflateInit() failed";
    }

    CHECK_LE(in_size, std::numeric_limits<uint32_t>::max());
    uint32_t deflate_bound = deflateBound(&strm, in_size);

    std::unique_ptr<uint8_t[]> temp_decompressed(new uint8_t[deflate_bound]);

    strm.avail_in = in_size;
    strm.next_in = in;
    strm.avail_out = deflate_bound;
    strm.next_out = temp_decompressed.get();
    ret = deflate(&strm, Z_FINISH);
    CHECK_EQ(ret, Z_STREAM_END);

    out_size = strm.total_out;
    deflateEnd(&strm);

    out.reset(new uint8_t[out_size]);
    memcpy(out.get(), temp_decompressed.get(), out_size);

    return true;
}

bool ZlibCompressionEngine::Decompress(uint8_t* in, size_t in_size, std::unique_ptr<uint8_t[]>& out,
                                       size_t out_size) {
    out.reset(new uint8_t[out_size]);

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = in_size;
    strm.next_in = in;
    strm.avail_out = out_size;
    strm.next_out = out.get();

    inflateInit(&strm);
    int ret = inflate(&strm, Z_NO_FLUSH);

    CHECK_EQ(strm.avail_in, 0U);
    CHECK_EQ(strm.avail_out, 0U);
    CHECK_EQ(ret, Z_STREAM_END);
    inflateEnd(&strm);

    return true;
}

bool ZstdCompressionEngine::Compress(uint8_t* in, size_t in_size, std::unique_ptr<uint8_t[]>& out,
                                     size_t& out_size) {
    uint32_t compress_bound = ZSTD_compressBound(in_size);
    std::unique_ptr<uint8_t[]> temp_decompressed(new uint8_t[compress_bound]);

    out_size = ZSTD_compress(temp_decompressed.get(), compress_bound, in, in_size, 1);
    if (ZSTD_isError(out_size)) {
        LOG(FATAL) << "ZSTD_compress failed: " << ZSTD_getErrorName(out_size);
    }
    out.reset(new uint8_t[compress_bound]);
    memcpy(out.get(), temp_decompressed.get(), out_size);

    return true;
}

bool ZstdCompressionEngine::Decompress(uint8_t* in, size_t in_size, std::unique_ptr<uint8_t[]>& out,
                                       size_t out_size) {
    out.reset(new uint8_t[out_size]);
    size_t result = ZSTD_decompress(out.get(), out_size, in, in_size);
    if (ZSTD_isError(result)) {
        LOG(FATAL) << "ZSTD_decompress failed: " << ZSTD_getErrorName(result);
    }
    CHECK_EQ(result, out_size);
    return true;
}
