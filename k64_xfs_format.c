#include "k64_xfs_format.h"
#include "k64_string.h"

uint32_t k64_xfs_crc32(const void* data, uint32_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t k64_xfs_super_checksum(const k64_xfs_superblock_disk_t* sb) {
    k64_xfs_superblock_disk_t tmp;

    if (!sb) {
        return 0;
    }
    memcpy(&tmp, sb, sizeof(tmp));
    tmp.checksum = 0;
    return k64_xfs_crc32(&tmp, sizeof(tmp));
}

uint32_t k64_xfs_inode_checksum(const k64_xfs_inode_disk_t* inode) {
    k64_xfs_inode_disk_t tmp;

    if (!inode) {
        return 0;
    }
    memcpy(&tmp, inode, sizeof(tmp));
    tmp.checksum = 0;
    return k64_xfs_crc32(&tmp, sizeof(tmp));
}

uint32_t k64_xfs_dirent_checksum(const k64_xfs_dirent_disk_t* dirent) {
    k64_xfs_dirent_disk_t tmp;

    if (!dirent) {
        return 0;
    }
    memcpy(&tmp, dirent, sizeof(tmp));
    tmp.checksum = 0;
    return k64_xfs_crc32(&tmp, sizeof(tmp));
}
