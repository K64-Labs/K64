#include "k64_xfs_journal.h"
#include "k64_xfs.h"
#include "k64_string.h"

static uint8_t journal_header_block[K64_XFS_BLOCK_SIZE];
static uint8_t journal_payload_block[K64_XFS_BLOCK_SIZE];

static uint32_t journal_header_checksum(const k64_xfs_journal_entry_disk_t* entry) {
    k64_xfs_journal_entry_disk_t tmp;

    if (!entry) {
        return 0;
    }
    memcpy(&tmp, entry, sizeof(tmp));
    tmp.header_checksum = 0;
    return k64_xfs_crc32(&tmp, sizeof(tmp));
}

static bool journal_block_write(k64_xfs_mount_t* fs, uint64_t block, const void* data) {
    uint32_t sectors;

    if (!fs || !fs->dev || !data || fs->dev->block_size == 0 ||
        K64_XFS_BLOCK_SIZE % fs->dev->block_size != 0 ||
        block > UINT64_MAX / (K64_XFS_BLOCK_SIZE / fs->dev->block_size)) {
        return false;
    }
    sectors = K64_XFS_BLOCK_SIZE / fs->dev->block_size;
    return k64_block_write(fs->dev, block * sectors, sectors, data);
}

static bool journal_block_read(k64_xfs_mount_t* fs, uint64_t block, void* out) {
    uint32_t sectors;

    if (!fs || !fs->dev || !out || fs->dev->block_size == 0 ||
        K64_XFS_BLOCK_SIZE % fs->dev->block_size != 0 ||
        block > UINT64_MAX / (K64_XFS_BLOCK_SIZE / fs->dev->block_size)) {
        return false;
    }
    sectors = K64_XFS_BLOCK_SIZE / fs->dev->block_size;
    return k64_block_read(fs->dev, block * sectors, sectors, out);
}

static bool journal_read_entry(k64_xfs_mount_t* fs,
                               uint64_t block,
                               k64_xfs_journal_entry_disk_t* out) {
    if (!fs || !out || !journal_block_read(fs, block, journal_header_block)) {
        return false;
    }
    memcpy(out, journal_header_block, sizeof(*out));
    if (out->magic != K64_XFS_JOURNAL_MAGIC ||
        out->header_checksum != journal_header_checksum(out)) {
        return false;
    }
    return true;
}

bool k64_xfs_journal_recover(struct k64_xfs_mount* fs) {
    k64_xfs_journal_entry_disk_t entry;
    k64_xfs_journal_entry_disk_t records[K64_XFS_JOURNAL_BLOCKS / 2u];
    uint32_t record_count = 0;
    bool committed = false;
    uint64_t tx_id = 0;

    if (!fs || !fs->mounted) {
        return false;
    }
    for (uint64_t off = 0; off < fs->super.journal_blocks; ) {
        uint64_t block = fs->super.journal_start + off;

        if (!journal_read_entry(fs, block, &entry)) {
            break;
        }
        if (tx_id == 0) {
            tx_id = entry.tx_id;
        }
        if (entry.tx_id != tx_id) {
            break;
        }
        if (entry.type == K64_XFS_JOURNAL_COMMIT) {
            committed = true;
            break;
        }
        if (entry.type != K64_XFS_JOURNAL_METADATA_BLOCK ||
            entry.payload_size != K64_XFS_BLOCK_SIZE ||
            off + 1u >= fs->super.journal_blocks ||
            record_count >= (uint32_t)(K64_XFS_JOURNAL_BLOCKS / 2u)) {
            break;
        }
        records[record_count++] = entry;
        off += 2u;
    }
    if (committed) {
        for (uint32_t i = 0; i < record_count; ++i) {
            uint64_t payload_block = fs->super.journal_start + (uint64_t)i * 2u + 1u;

            if (!journal_block_read(fs, payload_block, journal_payload_block) ||
                k64_xfs_crc32(journal_payload_block, K64_XFS_BLOCK_SIZE) != records[i].payload_checksum ||
                !journal_block_write(fs, records[i].target_block, journal_payload_block)) {
                return false;
            }
        }
    }
    memset(journal_header_block, 0, sizeof(journal_header_block));
    for (uint64_t off = 0; off < fs->super.journal_blocks; ++off) {
        if (!journal_block_write(fs, fs->super.journal_start + off, journal_header_block)) {
            return false;
        }
    }
    fs->journal_active = false;
    fs->journal_records = 0;
    fs->super.mount_state = K64_XFS_MOUNT_CLEAN;
    return true;
}

bool k64_xfs_journal_begin(struct k64_xfs_mount* fs) {
    if (!fs || !fs->mounted) {
        return false;
    }
    if (fs->journal_active) {
        return true;
    }
    fs->tx_id++;
    fs->journal_records = 0;
    fs->journal_active = true;
    fs->super.mount_state = K64_XFS_MOUNT_DIRTY;
    return true;
}

bool k64_xfs_journal_metadata(struct k64_xfs_mount* fs, uint64_t block_no, const void* block) {
    k64_xfs_journal_entry_disk_t entry;
    uint64_t header_block;
    uint64_t payload_block;

    if (!fs || !fs->mounted || !fs->journal_active || !block ||
        block_no >= fs->super.total_blocks) {
        return false;
    }
    header_block = fs->super.journal_start + (uint64_t)fs->journal_records * 2u;
    payload_block = header_block + 1u;
    if (payload_block >= fs->super.journal_start + fs->super.journal_blocks) {
        return false;
    }
    memset(journal_header_block, 0, sizeof(journal_header_block));
    memset(&entry, 0, sizeof(entry));
    entry.magic = K64_XFS_JOURNAL_MAGIC;
    entry.tx_id = fs->tx_id;
    entry.type = K64_XFS_JOURNAL_METADATA_BLOCK;
    entry.target_block = block_no;
    entry.payload_size = K64_XFS_BLOCK_SIZE;
    entry.payload_checksum = k64_xfs_crc32(block, K64_XFS_BLOCK_SIZE);
    entry.header_checksum = journal_header_checksum(&entry);
    memcpy(journal_header_block, &entry, sizeof(entry));
    if (!journal_block_write(fs, header_block, journal_header_block) ||
        !journal_block_write(fs, payload_block, block)) {
        return false;
    }
    fs->journal_records++;
    return true;
}

bool k64_xfs_journal_commit(struct k64_xfs_mount* fs) {
    k64_xfs_journal_entry_disk_t entry;
    uint64_t commit_block;

    if (!fs || !fs->mounted) {
        return false;
    }
    if (!fs->journal_active) {
        return true;
    }
    commit_block = fs->super.journal_start + (uint64_t)fs->journal_records * 2u;
    if (commit_block >= fs->super.journal_start + fs->super.journal_blocks) {
        return false;
    }
    memset(journal_header_block, 0, sizeof(journal_header_block));
    memset(&entry, 0, sizeof(entry));
    entry.magic = K64_XFS_JOURNAL_MAGIC;
    entry.tx_id = fs->tx_id;
    entry.type = K64_XFS_JOURNAL_COMMIT;
    entry.header_checksum = journal_header_checksum(&entry);
    memcpy(journal_header_block, &entry, sizeof(entry));
    if (!journal_block_write(fs, commit_block, journal_header_block)) {
        return false;
    }
    fs->super.generation++;
    fs->journal_active = false;
    fs->journal_records = 0;
    return true;
}
