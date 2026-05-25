# K64XFS

K64XFS is the block-backed filesystem core introduced in K64 v0.3.23 and promoted to the only standard boot/root filesystem in v0.3.24. The old image-backed filesystem is no longer used by the normal build, boot path, release artifacts, or runtime root driver.

## Goals

- Use block allocation instead of whole-image rewrites.
- Persist mode, UID, GID, size, timestamps, generation, and extent metadata in inodes.
- Use extents for file data.
- Provide a superblock with versioning, feature flags, mount state, UUID, label, and CRC32 checksum.
- Provide a block cache and metadata-journal skeleton.
- Keep current service-call userland APIs and v0.3.22 permission checks intact.

## Non-Goals

v0.3.23 does not implement compression, encryption, deduplication, snapshots, subvolumes, ACLs, extended attributes, indexed directories, indirect extents, mmap file backing, or root migration.

## Layout

K64XFS v1 uses 4096-byte filesystem blocks. Block devices with smaller sectors are accessed in groups.

- block 0: superblock
- block 1: inode bitmap
- block 2: block bitmap
- blocks 3-10: journal area
- following blocks: fixed inode table
- remaining blocks: data area

The superblock stores the magic `K64XFS1`, version, block size, total/free block counts, metadata region locations, root inode, mount state, feature flags, UUID, label, generation, and checksum.

## Inodes

Each inode stores:

- inode ID and type
- POSIX-like mode bits
- UID and GID
- file size
- link count
- created, modified, and accessed ticks
- generation
- up to eight direct extents
- checksum

Supported inode types in v0.3.23 are regular files and directories. Symlink/device/pipe type values are reserved for future work.

## Directories

Directories are stored as regular inode data containing fixed-size directory entries. v0.3.23 uses a linear directory scan. The on-disk format leaves room for indexed directories later.

## Journaling

v0.3.23 includes the journal region, transaction IDs, dirty/clean mount state, and journal API skeleton. Metadata updates are routed through transaction begin/commit hooks, but full committed-record replay is still limited. The docs and release notes intentionally describe this as a crash-awareness foundation, not complete ext4-grade journaling.

## Cache

K64XFS has a 64-entry 4 KiB block cache with dirty tracking and simple LRU eviction. `sync` and `unmount` flush dirty blocks.

## Permissions

K64XFS persists UID, GID, and mode in inodes. The low-level `k64_xfs_*` API is a trusted kernel mechanism. User-visible enforcement belongs in service/VFS wrappers and command tooling, matching the v0.3.22 model:

- read file: read bit
- write file: write bit
- execute ELF: read and execute bits
- list directory: read and execute bits
- create/delete/rename: write and execute bits on the parent
- chmod/chown: root only in v0.3.23 tooling

## Tooling

`xfsctl` is the initial shell surface:

- `xfsctl devices`
- `xfsctl format <device> <label> yes`
- `xfsctl mount <device> /x`
- `xfsctl unmount /x`
- `xfsctl info`
- `xfsctl ls /x/path`
- `xfsctl stat /x/path`
- `xfsctl mkdir /x/path`
- `xfsctl write /x/path <text>`
- `xfsctl append /x/path <text>`
- `xfsctl cat /x/path`
- `xfsctl rm /x/path`
- `xfsctl rmdir /x/path`
- `xfsctl chmod <mode> /x/path`
- `xfsctl chown <user>:<group> /x/path`
- `xfsctl sync`
- `xfsctl check`

## Current Limits

- K64XFS is not the default root filesystem.
- `/` still boots from K64XFS LegacyFS/BootFS.
- Only direct extents are implemented, capped at eight extents per file.
- Directories are linear.
- The checker is read-only and does not repair.
- The journal is a skeleton with clean/dirty state and transaction hooks, not full crash replay.
- Service-call path routing to `/x` is future work; v0.3.23 exposes K64XFS through `xfsctl` and kernel APIs.
