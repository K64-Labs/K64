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

The implementation treats all on-disk integers as fixed little-endian values in packed structures. v0.3.x only supports 4096-byte filesystem blocks, but the block size is still stored in the superblock so future incompatible versions can reject unsupported images cleanly instead of guessing.

The metadata regions are intentionally fixed in v1. That keeps the early checker, formatter, and GRUB reader simple:

1. Read block `0`.
2. Verify magic, version, block size, geometry, and checksum.
3. Locate the inode bitmap, block bitmap, journal, inode table, and data region from the superblock fields.
4. Validate that every region fits inside the device.
5. Verify that the root inode exists and is a directory.

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

An inode checksum covers the inode record with its checksum field cleared. The checksum is not meant to be cryptographic; it is a corruption detector for accidental metadata damage and malformed images. The checker treats bad inode magic, ID mismatch, or checksum mismatch as filesystem errors.

File data is described by direct extents. Each extent maps a logical block range inside the file to a physical block range in the K64XFS data area. A sparse-file model is not implemented yet: writes allocate real blocks, and reads expect extents to cover the requested data.

## Directories

Directories are stored as regular inode data containing fixed-size directory entries. v0.3.23 uses a linear directory scan. The on-disk format leaves room for indexed directories later.

Directory operations are intentionally conservative:

- path components must fit in the fixed entry name field
- empty components and traversal tricks are rejected by higher path-normalization layers
- entries point to inode IDs rather than physical offsets
- removing a file clears the directory entry and frees the inode/data blocks
- larger indexed-directory structures are future work

## Journaling

v0.3.23 includes the journal region, transaction IDs, dirty/clean mount state, and journal API skeleton. Metadata updates are routed through transaction begin/commit hooks, but full committed-record replay is still limited. The docs and release notes intentionally describe this as a crash-awareness foundation, not complete ext4-grade journaling.

The intended write ordering is:

1. Start a transaction.
2. Mark the filesystem dirty.
3. Journal metadata blocks that are about to change.
4. Write updated metadata through the cache.
5. Commit the transaction.
6. Flush dirty cache entries on sync/unmount.
7. Mark the filesystem clean after a successful sync.

Current code follows the transaction hooks for metadata mutation, but recovery remains a limited skeleton. If the mount state says the filesystem needs recovery, the mount path asks the journal layer to recover before the volume is considered clean. Do not document this as full crash safety until committed-record replay and crash simulation tests cover the common failure windows.

## Cache

K64XFS has a 64-entry 4 KiB block cache with dirty tracking and simple LRU eviction. `sync` and `unmount` flush dirty blocks.

The cache is deliberately small because K64 still targets compact VM test environments. Reads go through `k64_xfs_cache_read()`, writes go through `k64_xfs_cache_write()`, and metadata writers should journal the block before or while marking it dirty. Eviction is least-recently-used among valid entries; dirty entries must be flushed before reuse.

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

- K64XFS is the standard root filesystem for the normal build and release path, but it is still young.
- Only direct extents are implemented, capped at eight extents per file.
- Directories are linear.
- The checker is read-only and does not repair.
- The journal is a skeleton with clean/dirty state and transaction hooks, not full crash replay.
- Service-call routing uses the stable `k64_fs_*` facade over the mounted K64XFS root; a full VFS with multiple mounted filesystem kinds is future work.
- The implementation still needs broader malformed-image and crash-simulation coverage before it should be treated as production-grade.
