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

#include <errno.h>
#include <getopt.h>
#include <libxml/xmlreader.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <trusty_keymaster/ipc/trusty_keymaster_ipc.h>

static const char* _sopts = "h";
static const struct option _lopts[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
};

static const char* usage =
        "Usage: %s [options] xml-file\n"
        "\n"
        "options:\n"
        "  -h, --help            prints this message and exit\n"
        "\n";

static void print_usage_and_exit(const char* prog, int code) {
    fprintf(stderr, usage, prog);
    exit(code);
}

static void parse_options(int argc, char** argv) {
    int c;
    int oidx = 0;

    while (1) {
        c = getopt_long(argc, argv, _sopts, _lopts, &oidx);
        if (c == -1) {
            break; /* done */
        }

        switch (c) {
            case 'h':
                print_usage_and_exit(argv[0], EXIT_SUCCESS);
                break;

            default:
                print_usage_and_exit(argv[0], EXIT_FAILURE);
        }
    }
}

struct SetAttestationKeyRequest : public keymaster::KeymasterMessage {
    explicit SetAttestationKeyRequest(int32_t ver = keymaster::MAX_MESSAGE_VERSION)
        : KeymasterMessage(ver) {}

    size_t SerializedSize() const override { return sizeof(uint32_t) + key_data.SerializedSize(); }
    uint8_t* Serialize(uint8_t* buf, const uint8_t* end) const override {
        buf = keymaster::append_uint32_to_buf(buf, end, algorithm);
        return key_data.Serialize(buf, end);
    }
    bool Deserialize(const uint8_t** buf_ptr, const uint8_t* end) override {
        return keymaster::copy_uint32_from_buf(buf_ptr, end, &algorithm) &&
               key_data.Deserialize(buf_ptr, end);
    }

    keymaster_algorithm_t algorithm;
    keymaster::Buffer key_data;
};

struct SetAttestationKeyResponse : public keymaster::KeymasterResponse {
    explicit SetAttestationKeyResponse(int32_t ver = keymaster::MAX_MESSAGE_VERSION)
        : keymaster::KeymasterResponse(ver) {}

    size_t NonErrorSerializedSize() const override { return 0; }
    uint8_t* NonErrorSerialize(uint8_t* buf, const uint8_t*) const override { return buf; }
    bool NonErrorDeserialize(const uint8_t**, const uint8_t*) override { return true; }
};

static int set_attestation_key_or_cert_bin(uint32_t cmd, keymaster_algorithm_t algorithm,
                                           const void* key_data, size_t key_data_size) {
    int ret;

    SetAttestationKeyRequest req;
    req.algorithm = algorithm;
    req.key_data.Reinitialize(key_data, key_data_size);
    SetAttestationKeyResponse rsp;

    ret = trusty_keymaster_send(cmd, req, &rsp);
    if (ret) {
        fprintf(stderr, "trusty_keymaster_send failed %d\n", ret);
        return ret;
    }

    return 0;
}

static int set_attestation_key_or_cert_pem(uint32_t cmd, keymaster_algorithm_t algorithm,
                                           const xmlChar* pemkey) {
    int ret;
    int sslret;
    BIO* bio = BIO_new_mem_buf(pemkey, xmlStrlen(pemkey));
    if (!bio) {
        fprintf(stderr, "BIO_new_mem_buf failed\n");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    char* key_name;
    char* key_header;
    uint8_t* key;
    long keylen;
    sslret = PEM_read_bio(bio, &key_name, &key_header, &key, &keylen);
    BIO_free(bio);

    if (!sslret) {
        fprintf(stderr, "PEM_read_bio failed\n");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    ret = set_attestation_key_or_cert_bin(cmd, algorithm, key, keylen);
    OPENSSL_free(key_name);
    OPENSSL_free(key_header);
    OPENSSL_free(key);

    return ret;
}

static int set_attestation_key_or_cert(uint32_t cmd, const xmlChar* algorithm_str,
                                       const xmlChar* format, const xmlChar* str) {
    int ret;
    keymaster_algorithm_t algorithm;

    if (xmlStrEqual(algorithm_str, BAD_CAST "rsa")) {
        algorithm = KM_ALGORITHM_RSA;
    } else if (xmlStrEqual(algorithm_str, BAD_CAST "ecdsa")) {
        algorithm = KM_ALGORITHM_EC;
    } else {
        printf("unsupported algorithm: %s\n", algorithm_str);
        return -1;
    }

    if (xmlStrEqual(format, BAD_CAST "pem")) {
        ret = set_attestation_key_or_cert_pem(cmd, algorithm, str);
    } else {
        printf("unsupported key/cert format: %s\n", format);
        return -1;
    }
    printf("algorithm %s, format %s, cmd %x done\n", algorithm_str, format, cmd);
    return ret;
}

static int process_xml(xmlTextReaderPtr xml) {
    int ret;
    const xmlChar* algorithm;

    const xmlChar* element = NULL;
    const xmlChar* element_format = NULL;
    while ((ret = xmlTextReaderRead(xml)) == 1) {
        int nodetype = xmlTextReaderNodeType(xml);
        const xmlChar *name, *value;
        name = xmlTextReaderConstName(xml);
        switch (nodetype) {
            case XML_READER_TYPE_ELEMENT:
                element = name;
                element_format = xmlTextReaderGetAttribute(xml, BAD_CAST "format");
                if (xmlStrEqual(name, BAD_CAST "Key")) {
                    algorithm = xmlTextReaderGetAttribute(xml, BAD_CAST "algorithm");
                }
                break;
            case XML_READER_TYPE_TEXT:
                value = xmlTextReaderConstValue(xml);
                uint32_t cmd;
                if (xmlStrEqual(element, BAD_CAST "PrivateKey")) {
                    cmd = KM_SET_ATTESTATION_KEY;
                } else if (xmlStrEqual(element, BAD_CAST "Certificate")) {
                    cmd = KM_APPEND_ATTESTATION_CERT_CHAIN;
                } else {
                    break;
                }

                ret = set_attestation_key_or_cert(cmd, algorithm, element_format, value);
                if (ret) {
                    return ret;
                }
                break;
            case XML_READER_TYPE_END_ELEMENT:
                element = NULL;
                break;
        }
    }
    return 0;
}

static int parse_xml_file(const char* filename) {
    int ret;
    xmlTextReaderPtr xml = xmlReaderForFile(filename, NULL, 0);
    if (!xml) {
        fprintf(stderr, "failed to open %s\n", filename);
        return -1;
    }

    ret = process_xml(xml);

    xmlFreeTextReader(xml);
    if (ret != 0) {
        fprintf(stderr, "failed to parse %s\n", filename);
        return -1;
    }

    return 0;
}

int main(int argc, char** argv) {
    int ret = 0;

    parse_options(argc, argv);
    if (optind + 1 != argc) {
        print_usage_and_exit(argv[0], EXIT_FAILURE);
    }

    ret = trusty_keymaster_connect();
    if (ret) {
        fprintf(stderr, "trusty_keymaster_connect failed %d\n", ret);
    } else {
        ret = parse_xml_file(argv[optind]);
        trusty_keymaster_disconnect();
    }

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
