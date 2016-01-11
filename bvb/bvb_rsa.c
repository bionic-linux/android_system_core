/*
 * Copyright (C) 2016 The Android Open Source Project
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

/* Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Implementation of RSA signature verification which uses a pre-processed
 * key for computation. The code extends Android's RSA verification code to
 * support multiple RSA key lengths and hash digest algorithms.
 */

#include "bvb_rsa.h"
#include "bvb_sha.h"
#include "bvb_util.h"

typedef struct Key {
  unsigned int len;  /* Length of n[] in number of uint32_t */
  uint32_t n0inv;    /* -1 / n[0] mod 2^32 */
  uint32_t* n;       /* modulus as little endian array */
  uint32_t* rr;      /* R^2 as little endian array */
} Key;

Key* parse_key_data(const uint8_t* data, size_t length) {
  BvbRSAPublicKeyHeader h;
  Key* key = NULL;
  size_t expected_length;
  unsigned int i;
  const uint8_t* n;
  const uint8_t *rr;

  bvb_rsa_public_key_header_to_host_byte_order(
      (const BvbRSAPublicKeyHeader *) data, &h);

  if (!(h.key_num_bits == 2048 ||
        h.key_num_bits == 4096 ||
        h.key_num_bits == 8192)) {
    bvb_debug("Unexpected key length.\n");
    goto fail;
  }

  expected_length = sizeof(BvbRSAPublicKeyHeader) + 2*h.key_num_bits/8;
  if (length != expected_length) {
    bvb_debug("Key does not match expected length.\n");
    goto fail;
  }

  n = data + sizeof(BvbRSAPublicKeyHeader);
  rr = data + sizeof(BvbRSAPublicKeyHeader) + h.key_num_bits/8;

  // Store n and rr following the key header so we only have to do one
  // allocation.
  key = (Key *) (bvb_malloc(sizeof(Key) + 2*h.key_num_bits/8));
  if (key == NULL)
    goto fail;

  key->len = h.key_num_bits/32;
  key->n0inv = h.n0inv;
  key->n =  (uint32_t *) (key + 1);  // Skip ahead sizeof(Key) bytes.
  key->rr = key->n + key->len;

  // Crypto-code below (modpowF4() and friends) expects the key in
  // little-endian format (rather than the format we're storing the
  // key in), so convert it.
  for (i = 0; i < key->len; i++) {
    key->n[i] = bvb_be32toh(((uint32_t *) n)[key->len - i - 1]);
    key->rr[i] = bvb_be32toh(((uint32_t *) rr)[key->len - i - 1]);
  }
  return key;

fail:
  if (key != NULL)
    bvb_free(key);
  return NULL;
}

void free_parsed_key(Key* key) {
  bvb_free(key);
}

/* a[] -= mod */
static void subM(const Key* key, uint32_t* a) {
  int64_t A = 0;
  uint32_t i;
  for (i = 0; i < key->len; ++i) {
    A += (uint64_t)a[i] - key->n[i];
    a[i] = (uint32_t)A;
    A >>= 32;
  }
}

/* return a[] >= mod */
static int geM(const Key* key, uint32_t *a) {
  uint32_t i;
  for (i = key->len; i;) {
    --i;
    if (a[i] < key->n[i]) return 0;
    if (a[i] > key->n[i]) return 1;
  }
  return 1;  /* equal */
 }

/* montgomery c[] += a * b[] / R % mod */
static void montMulAdd(const Key* key,
                       uint32_t* c,
                       const uint32_t a,
                       const uint32_t* b) {
  uint64_t A = (uint64_t)a * b[0] + c[0];
  uint32_t d0 = (uint32_t)A * key->n0inv;
  uint64_t B = (uint64_t)d0 * key->n[0] + (uint32_t)A;
  uint32_t i;

  for (i = 1; i < key->len; ++i) {
    A = (A >> 32) + (uint64_t)a * b[i] + c[i];
    B = (B >> 32) + (uint64_t)d0 * key->n[i] + (uint32_t)A;
    c[i - 1] = (uint32_t)B;
  }

  A = (A >> 32) + (B >> 32);

  c[i - 1] = (uint32_t)A;

  if (A >> 32) {
    subM(key, c);
  }
}

/* montgomery c[] = a[] * b[] / R % mod */
static void montMul(const Key* key,
                    uint32_t* c,
                    uint32_t* a,
                    uint32_t* b) {
  uint32_t i;
  for (i = 0; i < key->len; ++i) {
    c[i] = 0;
  }
  for (i = 0; i < key->len; ++i) {
    montMulAdd(key, c, a[i], b);
  }
}

/* In-place public exponentiation. (65537}
 * Input and output big-endian byte array in inout.
 */
static void modpowF4(const Key *key,
                     uint8_t* inout) {
  uint32_t* a = (uint32_t*) bvb_malloc(key->len * sizeof(uint32_t));
  uint32_t* aR = (uint32_t*) bvb_malloc(key->len * sizeof(uint32_t));
  uint32_t* aaR = (uint32_t*) bvb_malloc(key->len * sizeof(uint32_t));
  if (a == NULL || aR == NULL || aaR == NULL)
    goto out;

  uint32_t* aaa = aaR;  /* Re-use location. */
  int i;

  /* Convert from big endian byte array to little endian word array. */
  for (i = 0; i < (int)key->len; ++i) {
    uint32_t tmp =
        (inout[((key->len - 1 - i) * 4) + 0] << 24) |
        (inout[((key->len - 1 - i) * 4) + 1] << 16) |
        (inout[((key->len - 1 - i) * 4) + 2] << 8) |
        (inout[((key->len - 1 - i) * 4) + 3] << 0);
    a[i] = tmp;
  }

  montMul(key, aR, a, key->rr);  /* aR = a * RR / R mod M   */
  for (i = 0; i < 16; i+=2) {
    montMul(key, aaR, aR, aR);  /* aaR = aR * aR / R mod M */
    montMul(key, aR, aaR, aaR);  /* aR = aaR * aaR / R mod M */
  }
  montMul(key, aaa, aR, a);  /* aaa = aR * a / R mod M */


  /* Make sure aaa < mod; aaa is at most 1x mod too large. */
  if (geM(key, aaa)) {
    subM(key, aaa);
  }

  /* Convert to bigendian byte array */
  for (i = (int)key->len - 1; i >= 0; --i) {
    uint32_t tmp = aaa[i];
    *inout++ = (uint8_t)(tmp >> 24);
    *inout++ = (uint8_t)(tmp >> 16);
    *inout++ = (uint8_t)(tmp >>  8);
    *inout++ = (uint8_t)(tmp >>  0);
  }

out:
  if (a != NULL)
    bvb_free(a);
  if (aR != NULL)
    bvb_free(aR);
  if (aaR != NULL)
    bvb_free(aaR);
}

/* Verify a RSA PKCS1.5 signature against an expected hash.
 * Returns 0 on failure, 1 on success.
 */
int bvb_rsa_verify(const uint8_t* key, size_t key_num_bytes,
                   const uint8_t* sig, size_t sig_num_bytes,
                   const uint8_t* hash, size_t hash_num_bytes,
                   const uint8_t* padding, size_t padding_num_bytes) {
  uint8_t* buf = NULL;
  Key* parsed_key = NULL;
  int success = 0;

  if (key == NULL || sig == NULL || hash == NULL || padding == NULL) {
    bvb_debug("Invalid input.\n");
    goto out;
  }

  parsed_key = parse_key_data(key, key_num_bytes);
  if (parsed_key == NULL) {
    bvb_debug("Error parsing key.\n");
    goto out;
  }

  if (sig_num_bytes != (parsed_key->len * sizeof(uint32_t))) {
    bvb_debug("Signature length does not match key length.\n");
    goto out;
  }

  if (padding_num_bytes != sig_num_bytes - hash_num_bytes) {
    bvb_debug("Padding length does not match hash and signature lengths.\n");
    goto out;
  }

  buf = (uint8_t *) bvb_malloc(sig_num_bytes);
  if (buf == NULL) {
    bvb_debug("Error allocating %d bytes.\n", (int) sig_num_bytes);
    goto out;
  }
  bvb_memcpy(buf, sig, sig_num_bytes);

  modpowF4(parsed_key, buf);

  /* Check padding bytes.
   *
   * Even though there are probably no timing issues here, we use
   * bvb_safe_memcmp() just to be on the safe side.
   */
  if (bvb_safe_memcmp(buf, padding, padding_num_bytes)) {
    bvb_debug("Padding check failed.\n");
    goto out;
  }

  /* Check hash. */
  if (bvb_safe_memcmp(buf + padding_num_bytes, hash, hash_num_bytes)) {
    bvb_debug("Hash check failed.\n");
    goto out;
  }

  success = 1;

out:
  if (parsed_key != NULL)
    free_parsed_key(parsed_key);
  if (buf != NULL)
    bvb_free(buf);
  return success;
}
