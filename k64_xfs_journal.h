#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "k64_xfs_format.h"

struct k64_xfs_mount;

bool k64_xfs_journal_recover(struct k64_xfs_mount* fs);
bool k64_xfs_journal_begin(struct k64_xfs_mount* fs);
bool k64_xfs_journal_metadata(struct k64_xfs_mount* fs, uint64_t block_no, const void* block);
bool k64_xfs_journal_commit(struct k64_xfs_mount* fs);
