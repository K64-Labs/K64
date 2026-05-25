#include "k64_xfs_journal.h"
#include "k64_xfs.h"

bool k64_xfs_journal_recover(struct k64_xfs_mount* fs) {
    if (!fs || !fs->mounted) {
        return false;
    }
    return true;
}

bool k64_xfs_journal_begin(struct k64_xfs_mount* fs) {
    if (!fs || !fs->mounted) {
        return false;
    }
    fs->tx_id++;
    fs->super.mount_state = K64_XFS_MOUNT_DIRTY;
    return true;
}

bool k64_xfs_journal_metadata(struct k64_xfs_mount* fs, uint64_t block_no, const void* block) {
    (void)block_no;
    (void)block;
    return fs && fs->mounted;
}

bool k64_xfs_journal_commit(struct k64_xfs_mount* fs) {
    if (!fs || !fs->mounted) {
        return false;
    }
    fs->super.generation++;
    return true;
}
