#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <selinux/selinux.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <ext4_utils/ext4_utils.h>

#include <fs_mgr.h>
#include <fs_mgr_dm_linear.h>
#include <fstab/fstab.h>
#include <liblp/builder.h>
#include <storage_literals/storage_literals.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "scratch_super.h"

using namespace std::literals;
using namespace android::dm;
using namespace android::fs_mgr;
using namespace android::storage_literals;

namespace android {
namespace snapshot {

bool UmountScratch() {
    std::string dir = kScratchMount;
    auto ota_dir = dir + "/" + "ota";
    auto snapshot_dir = ota_dir + "/" + "snapshots";

    if (!std::filesystem::remove(snapshot_dir)) {
        PLOG(ERROR) << "Failed to remove snapshot directory";
        return false;
    }

    std::filesystem::remove_all(ota_dir);

    if (umount(kScratchMount.c_str()) != 0) {
        PLOG(ERROR) << "UmountScratch failed";
        return false;
    }

    LOG(INFO) << "umount scratch success";
    return true;
}

bool CleanupScratch() {
    if (!UmountScratch()) {
        LOG(ERROR) << "UmountScratch failed";
        return false;
    }

    std::string super_device;
    std::unique_ptr<MetadataBuilder> builder;
    const auto partition_name = android::base::Basename(kScratchMount);
    const std::vector<int> slots = {0, 1};
    for (auto slot : slots) {
        super_device = kPhysicalDevice + fs_mgr_get_super_partition_name(slot);
        if (access(super_device.c_str(), R_OK | W_OK)) {
            return false;
        }

        builder = MetadataBuilder::New(super_device, slot);
        if (!builder) {
            return false;
        }
        if (builder->FindPartition(partition_name) != nullptr) {
            builder->RemovePartition(partition_name);
            auto metadata = builder->Export();
            if (metadata && UpdatePartitionTable(super_device, *metadata.get(), slot)) {
                if (DestroyLogicalPartition(partition_name)) {
                    LOG(INFO) << "CleanupScratch success for slot: " << slot;
                }
            }
        }
    }

    return true;
}

bool SetupOTADirs() {
    std::string dir = kScratchMount;
    if (setfscreatecon(android::snapshot::kOtaFileContext.c_str())) {
        PLOG(ERROR) << "setfscreatecon failed: " << android::snapshot::kOtaFileContext;
        return false;
    }
    auto ota_dir = dir + "/" + "ota";
    if (mkdir(ota_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "mkdir " << ota_dir;
        return false;
    }

    auto snapshot_dir = ota_dir + "/" + "snapshots";
    if (mkdir(snapshot_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "mkdir " << snapshot_dir;
        return false;
    }
    if (setfscreatecon(nullptr)) {
        PLOG(ERROR) << "setfscreatecon null";
        return false;
    }
    return true;
}

bool MountScratch(const std::string& device_path) {
    if (access(device_path.c_str(), R_OK | W_OK)) {
        LOG(ERROR) << "Path does not exist or is not readwrite: " << device_path;
        return false;
    }

    std::vector<const char*> filesystem_candidates;
    if (fs_mgr_is_ext4(device_path)) {
        filesystem_candidates = {"ext4"};
    } else {
        LOG(ERROR) << "Scratch partition is not ext4";
        return false;
    }
    if (setfscreatecon(android::snapshot::kOtaFileContext.c_str())) {
        PLOG(ERROR) << "setfscreatecon failed: " << android::snapshot::kOtaFileContext;
        return false;
    }
    if (mkdir(kScratchMount.c_str(), 0755) && (errno != EEXIST)) {
        PLOG(ERROR) << "create " << kScratchMount;
        return false;
    }

    android::fs_mgr::FstabEntry entry;
    entry.blk_device = device_path;
    entry.mount_point = kScratchMount;
    entry.flags = MS_NOATIME;
    entry.flags |= MS_SYNCHRONOUS;
    entry.fs_options = "nodiscard";
    fs_mgr_set_blk_ro(device_path, false);
    entry.fs_mgr_flags.check = true;

    bool mounted = false;
    for (auto fs_type : filesystem_candidates) {
        entry.fs_type = fs_type;
        if (fs_mgr_do_mount_one(entry) == 0) {
            mounted = true;
            break;
        }
    }
    if (setfscreatecon(nullptr)) {
        PLOG(ERROR) << "setfscreatecon null";
        return false;
    }
    if (!mounted) {
        rmdir(kScratchMount.c_str());
        return false;
    }

    return true;
}

bool MakeScratchFilesystem(const std::string& scratch_device) {
    auto fs_type = ""s;
    auto command = ""s;
    if (!access(kMkExt4.c_str(), X_OK)) {
        fs_type = "ext4";
        command = kMkExt4 + " -F -b 4096 -t ext4 -m 0 -O has_journal -M "s + kScratchMount;
    } else {
        LOG(ERROR) << "No supported mkfs command or filesystem driver available, supported "
                      "filesystems "
                      "are: f2fs, ext4";
        return false;
    }
    command += " " + scratch_device + " >/dev/null 2>/dev/null </dev/null";
    fs_mgr_set_blk_ro(scratch_device, false);
    auto ret = system(command.c_str());
    if (ret) {
        LOG(ERROR) << "make " << fs_type << " filesystem on " << scratch_device
                   << " return=" << ret;
        return false;
    }
    return true;
}

bool CreateDynamicScratch(std::string* scratch_device, size_t size, int slot) {
    const auto partition_name = android::base::Basename(kScratchMount);
    auto& dm = DeviceMapper::Instance();

    bool partition_exists = dm.GetState(partition_name) != DmDeviceState::INVALID;
    if (partition_exists) {
        LOG(ERROR) << "Partition already exists: " << partition_name;
        return false;
    }

    auto target_slot_number = slot;
    const auto super_device = kPhysicalDevice + fs_mgr_get_super_partition_name(target_slot_number);
    auto builder = MetadataBuilder::New(super_device, target_slot_number);
    if (!builder) {
        LOG(ERROR) << "open " << super_device << " metadata";
        return false;
    }
    auto partition = builder->FindPartition(partition_name);
    partition_exists = partition != nullptr;
    if (partition_exists) {
        LOG(ERROR) << "Partition exists in super metadata";
        return false;
    }

    partition = builder->AddPartition(partition_name, LP_PARTITION_ATTR_NONE);
    if (!partition) {
        PLOG(ERROR) << "AddPartition failed " << partition_name;
        return false;
    }

    auto free_space = builder->AllocatableSpace() - builder->UsedSpace();
    if (free_space < size) {
        PLOG(ERROR) << "No space in super partition. Free space: " << free_space
                    << " Requested space: " << size;
        return false;
    }

    LOG(INFO) << "CreateDynamicScratch: free_space: " << free_space << " scratch_size: " << size
              << " slot_number: " << target_slot_number;

    if (!builder->ResizePartition(partition, size)) {
        LOG(ERROR) << "ResizePartition failed: " << partition_name << " free_space: " << free_space
                   << " scratch_size: " << size;
        return false;
    }

    auto metadata = builder->Export();
    if (!metadata || !UpdatePartitionTable(super_device, *metadata.get(), target_slot_number)) {
        LOG(ERROR) << "UpdatePartitionTable failed: " << partition_name;
        return false;
    }

    CreateLogicalPartitionParams params = {
            .block_device = super_device,
            .metadata_slot = target_slot_number,
            .partition_name = partition_name,
            .force_writable = true,
            .timeout_ms = 10s,
    };

    if (!CreateLogicalPartition(params, scratch_device)) {
        LOG(ERROR) << "CreateLogicalPartition failed";
        return false;
    }

    LOG(INFO) << "Scratch device created successfully: " << *scratch_device
              << " slot: " << target_slot_number;
    return true;
}

bool IsScratchPresent() {
    auto partition_name = android::base::Basename(kScratchMount);
    auto source_slot = fs_mgr_get_slot_suffix();
    auto source_slot_number = SlotNumberForSlotSuffix(source_slot);

    const auto super_device =
            kPhysicalDevice + fs_mgr_get_super_partition_name(!source_slot_number);
#if 0
    auto builder = MetadataBuilder::New(super_device, source_slot_number);
    if (!builder) {
        LOG(ERROR) << "open " << super_device << " metadata" << "slot: " << source_slot_number;
        return false;
    }
    auto partition = builder->FindPartition(partition_name);
    bool partition_exists = partition != nullptr;
    if (!partition_exists) {
        LOG(ERROR) << "Partition: " << partition_name << " not present on slot: " << source_slot_number
                   << " source_slot: " << source_slot
                   << " super_device: " << super_device;
        return false;
    }
#endif

#if 1
    // const auto super_device = kPhysicalDevice +
    // fs_mgr_get_super_partition_name(source_slot_number);
    auto metadata = android::fs_mgr::ReadCurrentMetadata(super_device);
    if (!metadata) {
        LOG(ERROR) << "Invalid super metadata: " << super_device;
        return false;
    }
    auto partition = android::fs_mgr::FindPartition(*metadata.get(), partition_name);
    if (!partition) {
        LOG(ERROR) << "Partition: " << partition_name
                   << " not present on slot: " << source_slot_number
                   << " source_slot: " << source_slot << " super_device: " << super_device;
        return false;
    }
#endif
    /*
        CreateLogicalPartitionParams params = {
          .block_device = super_device,
          .metadata_slot = source_slot_number,
          .partition_name = partition_name,
          .force_writable = true,
          .timeout_ms = 10s,
        };
    */

    CreateLogicalPartitionParams params = {
            .block_device = super_device,
            .metadata = metadata.get(),
            .partition = partition,
    };
    /*
        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata_slot = source_slot_number,
                .partition_name = partition_name,
                //.partition = partition,
        };
    */
    std::string scratch_path;
    if (!CreateLogicalPartition(params, &scratch_path)) {
        LOG(ERROR) << "Could not create logical partition: " << partition_name;
        return false;
    }

    LOG(INFO) << "CreateLogicalPartition success: " << scratch_path
              << " slot: " << !source_slot_number;
    return true;
}

std::string GetScratchDevice() {
    std::string device;
    auto& dm = DeviceMapper::Instance();
    auto partition_name = android::base::Basename(kScratchMount);

    bool invalid_partition = (dm.GetState(partition_name) == DmDeviceState::INVALID);
    if (!invalid_partition && dm.GetDmDevicePathByName(partition_name, &device)) {
        LOG(INFO) << "ScratchDevice: " << device;
        return device;
    }
    return "";
}

static bool ScratchAlreadyMounted(const std::string& mount_point) {
    android::fs_mgr::Fstab fstab;
    if (!ReadFstabFromProcMounts(&fstab)) {
        return false;
    }
    for (const auto& entry : GetEntriesForMountPoint(&fstab, mount_point)) {
        if (entry->fs_type == "ext4") {
            return true;
        }
    }
    return false;
}

std::string MapScratchDevice(std::string scratch_device) {
    if (!ScratchAlreadyMounted(kScratchMount)) {
        if (!MountScratch(scratch_device)) {
            return "";
        }
    }

    auto ota_dir = kScratchMount + "/" + "ota";
    if (access(ota_dir.c_str(), F_OK) != 0) {
        return "";
    }
    return ota_dir;
}

}  // namespace snapshot
}  // namespace android
