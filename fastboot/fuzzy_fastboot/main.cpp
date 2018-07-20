/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex>

#include <termios.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/parsenetaddress.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/test_utils.h>
#include <android-base/unique_fd.h>
#include <gtest/gtest.h>
#include <sparse/sparse.h>
#include <ziparchive/zip_archive.h>

#include "bootimg_utils.h"
#include "diagnose_usb.h"
#include "fastboot_driver.h"
#include "fs.h"
#include "tcp.h"
#include "transport.h"
#include "udp.h"
#include "usb.h"

#include "extensions.h"

#define USB_TIMEOUT 3000

namespace fastboot {

constexpr char USB_PORT_GONE[] =
        "The USB port has disappeared, this is usually due to the bootloader crashing";

// const char scratch_partition[] = "userdata";
const std::vector<std::string> CMDS{"boot",        "continue", "download:", "erase:",
                                    "flash:",      "getvar:",  "powerdown", "reboot",
                                    "set_active:", "upload",   "verify"};

constexpr int rand_seed = 0;
std::default_random_engine rnd(rand_seed);

// Legal ascii
const auto rand_legal = []() -> char { return rnd() % 128; };
// Illegal ascii
const auto rand_illegal = []() -> char { return rand_legal() + 128; };
// All chars
const auto rand_char = []() -> char { return rnd() % 256; };

::std::ostream& operator<<(::std::ostream& os, const RetCode& ret) {
    return os << FastBootDriver::RCString(ret);  // whatever needed to print bar to os
}

std::string RandomString(size_t length, std::function<char(void)> provider) {
    std::string str(length, 0);
    std::generate_n(str.begin(), length, provider);
    return str;
}

// A special class for sniffing reads and writes
class UsbTransportSniffer : public UsbTransport {
  public:
    enum EventType {
        READ,
        WRITE,
        RESET,
        SERIAL,  // Serial log message from device
        READ_ERROR,
        WRITE_ERROR,
    };

    struct Event {
        Event(EventType t, const std::vector<char> cbuf) : type(t), buf(cbuf) {
            start = std::chrono::high_resolution_clock::now();
        };
        EventType type;
        std::chrono::high_resolution_clock::time_point start;
        const std::vector<char> buf;
    };

    UsbTransportSniffer(std::unique_ptr<UsbTransport> transport, const int serial_fd = 0)
        : transport_(std::move(transport)), serial_fd_(serial_fd) {}

    virtual ssize_t Read(void* data, size_t len) override {
        ProcessSerial();

        ssize_t ret = transport_->Read(data, len);
        if (ret < 0) {
            const char* err = strerror(errno);
            std::vector<char> buf(err, err + strlen(err));
            Event e(READ_ERROR, std::move(buf));
            transfers_.push_back(e);
            return ret;
        }

        char* cdata = static_cast<char*>(data);
        std::vector<char> buf(cdata, cdata + ret);
        Event e(READ, std::move(buf));
        transfers_.push_back(e);

        ProcessSerial();
        return ret;
    }

    virtual ssize_t Write(const void* data, size_t len) override {
        ProcessSerial();

        size_t ret = transport_->Write(data, len);
        if (ret != len) {
            const char* err = strerror(errno);
            std::vector<char> buf(err, err + strlen(err));
            Event e(WRITE_ERROR, std::move(buf));
            transfers_.push_back(e);
            return ret;
        }

        const char* cdata = static_cast<const char*>(data);
        std::vector<char> buf(cdata, cdata + len);
        Event e(WRITE, std::move(buf));
        transfers_.push_back(e);

        ProcessSerial();
        return ret;
    }

    virtual int Close() override { return transport_->Close(); }

    virtual int Reset() override {
        ProcessSerial();
        int ret = transport_->Reset();
        std::vector<char> buf;
        Event e(RESET, std::move(buf));
        transfers_.push_back(e);
        ProcessSerial();
        return ret;
    }

    const std::vector<Event> Transfers() { return transfers_; }

    std::string CreateTrace() {
        std::string ret;
        //
        const auto no_print = [](char c) -> bool { return !(isgraph(c) || isspace(c)); };
        const auto msg = [&ret, no_print](const std::vector<char>& buf) {
            ret += android::base::StringPrintf("(%lu bytes): ", buf.size());
            std::vector<const char>::iterator iter = buf.end();
            const unsigned max_chars = 40;
            if (buf.size() > max_chars) {
                iter = buf.begin() + max_chars;
            }
            ret += '"';
            if (std::count_if(buf.begin(), iter, no_print) == 0) {  // print as ascii
                ret.insert(ret.end(), buf.begin(), iter);
            } else {  // print as hex
                std::stringstream ss;
                for (auto c = buf.begin(); c < iter; c++) {
                    ss << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<uint16_t>(static_cast<uint8_t>(*c));
                    ss << ',';
                }
                ret += ss.str();
            }
            if (buf.size() > max_chars) {
                ret += android::base::StringPrintf("...\"(+%lu bytes)\n", buf.size() - max_chars);
            } else {
                ret += "\"\n";
            }
        };

        for (const auto& event : transfers_) {
            const std::vector<char>& cbuf = event.buf;
            const std::string tmp(cbuf.begin(), cbuf.end());
            auto start = transfers_.front().start;
            auto durr = event.start - start;
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(durr).count();

            switch (event.type) {
                case READ:
                    ret += android::base::StringPrintf("[READ %lldms]", millis);
                    msg(cbuf);
                    break;

                case WRITE:
                    ret += android::base::StringPrintf("[WRITE %lldms]", millis);
                    msg(cbuf);
                    break;

                case RESET:
                    ret += android::base::StringPrintf("[RESET %lldms]\n", millis);
                    break;

                case READ_ERROR:
                    ret += android::base::StringPrintf("[READ_ERROR %lldms] %s\n", millis,
                                                       tmp.c_str());
                    break;

                case WRITE_ERROR:
                    ret += android::base::StringPrintf("[WRITE_ERROR %lldms] %s\n", millis,
                                                       tmp.c_str());
                    break;

                case SERIAL:
                    ret += android::base::StringPrintf("[SERIAL %lldms] %s", millis, tmp.c_str());
                    if (ret.back() != '\n') ret += '\n';
                    break;
            }
        }
        return ret;
    }

    void ProcessSerial() {
        if (serial_fd_ <= 0) return;

        fd_set set;
        struct timeval timeout;

        FD_ZERO(&set);
        FD_SET(serial_fd_, &set);
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms

        int count = 0;
        int n = 0;
        std::vector<char> buf;
        buf.resize(1000);
        while (select(serial_fd_ + 1, &set, NULL, NULL, &timeout) > 0) {
            n = read(serial_fd_, buf.data() + count, buf.size() - count);
            if (n > 0) {
                count += n;
            } else {
                break;
            }
        }

        buf.resize(count);

        if (count > 0) {
            Event e(SERIAL, std::move(buf));
            transfers_.push_back(e);
        }
    }

  private:
    std::vector<Event> transfers_;
    std::unique_ptr<UsbTransport> transport_;
    const int serial_fd_;
};

class FastBootTest : public testing::Test {
  public:
    static int serial_port;
    static constexpr int MAX_USB_TRIES = 10;

    static int MatchFastboot(usb_ifc_info* info, const char* local_serial = nullptr) {
        if (info->ifc_class != 0xff || info->ifc_subclass != 0x42 || info->ifc_protocol != 0x03) {
            return -1;
        }

        cb_scratch = info->device_path;

        // require matching serial number or device path if requested
        // at the command line with the -s option.
        if (local_serial && (strcmp(local_serial, info->serial_number) != 0 &&
                             strcmp(local_serial, info->device_path) != 0))
            return -1;
        return 0;
    }

    inline bool UsbStillAvailible() {
        // For some reason someone decided to prefix the path with "usb:"
        std::string prefix("usb:");
        if (std::equal(prefix.begin(), prefix.end(), device_path.begin())) {
            std::string fname(device_path.begin() + prefix.size(), device_path.end());
            std::string real_path =
                    android::base::StringPrintf("/sys/bus/usb/devices/%s/serial", fname.c_str());
            std::ifstream f(real_path.c_str());
            return f.good();
        }
        exit(-1);  // This should never happend
        return true;
    }

  protected:
    RetCode DownloadCommand(uint32_t size, std::string* response = nullptr,
                            std::vector<std::string>* info = nullptr) {
        return fb->DownloadCommand(size, response, info);
    }

    RetCode SendBuffer(const std::vector<char>& buf) { return fb->SendBuffer(buf); }

    RetCode HandleResponse(std::string* response = nullptr,
                           std::vector<std::string>* info = nullptr, int* dsize = nullptr) {
        return fb->HandleResponse(response, info, dsize);
    }

    void SetUp() override {
        if (device_path != "") {               // make sure the device is still connected
            ASSERT_TRUE(UsbStillAvailible());  // The device disconnected
        }

        const auto matcher = [](usb_ifc_info* info) -> int { return MatchFastboot(info, nullptr); };
        for (int i = 0; i < MAX_USB_TRIES && !transport; i++) {
            std::unique_ptr<UsbTransport> usb(usb_open(matcher, USB_TIMEOUT));
            if (usb)
                transport = std::unique_ptr<UsbTransportSniffer>(
                        new UsbTransportSniffer(std::move(usb), serial_port));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_TRUE(transport);  // no nullptr

        if (device_path == "") {  // We set it the first time, then make sure it never changes
            device_path = cb_scratch;
        } else {
            ASSERT_EQ(device_path, cb_scratch);  // The path can not change
        }
        fb = std::unique_ptr<FastBootDriver>(
                new FastBootDriver(transport.get(), [](std::string&) {}, true));
    }

    void TearDown() override {
        ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;

        TearDownSerial();

        fb.reset();

        if (transport) {
            // transport->Reset();
            transport->Close();
            transport.reset();
        }

        ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
    }

    void TearDownSerial() {
        if (!transport) return;
        // One last read from serial
        transport->ProcessSerial();
        if (HasFailure()) {
            // TODO, print commands leading up
            printf("<<<<<<<< TRACE BEGIN >>>>>>>>>\n");
            printf("%s", transport->CreateTrace().c_str());
            printf("<<<<<<<< TRACE END >>>>>>>>>\n");
            // std::vector<std::pair<const TransferType, const std::vector<char>>>  prev =
            // transport->Transfers();
        }
    }

    void ChangeLockState(bool unlock, bool assert_change = true) {
        if (!fb) {
            return;
        }
        std::string resp;
        EXPECT_EQ(fb->GetVar("unlocked", &resp), SUCCESS) << "getvar:unlocked failed";
        ASSERT_TRUE(resp == "no" || resp == "yes")
                << "getvar:unlocked response was not 'no' or 'yes': " + resp;

        if ((unlock && resp == "no") || (!unlock && resp == "yes")) {
            std::string cmd = unlock ? "unlock" : "lock";
            ASSERT_EQ(fb->RawCommand("flashing " + cmd, &resp), SUCCESS)
                    << "Attempting to change locked state, but 'flashing" + cmd +
                               "' command failed";
            fb.reset();
            transport->Close();
            transport.reset();
            printf("PLEASE CONFIRM '%sing' BOOTLOADER ON DEVICE\n", cmd.c_str());
            while (UsbStillAvailible())
                ;  // Wait for disconnect
            printf("WAITING FOR DEVICE");
            // Need to wait for device
            const auto matcher = [](usb_ifc_info* info) -> int {
                return MatchFastboot(info, nullptr);
            };
            while (!transport) {
                std::unique_ptr<UsbTransport> usb(usb_open(matcher, USB_TIMEOUT));
                if (usb) {
                    transport = std::unique_ptr<UsbTransportSniffer>(
                            new UsbTransportSniffer(std::move(usb), serial_port));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                putchar('.');
            }
            device_path = cb_scratch;
            fb = std::unique_ptr<FastBootDriver>(
                    new FastBootDriver(transport.get(), [](std::string&) {}, true));
            if (assert_change) {
                ASSERT_EQ(fb->GetVar("unlocked", &resp), SUCCESS) << "getvar:unlocked failed";
                ASSERT_EQ(resp, unlock ? "yes" : "no")
                        << "getvar:unlocked response was not 'no' or 'yes': " + resp;
            }
            printf("SUCCESS\n");
        }
    }

    std::unique_ptr<UsbTransportSniffer> transport;
    std::unique_ptr<FastBootDriver> fb;

  private:
    // This is an annoying hack
    static std::string cb_scratch;
    static std::string device_path;
};

std::string FastBootTest::device_path = "";
std::string FastBootTest::cb_scratch = "";

int FastBootTest::serial_port = 0;

class BasicFunctionality : public FastBootTest {
  protected:
    void SetUp() override {
        FastBootTest::SetUp();
        ChangeLockState(true);
    }
};

class Conformance : public FastBootTest {
  protected:
    void SetUp() override {
        FastBootTest::SetUp();
        ChangeLockState(true);
    }
};

class UnlockPermissions : public FastBootTest {
  protected:
    void SetUp() override {
        FastBootTest::SetUp();
        ChangeLockState(true);
    }
};

class LockPermissions : public FastBootTest {
  protected:
    void SetUp() override {
        FastBootTest::SetUp();
        ChangeLockState(false);
    }
};

class Fuzz : public FastBootTest {
  protected:
    void SetUp() override {
        FastBootTest::SetUp();
        ChangeLockState(true);
    }

    void TearDown() override {
        ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;

        TearDownSerial();

        std::string tmp;
        if (fb->GetVar("product", &tmp) != SUCCESS) {
            printf("DEVICE UNRESPONSE, attempting to recover...");
            transport->Reset();
            printf("issued USB reset...");

            if (fb->GetVar("product", &tmp) != SUCCESS) {
                printf("FAIL\n");
                exit(-1);
            }
            printf("SUCCESS!\n");
        }

        if (transport) {
            transport->Close();
            transport.reset();
        }

        ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
    }
};

// Only allow alphanumeric, _, -, and .
const auto not_allowed = [](char c) -> int {
    return !(isalnum(c) || c == '_' || c == '-' || c == '.');
};

// Test that USB even works
TEST(USBFunctionality, USBConnect) {
    const auto matcher = [](usb_ifc_info* info) -> int {
        return FastBootTest::MatchFastboot(info, nullptr);
    };
    Transport* transport = nullptr;
    for (int i = 0; i < FastBootTest::MAX_USB_TRIES && !transport; i++) {
        transport = usb_open(matcher);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(transport, nullptr) << "Could not find the fastboot device after: "
                                  << 10 * FastBootTest::MAX_USB_TRIES << "ms";
    if (transport) {
        transport->Close();
        delete transport;
    }
}

// Conformance tests
TEST_F(Conformance, GetVar) {
    std::string product;
    EXPECT_EQ(fb->GetVar("product", &product), SUCCESS) << "getvar:product failed";
    EXPECT_NE(product, "") << "getvar:product response was empty string";
    EXPECT_EQ(std::count_if(product.begin(), product.end(), not_allowed), 0)
            << "getvar:product response contained illegal chars";
    EXPECT_LE(product.size(), FB_RESPONSE_SZ - 4) << "getvar:product response was too large";
}

TEST_F(Conformance, GetVarVersionBootloader) {
    std::string var;
    EXPECT_EQ(fb->GetVar("version-bootloader", &var), SUCCESS)
            << "getvar:version-bootloader failed";
    EXPECT_NE(var, "") << "getvar:version-bootloader response was empty string";
    EXPECT_EQ(std::count_if(var.begin(), var.end(), not_allowed), 0)
            << "getvar:version-bootloader response contained illegal chars";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4) << "getvar:version-bootloader response was too large";
    ;
}

TEST_F(Conformance, GetVarVersionBaseband) {
    std::string var;
    EXPECT_EQ(fb->GetVar("version-baseband", &var), SUCCESS) << "getvar:version-baseband failed";
    EXPECT_NE(var, "") << "getvar:version-baseband response was empty string";
    EXPECT_EQ(std::count_if(var.begin(), var.end(), not_allowed), 0)
            << "getvar:version-baseband response contained illegal chars";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4) << "getvar:version-baseband response was too large";
}

TEST_F(Conformance, GetVarSerialNo) {
    std::string var;
    EXPECT_EQ(fb->GetVar("serialno", &var), SUCCESS) << "getvar:serialno failed";
    EXPECT_NE(var, "") << "getvar:serialno can not be empty string";
    EXPECT_EQ(std::count_if(var.begin(), var.end(), isalnum), var.size())
            << "getvar:serialno must be alpha-numeric";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4) << "getvar:serialno response is too long";
}

TEST_F(Conformance, GetVarSecure) {
    std::string var;
    EXPECT_EQ(fb->GetVar("secure", &var), SUCCESS);
    EXPECT_TRUE(var == "yes" || var == "no");
}

TEST_F(Conformance, GetVarOffModeCharge) {
    std::string var;
    EXPECT_EQ(fb->GetVar("off-mode-charge", &var), SUCCESS) << "getvar:off-mode-charge failed";
    EXPECT_TRUE(var == "0" || var == "1") << "getvar:off-mode-charge response must be '0' or '1'";
}

TEST_F(Conformance, GetVarVariant) {
    std::string var;
    EXPECT_EQ(fb->GetVar("variant", &var), SUCCESS) << "getvar:variant failed";
    EXPECT_NE(var, "") << "getvar:variant response can not be empty";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4) << "getvar:variant response is too large";
}

TEST_F(Conformance, GetVarRevision) {
    std::string var;
    EXPECT_EQ(fb->GetVar("hw-revision", &var), SUCCESS) << "getvar:hw-revision failed";
    EXPECT_NE(var, "") << "getvar:battery-voltage response was empty";
    EXPECT_EQ(std::count_if(var.begin(), var.end(), not_allowed), 0)
            << "getvar:hw-revision contained illegal ASCII chars";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4) << "getvar:hw-revision response was too large";
}

TEST_F(Conformance, GetVarBattVoltage) {
    std::string var;
    EXPECT_EQ(fb->GetVar("battery-voltage", &var), SUCCESS) << "getvar:battery-voltage failed";
    EXPECT_NE(var, "") << "getvar:battery-voltage response was empty";
    ;
    EXPECT_EQ(std::count_if(var.begin(), var.end(), not_allowed), 0)
            << "getvar:battery-soc-ok response contains illegal ASCII chars";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4)
            << "getvar:battery-voltage response is too large: " + var;
}

TEST_F(Conformance, GetVarBattVoltageOk) {
    std::string var;
    EXPECT_EQ(fb->GetVar("battery-soc-ok", &var), SUCCESS) << "getvar:battery-soc-ok failed";
    EXPECT_TRUE(var == "yes" || var == "no") << "getvar:battery-soc-ok must be 'yes' or 'no'";
}

TEST_F(Conformance, GetVarDownloadSize) {
    std::string var;
    EXPECT_EQ(fb->GetVar("max-download-size", &var), SUCCESS) << "getvar:max-download-size failed";
    EXPECT_NE(var, "") << "getvar:max-download-size responded with empty string";
    // This must start with 0x
    EXPECT_FALSE(isspace(var.front()))
            << "getvar:max-download-size responded with a string with leading whitespace";
    ;
    EXPECT_FALSE(var.compare(0, 2, "0x"))
            << "getvar:max-download-size responded with a string that does not start with 0x...";
    int64_t size = strtoll(var.c_str(), nullptr, 16);
    EXPECT_GT(size, 0) << "'" + var + "' is not a valid response from getvar:max-download-size";
    // At most 32-bits
    EXPECT_LE(size, std::numeric_limits<uint32_t>::max())
            << "getvar:max-download-size must fit in a uint32_t";
    EXPECT_LE(var.size(), FB_RESPONSE_SZ - 4)
            << "getvar:max-download-size responded with too large of string: " + var;
}

TEST_F(Conformance, GetVarAll) {
    std::vector<std::string> vars;
    EXPECT_EQ(fb->GetVarAll(&vars), SUCCESS) << "getvar:all failed";
    EXPECT_GT(vars.size(), 0) << "getvar:all did not respond with any INFO responses";
    for (const auto s : vars) {
        EXPECT_LE(s.size(), FB_RESPONSE_SZ - 4)
                << "getvar:all included an INFO response: 'INFO" + s << "' which is too long";
    }
}

TEST_F(Conformance, PartitionInfo) {
    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed";
    EXPECT_GT(parts.size(), 0)
            << "getvar:all did not report any partition-size: through INFO responses";
    std::set<std::string> allowed{"ext4", "f2fs", "raw"};
    for (const auto p : parts) {
        EXPECT_GT(std::get<1>(p), 0);
        std::string type;
        std::string part(std::get<0>(p));
        EXPECT_EQ(fb->GetVar("partition-type:" + part, &type), SUCCESS)
                << "getvar:partition-type:" + part << " failed";
        EXPECT_NE(allowed.find(type), allowed.end()) << "getvar:partition-type:" + part << " was '"
                                                     << type << "' this is not a valid type";
    }
}

TEST_F(Conformance, Slots) {
    std::string var;
    ASSERT_EQ(fb->GetVar("slot-count", &var), SUCCESS) << "getvar:slot-count failed";
    ASSERT_EQ(std::count_if(var.begin(), var.end(), isdigit), var.size())
            << "'" << var << "' is not all digits which it should be for getvar:slot-count";
    int32_t num_slots = strtol(var.c_str(), nullptr, 10);

    // Can't run out of alphabet letters...
    ASSERT_LE(num_slots, 26) << "What?! You can't have more than 26 slots";

    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed";

    std::map<std::string, std::set<char>> part_slots;
    if (num_slots > 0) {
        EXPECT_EQ(fb->GetVar("current-slot", &var), SUCCESS) << "getvar:current-slot failed";

        for (const auto p : parts) {
            std::string part(std::get<0>(p));
            std::regex reg("([[:graph:]]*)_([[:lower:]])");
            std::smatch sm;

            if (std::regex_match(part, sm, reg)) {  // This partition has slots
                std::string part_base(sm[1]);
                std::string slot(sm[2]);
                EXPECT_EQ(fb->GetVar("has-slot:" + part_base, &var), SUCCESS)
                        << "'getvar:has-slot:" << part_base << "' failed";
                EXPECT_EQ(var, "yes") << "'getvar:has-slot:" << part_base << "' was not 'yes'";
                EXPECT_TRUE(islower(slot.front()))
                        << "'" << slot.front() << "' is an invalid slot-suffix for " << part_base;
                std::set<char> tmp{slot.front()};
                part_slots.emplace(part_base, tmp);
                part_slots.at(part_base).insert(slot.front());
            } else {
                EXPECT_EQ(fb->GetVar("has-slot:" + part, &var), SUCCESS)
                        << "'getvar:has-slot:" << part << "' failed";
                EXPECT_EQ(var, "no") << "'getvar:has-slot:" << part << "' should be no";
            }
        }
        // Ensure each partition has the correct slot suffix
        for (const auto iter : part_slots) {
            const std::set<char>& char_set = iter.second;
            std::string chars;
            for (char c : char_set) {
                chars += c;
                chars += ',';
            }
            EXPECT_EQ(char_set.size(), num_slots)
                    << "There should only be slot suffixes from a to " << 'a' + num_slots - 1
                    << " instead encountered: " << chars;
            for (const char c : char_set) {
                EXPECT_GE(c, 'a') << "Encountered invalid slot suffix of '" << c << "'";
                EXPECT_LT(c, 'a' + num_slots) << "Encountered invalid slot suffix of '" << c << "'";
            }
        }
    }
}

TEST_F(Conformance, LockAndUnlockPrompt) {
    std::string resp;
    ASSERT_EQ(fb->GetVar("unlocked", &resp), SUCCESS) << "getvar:unlocked failed";
    ASSERT_TRUE(resp == "yes" || resp == "no")
            << "Device did not respond with 'yes' or 'no' for getvar:unlocked";
    bool curr = resp == "yes";

    for (int i = 0; i < 2; i++) {
        std::string action = !curr ? "unlock" : "lock";
        printf("Device should prompt to '%s' bootloader, select 'no'\n", action.c_str());
        ChangeLockState(!curr, false);
        ASSERT_EQ(fb->GetVar("unlocked", &resp), SUCCESS) << "getvar:unlocked failed";
        ASSERT_EQ(resp, curr ? "yes" : "no") << "The locked/unlocked state of the bootloader "
                                                "incorrectly changed after selecting no";
        printf("Device should prompt to '%s' bootloader, select 'yes'\n", action.c_str());
        ChangeLockState(!curr, true);
        ASSERT_EQ(fb->GetVar("unlocked", &resp), SUCCESS) << "getvar:unlocked failed";
        ASSERT_EQ(resp, !curr ? "yes" : "no") << "The locked/unlocked state of the bootloader "
                                                 "failed to change after selecting yes";
        curr = !curr;
    }
}

TEST_F(UnlockPermissions, Download) {
    std::vector<char> buf{'a', 'o', 's', 'p'};
    EXPECT_EQ(fb->Download(buf), SUCCESS) << "getvar:all failed in locked mode";
    ;
}

TEST_F(UnlockPermissions, DownloadFlash) {
    std::vector<char> buf{'a', 'o', 's', 'p'};
    EXPECT_EQ(fb->Download(buf), SUCCESS) << "Download failed in unlocked mode";
    ;
    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed in unlocked mode";
}

TEST_F(LockPermissions, DownloadFlash) {
    std::vector<char> buf{'a', 'o', 's', 'p'};
    EXPECT_EQ(fb->Download(buf), SUCCESS) << "Download failed in locked mode";
    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed in locked mode";
    std::string resp;
    for (const auto tup : parts) {
        EXPECT_EQ(fb->Flash(std::get<0>(tup), &resp), DEVICE_FAIL)
                << "Device did not respond with FAIL when trying to flash '" << std::get<0>(tup)
                << "' in locked mode";
        EXPECT_GT(resp.size(), 0)
                << "Device sent empty error message after FAIL";  // meaningful error message
    }
}

TEST_F(LockPermissions, Erase) {
    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed";
    std::string resp;
    for (const auto tup : parts) {
        EXPECT_EQ(fb->Erase(std::get<0>(tup), &resp), DEVICE_FAIL)
                << "Device did not respond with FAIL when trying to erase '" << std::get<0>(tup)
                << "' in locked mode";
        EXPECT_GT(resp.size(), 0) << "Device sent empty error message after FAIL";
    }
}

TEST_F(LockPermissions, SetActive) {
    std::vector<std::tuple<std::string, uint32_t>> parts;
    EXPECT_EQ(fb->Partitions(&parts), SUCCESS) << "getvar:all failed";

    std::string resp;
    EXPECT_EQ(fb->GetVar("slot-count", &resp), SUCCESS) << "getvar:slot-count failed";
    int32_t num_slots = strtol(resp.c_str(), nullptr, 10);

    for (const auto tup : parts) {
        std::string part(std::get<0>(tup));
        std::regex reg("([[:graph:]]*)_([[:lower:]])");
        std::smatch sm;

        if (std::regex_match(part, sm, reg)) {  // This partition has slots
            std::string part_base(sm[1]);
            for (char c = 'a'; c < 'a' + num_slots; c++) {
                // We should not be able to SetActive any of these
                EXPECT_EQ(fb->SetActive(part_base + '_' + c, &resp), DEVICE_FAIL)
                        << "set:active:" << part_base + '_' + c << " did not fail in locked mode";
            }
        }
    }
}

TEST_F(LockPermissions, Boot) {
    std::vector<char> buf;
    buf.resize(1000);
    EXPECT_EQ(fb->Download(buf), SUCCESS) << "A 1000 byte download failed";
    std::string resp;
    ASSERT_EQ(fb->Boot(&resp), DEVICE_FAIL)
            << "The device did not respond with failure for 'boot' when locked";
    EXPECT_GT(resp.size(), 0) << "No error message was returned by device after FAIL";
}

TEST_F(Fuzz, DownloadSize) {
    std::string var;
    EXPECT_EQ(fb->GetVar("max-download-size", &var), SUCCESS) << "getvar:max-download-size failed";
    int64_t size = strtoll(var.c_str(), nullptr, 0);
    EXPECT_GT(size, 0) << '\'' << var << "' is not a valid response for getvar:max-download-size";

    EXPECT_EQ(DownloadCommand(size + 1), DEVICE_FAIL)
            << "Device reported max-download-size as '" << size
            << "' but did not reject a download of " << size + 1;

    std::vector<char> buf(size);
    EXPECT_EQ(fb->Download(buf), SUCCESS) << "Device reported max-download-size as '" << size
                                          << "' but downloading a payload of this size failed";
    ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
}

TEST_F(Fuzz, DownloadLargerBuf) {
    std::vector<char> buf{'a', 'o', 's', 'p'};
    ASSERT_EQ(DownloadCommand(buf.size() - 1), SUCCESS)
            << "Download command for " << buf.size() - 1 << " bytes failed";
    // There are two ways to handle this
    // Accept download, but send error response
    // Reject the download outright
    std::string resp;
    RetCode ret = SendBuffer(buf);
    EXPECT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
    if (ret == SUCCESS) {
        // If it accepts the buffer, it better send back an error response
        EXPECT_EQ(HandleResponse(&resp), DEVICE_FAIL)
                << "After sending too small of a payload for a download command, device accepted "
                   "payload and did not respond with FAIL";
        ;
    } else {
        EXPECT_EQ(ret, IO_ERROR) << "After sending too small of a payload for a download command, "
                                    "device did not return error";
    }

    ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
    // The device better still work after all that if we unplug and replug
    EXPECT_EQ(transport->Reset(), 0) << "USB reset failed";
    EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS) << "getvar:product failed";
}

TEST_F(Fuzz, DownloadOverRun) {
    std::vector<char> buf(1000, 'F');
    ASSERT_EQ(DownloadCommand(10), SUCCESS) << "Device rejected download request for 10 bytes";
    // There are two ways to handle this
    // Accept download, but send error response
    // Reject the download outright
    std::string resp;
    RetCode ret = SendBuffer(buf);
    if (ret == SUCCESS) {
        // If it accepts the buffer, it better send back an error response
        EXPECT_EQ(HandleResponse(&resp), DEVICE_FAIL)
                << "After sending too large of a payload for a download command, device accepted "
                   "payload and did not respond with FAIL";
    } else {
        EXPECT_EQ(ret, IO_ERROR) << "After sending too large of a payload for a download command, "
                                    "device did not return error";
    }

    ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
    // The device better still work after all that if we unplug and replug
    EXPECT_EQ(transport->Reset(), 0) << "USB reset failed";
    EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS)
            << "Device did not respond with SUCCESS to getvar:product.";
}

TEST_F(Fuzz, DownloadInvalid1) {
    EXPECT_EQ(DownloadCommand(0), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command 'download:0'";
}

TEST_F(Fuzz, DownloadInvalid2) {
    std::string cmd("download:1");
    EXPECT_EQ(fb->RawCommand("download:1"), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid3) {
    std::string cmd("download:-1");
    EXPECT_EQ(fb->RawCommand("download:-1"), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid4) {
    std::string cmd("download:-01000000");
    EXPECT_EQ(fb->RawCommand(cmd), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid5) {
    std::string cmd("download:-0100000");
    EXPECT_EQ(fb->RawCommand(cmd), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid6) {
    std::string cmd("download:");
    EXPECT_EQ(fb->RawCommand(cmd), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid7) {
    std::string cmd("download:01000000\0999", sizeof("download:01000000\0999"));
    EXPECT_EQ(fb->RawCommand(cmd), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, DownloadInvalid8) {
    std::string cmd("download:01000000\0dkjfvijafdaiuybgidabgybr",
                    sizeof("download:01000000\0dkjfvijafdaiuybgidabgybr"));
    EXPECT_EQ(fb->RawCommand(cmd), DEVICE_FAIL)
            << "Device did not respond with FAIL for malformed download command '" << cmd << "'";
}

TEST_F(Fuzz, GetVarAllSpam) {
    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed;
    unsigned i = 1;
    do {
        std::vector<std::string> vars;
        ASSERT_EQ(fb->GetVarAll(&vars), SUCCESS) << "Device did not respond with success after "
                                                 << i << "getvar:all commands in a row";
        ASSERT_GT(vars.size(), 0)
                << "Device did not send any INFO responses after getvar:all command";
        elapsed = std::chrono::high_resolution_clock::now() - start;
    } while (i++, elapsed.count() < 5);
}

TEST_F(Fuzz, BadCommandTooLarge) {
    std::string s1 = RandomString(1000, rand_legal);
    EXPECT_EQ(fb->RawCommand(s1), DEVICE_FAIL)
            << "Device did not respond with failure after sending length " << s1.size()
            << " string of random ASCII chars";
    std::string s2 = RandomString(1000, rand_illegal);
    EXPECT_EQ(fb->RawCommand(s2), DEVICE_FAIL)
            << "Device did not respond with failure after sending length " << s1.size()
            << " string of random non-ASCII chars";
    std::string s3 = RandomString(1000, rand_char);
    EXPECT_EQ(fb->RawCommand(s3), DEVICE_FAIL)
            << "Device did not respond with failure after sending length " << s1.size()
            << " string of random chars";
}

TEST_F(Fuzz, CommandTooLarge) {
    for (const std::string& s : CMDS) {
        std::string rs = RandomString(1000, rand_char);
        EXPECT_EQ(fb->RawCommand(s + rs), DEVICE_FAIL)
                << "Device did not respond with failure after '" << s + rs << "'";
        ASSERT_TRUE(UsbStillAvailible()) << USB_PORT_GONE;
        std::string resp;
        EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS)
                << "Device is unresponsive to getvar command";
    }
}

TEST_F(Fuzz, CommandMissingArgs) {
    for (const std::string& s : CMDS) {
        if (s.back() == ':') {
            EXPECT_EQ(fb->RawCommand(s), DEVICE_FAIL)
                    << "Device did not respond with failure after '" << s << "'";
            std::string sub(s.begin(), s.end() - 1);
            EXPECT_EQ(fb->RawCommand(sub), DEVICE_FAIL)
                    << "Device did not respond with failure after '" << sub << "'";
        } else {
            std::string rs = RandomString(10, rand_illegal);
            EXPECT_EQ(fb->RawCommand(rs + s), DEVICE_FAIL)
                    << "Device did not respond with failure after '" << rs + s << "'";
        }
        std::string resp;
        EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS)
                << "Device is unresponsive to getvar command";
    }
}

TEST_F(Fuzz, USBResetSpam) {
    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed;
    int i = 0;
    do {
        ASSERT_EQ(transport->Reset(), 0) << "USB Reset failed after " << i << " resets in a row";
        elapsed = std::chrono::high_resolution_clock::now() - start;
    } while (i++, elapsed.count() < 5);
    std::string resp;
    EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS)
            << "getvar failed after " << i << " USB reset(s) in a row";
}

TEST_F(Fuzz, USBResetCommandSpam) {
    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed;
    do {
        std::string resp;
        std::vector<std::string> all;
        ASSERT_EQ(transport->Reset(), 0) << "USB Reset failed";
        EXPECT_EQ(fb->GetVarAll(&all), SUCCESS) << "getvar:all failed after USB reset";
        EXPECT_EQ(fb->GetVar("product", &resp), SUCCESS) << "getvar:product failed";
        elapsed = std::chrono::high_resolution_clock::now() - start;
    } while (elapsed.count() < 10);
}

TEST_F(Fuzz, USBResetAfterDownload) {
    std::vector<char> buf;
    buf.resize(1000000);
    EXPECT_EQ(DownloadCommand(buf.size()), SUCCESS) << "Download command failed";
    EXPECT_EQ(transport->Reset(), 0) << "USB Reset failed";
    std::vector<std::string> all;
    EXPECT_EQ(fb->GetVarAll(&all), SUCCESS) << "getvar:all failed after USB reset.";
}

}  // namespace fastboot

int ConfigureSerial(const std::string& port) {
    int fd = open(port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);

    if (fd <= 0) {
        return fd;
    }

    struct termios tty;
    tcgetattr(fd, &tty);

    cfsetospeed(&tty, (speed_t)B115200);
    cfsetispeed(&tty, (speed_t)B115200);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 2;
    tty.c_cflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    cfmakeraw(&tty);

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return -1;
    }

    return fd;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);  // no buffering
    printf("<Waiting for Device>\n");
    const auto matcher = [](usb_ifc_info* info) -> int {
        return fastboot::FastBootTest::MatchFastboot(info, nullptr);
    };
    Transport* transport = nullptr;
    while (!transport) {
        transport = usb_open(matcher);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    transport->Close();

    // TODO add command line argv parsing and don't hardcode
    std::string serial_port = "/dev/ttyUSB0";
    fastboot::FastBootTest::serial_port = ConfigureSerial(serial_port);

    ::testing::InitGoogleTest(&argc, argv);
    auto ret = RUN_ALL_TESTS();
    if (fastboot::FastBootTest::serial_port > 0) {
        close(fastboot::FastBootTest::serial_port);
    }
    return ret;
}
