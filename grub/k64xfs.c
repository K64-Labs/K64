#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/mm.h>
#include <grub/misc.h>

GRUB_MOD_LICENSE ("GPLv3+");
GRUB_MOD_NAME (k64xfs);

#define K64XFS_MAGIC "K64XFS1"
#define K64XFS_MAGIC_SIZE 7
#define K64XFS_BLOCK_SIZE 4096U
#define K64XFS_MAX_INODES 128U
#define K64XFS_DIRECT_EXTENTS 8U
#define K64XFS_NAME_MAX 128U
#define K64XFS_INODES_PER_BLOCK 11U
#define K64XFS_DIRENTS_PER_BLOCK 25U
#define K64XFS_INODE_MAGIC 0x58494E4FU
#define K64XFS_TYPE_REGULAR 1U
#define K64XFS_TYPE_DIRECTORY 2U

struct grub_k64xfs_extent
{
  grub_uint64_t logical_block;
  grub_uint64_t physical_block;
  grub_uint64_t block_count;
  grub_uint32_t flags;
  grub_uint32_t reserved;
} GRUB_PACKED;

struct grub_k64xfs_inode
{
  grub_uint32_t magic;
  grub_uint32_t inode_id;
  grub_uint16_t type;
  grub_uint16_t mode;
  grub_uint32_t uid;
  grub_uint32_t gid;
  grub_uint64_t size;
  grub_uint32_t link_count;
  grub_uint32_t flags;
  grub_uint64_t created_tick;
  grub_uint64_t modified_tick;
  grub_uint64_t accessed_tick;
  grub_uint64_t generation;
  grub_uint32_t extent_count;
  grub_uint32_t reserved0;
  struct grub_k64xfs_extent direct_extents[K64XFS_DIRECT_EXTENTS];
  grub_uint64_t indirect_extent_block;
  grub_uint32_t checksum;
  grub_uint8_t reserved1[28];
} GRUB_PACKED;

struct grub_k64xfs_dirent
{
  grub_uint32_t inode_id;
  grub_uint16_t type;
  grub_uint16_t name_len;
  char name[K64XFS_NAME_MAX];
  grub_uint32_t checksum;
  grub_uint8_t reserved[20];
} GRUB_PACKED;

struct grub_k64xfs_super
{
  char magic[8];
  grub_uint16_t version_major;
  grub_uint16_t version_minor;
  grub_uint32_t block_size;
  grub_uint64_t total_blocks;
  grub_uint64_t free_blocks;
  grub_uint64_t inode_table_start;
  grub_uint64_t inode_table_blocks;
  grub_uint64_t block_bitmap_start;
  grub_uint64_t block_bitmap_blocks;
  grub_uint64_t inode_bitmap_start;
  grub_uint64_t inode_bitmap_blocks;
  grub_uint64_t journal_start;
  grub_uint64_t journal_blocks;
  grub_uint64_t data_start;
  grub_uint32_t root_inode;
  grub_uint32_t mount_state;
  grub_uint64_t feature_flags_compat;
  grub_uint64_t feature_flags_ro_compat;
  grub_uint64_t feature_flags_incompat;
  grub_uint8_t uuid[16];
  char label[32];
  grub_uint64_t generation;
  grub_uint32_t checksum;
  grub_uint8_t reserved[3784];
} GRUB_PACKED;

struct grub_k64xfs_mount
{
  grub_disk_t disk;
  struct grub_k64xfs_super super;
};

struct grub_k64xfs_file
{
  struct grub_k64xfs_mount *mount;
  struct grub_k64xfs_inode inode;
};

static grub_uint8_t grub_k64xfs_block_buf[K64XFS_BLOCK_SIZE];

static void
grub_k64xfs_copy (void *dst, const void *src, grub_size_t size)
{
  char *d = dst;
  const char *s = src;

  while (size-- > 0)
    *d++ = *s++;
}

static void
grub_k64xfs_zero (void *dst, grub_size_t size)
{
  char *d = dst;

  while (size-- > 0)
    *d++ = 0;
}

static int
grub_k64xfs_cmp (const void *a, const void *b, grub_size_t size)
{
  const unsigned char *pa = a;
  const unsigned char *pb = b;

  while (size-- > 0)
    {
      if (*pa != *pb)
        return (int) *pa - (int) *pb;
      pa++;
      pb++;
    }
  return 0;
}

static grub_size_t
grub_k64xfs_strlen (const char *s)
{
  grub_size_t len = 0;

  while (s && s[len] != '\0')
    len++;
  return len;
}

static char *
grub_k64xfs_strdup_local (const char *s)
{
  grub_size_t len = grub_k64xfs_strlen (s);
  char *copy = grub_malloc (len + 1);

  if (!copy)
    return NULL;
  grub_k64xfs_copy (copy, s, len + 1);
  return copy;
}

static grub_err_t
grub_k64xfs_read (grub_disk_t disk, grub_uint64_t offset, grub_size_t size, void *buf)
{
  grub_disk_addr_t sector = (grub_disk_addr_t) (offset >> GRUB_DISK_SECTOR_BITS);
  grub_off_t sector_offset = (grub_off_t) (offset & (GRUB_DISK_SECTOR_SIZE - 1));

  return grub_disk_read (disk, sector, sector_offset, size, buf);
}

static grub_err_t
grub_k64xfs_read_block (struct grub_k64xfs_mount *mount, grub_uint64_t block, void *buf)
{
  return grub_k64xfs_read (mount->disk, block * K64XFS_BLOCK_SIZE, K64XFS_BLOCK_SIZE, buf);
}

static struct grub_k64xfs_mount *
grub_k64xfs_mount (grub_disk_t disk)
{
  struct grub_k64xfs_mount *mount = grub_zalloc (sizeof (*mount));

  if (!mount)
    return NULL;

  mount->disk = disk;
  if (grub_k64xfs_read (disk, 0, sizeof (mount->super), &mount->super))
    goto fail;

  if (grub_k64xfs_cmp (mount->super.magic, K64XFS_MAGIC, K64XFS_MAGIC_SIZE) != 0
      || mount->super.version_major != 1
      || mount->super.block_size != K64XFS_BLOCK_SIZE
      || mount->super.root_inode == 0
      || mount->super.inode_table_start == 0
      || mount->super.data_start >= mount->super.total_blocks)
    {
      grub_error (GRUB_ERR_BAD_FS, "not a k64xfs filesystem");
      goto fail;
    }

  return mount;

fail:
  grub_free (mount);
  return NULL;
}

static void
grub_k64xfs_mount_free (struct grub_k64xfs_mount *mount)
{
  grub_free (mount);
}

static grub_err_t
grub_k64xfs_read_inode (struct grub_k64xfs_mount *mount,
                        grub_uint32_t inode_id,
                        struct grub_k64xfs_inode *inode)
{
  grub_uint32_t index;
  grub_uint64_t table_block;
  grub_uint32_t offset;

  if (!mount || !inode || inode_id == 0 || inode_id > K64XFS_MAX_INODES)
    return grub_error (GRUB_ERR_BAD_FS, "invalid k64xfs inode");

  index = inode_id - 1;
  table_block = mount->super.inode_table_start + index / K64XFS_INODES_PER_BLOCK;
  offset = (index % K64XFS_INODES_PER_BLOCK) * sizeof (*inode);

  if (grub_k64xfs_read_block (mount, table_block, grub_k64xfs_block_buf))
    return grub_errno;

  grub_k64xfs_copy (inode, grub_k64xfs_block_buf + offset, sizeof (*inode));
  if (inode->magic != K64XFS_INODE_MAGIC || inode->inode_id != inode_id)
    return grub_error (GRUB_ERR_BAD_FS, "bad k64xfs inode");

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_k64xfs_inode_read_block (struct grub_k64xfs_mount *mount,
                              const struct grub_k64xfs_inode *inode,
                              grub_uint64_t logical,
                              void *buf)
{
  grub_uint32_t i;

  for (i = 0; i < inode->extent_count && i < K64XFS_DIRECT_EXTENTS; i++)
    {
      const struct grub_k64xfs_extent *ex = &inode->direct_extents[i];
      if (logical >= ex->logical_block && logical < ex->logical_block + ex->block_count)
        return grub_k64xfs_read_block (mount, ex->physical_block + logical - ex->logical_block, buf);
    }

  return grub_error (GRUB_ERR_BAD_FS, "missing k64xfs extent");
}

static grub_int32_t
grub_k64xfs_find_child (struct grub_k64xfs_mount *mount,
                        const struct grub_k64xfs_inode *dir,
                        const char *name,
                        struct grub_k64xfs_dirent *out)
{
  grub_uint64_t blocks;
  grub_uint64_t b;
  grub_size_t name_len;

  if (!dir || dir->type != K64XFS_TYPE_DIRECTORY)
    return -1;

  name_len = grub_k64xfs_strlen (name);
  blocks = (dir->size + K64XFS_BLOCK_SIZE - 1) >> 12;
  for (b = 0; b < blocks; b++)
    {
      struct grub_k64xfs_dirent *entries;
      grub_uint32_t s;

      if (grub_k64xfs_inode_read_block (mount, dir, b, grub_k64xfs_block_buf))
        return -1;

      entries = (struct grub_k64xfs_dirent *) grub_k64xfs_block_buf;
      for (s = 0; s < K64XFS_DIRENTS_PER_BLOCK; s++)
        {
          if (entries[s].inode_id == 0 || entries[s].name_len == 0)
            continue;
          if (entries[s].name_len == name_len
              && grub_k64xfs_cmp (entries[s].name, name, name_len) == 0)
            {
              if (out)
                grub_k64xfs_copy (out, &entries[s], sizeof (*out));
              return (grub_int32_t) entries[s].inode_id;
            }
        }
    }

  return -1;
}

static grub_err_t
grub_k64xfs_resolve (struct grub_k64xfs_mount *mount,
                     const char *path,
                     struct grub_k64xfs_inode *inode)
{
  char *scratch;
  char *token;
  char *next;
  struct grub_k64xfs_inode current;

  if (grub_k64xfs_read_inode (mount, mount->super.root_inode, &current))
    return grub_errno;

  if (!path || !*path || (path[0] == '/' && path[1] == '\0'))
    {
      grub_k64xfs_copy (inode, &current, sizeof (*inode));
      return GRUB_ERR_NONE;
    }

  while (*path == '/')
    path++;

  scratch = grub_k64xfs_strdup_local (path);
  if (!scratch)
    return grub_errno;

  for (token = scratch; token && *token; token = next)
    {
      struct grub_k64xfs_dirent ent;
      grub_int32_t child_id;

      while (*token == '/')
        token++;
      if (*token == '\0')
        break;
      next = token;
      while (*next && *next != '/')
        next++;
      if (*next == '/')
        *next++ = '\0';
      else
        next = NULL;

      if (grub_strcmp (token, ".") == 0)
        continue;
      if (grub_strcmp (token, "..") == 0)
        return grub_error (GRUB_ERR_FILE_NOT_FOUND, "parent lookup unsupported");

      child_id = grub_k64xfs_find_child (mount, &current, token, &ent);
      if (child_id < 0)
        {
          grub_free (scratch);
          return grub_error (GRUB_ERR_FILE_NOT_FOUND, "file not found");
        }
      if (grub_k64xfs_read_inode (mount, ent.inode_id, &current))
        {
          grub_free (scratch);
          return grub_errno;
        }
    }

  grub_free (scratch);
  grub_k64xfs_copy (inode, &current, sizeof (*inode));
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_k64xfs_dir (grub_device_t device, const char *path,
                 grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_k64xfs_mount *mount;
  struct grub_k64xfs_inode dir;
  grub_uint64_t blocks;
  grub_uint64_t b;

  mount = grub_k64xfs_mount (device->disk);
  if (!mount)
    return grub_errno;

  if (grub_k64xfs_resolve (mount, path, &dir))
    goto out;
  if (dir.type != K64XFS_TYPE_DIRECTORY)
    {
      grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");
      goto out;
    }

  blocks = (dir.size + K64XFS_BLOCK_SIZE - 1) >> 12;
  for (b = 0; b < blocks; b++)
    {
      struct grub_k64xfs_dirent *entries;
      grub_uint32_t s;

      if (grub_k64xfs_inode_read_block (mount, &dir, b, grub_k64xfs_block_buf))
        goto out;
      entries = (struct grub_k64xfs_dirent *) grub_k64xfs_block_buf;
      for (s = 0; s < K64XFS_DIRENTS_PER_BLOCK; s++)
        {
          struct grub_dirhook_info info;
          char name[K64XFS_NAME_MAX + 1];

          if (entries[s].inode_id == 0 || entries[s].name_len == 0 || entries[s].name_len >= sizeof (name))
            continue;
          grub_k64xfs_copy (name, entries[s].name, entries[s].name_len);
          name[entries[s].name_len] = '\0';
          grub_k64xfs_zero (&info, sizeof (info));
          info.dir = (entries[s].type == K64XFS_TYPE_DIRECTORY);
          if (hook (name, &info, hook_data))
            goto out;
        }
    }

out:
  grub_k64xfs_mount_free (mount);
  return grub_errno;
}

static grub_err_t
grub_k64xfs_open (grub_file_t file, const char *name)
{
  struct grub_k64xfs_mount *mount;
  struct grub_k64xfs_file *ctx;
  struct grub_k64xfs_inode inode;

  mount = grub_k64xfs_mount (file->device->disk);
  if (!mount)
    return grub_errno;
  if (grub_k64xfs_resolve (mount, name, &inode))
    {
      grub_k64xfs_mount_free (mount);
      return grub_errno;
    }
  if (inode.type != K64XFS_TYPE_REGULAR)
    {
      grub_k64xfs_mount_free (mount);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a regular file");
    }

  ctx = grub_zalloc (sizeof (*ctx));
  if (!ctx)
    {
      grub_k64xfs_mount_free (mount);
      return grub_errno;
    }

  ctx->mount = mount;
  grub_k64xfs_copy (&ctx->inode, &inode, sizeof (inode));
  file->data = ctx;
  file->size = inode.size;
  return GRUB_ERR_NONE;
}

static grub_ssize_t
grub_k64xfs_fsread (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_k64xfs_file *ctx = file->data;
  grub_size_t done = 0;

  if (!ctx)
    return -1;
  if ((grub_uint64_t) file->offset >= ctx->inode.size)
    return 0;
  if ((grub_uint64_t) len > ctx->inode.size - (grub_uint64_t) file->offset)
    len = (grub_size_t) (ctx->inode.size - (grub_uint64_t) file->offset);

  while (done < len)
    {
      grub_uint64_t absolute = (grub_uint64_t) file->offset + done;
      grub_uint64_t logical = absolute >> 12;
      grub_size_t in_block = (grub_size_t) (absolute & (K64XFS_BLOCK_SIZE - 1));
      grub_size_t n = K64XFS_BLOCK_SIZE - in_block;
      if (n > len - done)
        n = len - done;
      if (grub_k64xfs_inode_read_block (ctx->mount, &ctx->inode, logical, grub_k64xfs_block_buf))
        return -1;
      grub_k64xfs_copy (buf + done, grub_k64xfs_block_buf + in_block, n);
      done += n;
    }

  return (grub_ssize_t) done;
}

static grub_err_t
grub_k64xfs_close (grub_file_t file)
{
  struct grub_k64xfs_file *ctx = file->data;

  if (ctx)
    {
      grub_k64xfs_mount_free (ctx->mount);
      grub_free (ctx);
    }
  return GRUB_ERR_NONE;
}

static struct grub_fs grub_k64xfs_fs =
  {
    .name = "k64xfs",
    .fs_dir = grub_k64xfs_dir,
    .fs_open = grub_k64xfs_open,
    .fs_read = grub_k64xfs_fsread,
    .fs_close = grub_k64xfs_close,
#ifdef GRUB_UTIL
    .reserved_first_sector = 0,
    .blocklist_install = 0,
#endif
  };

GRUB_MOD_INIT (k64xfs)
{
  grub_k64xfs_fs.mod = mod;
  grub_fs_register (&grub_k64xfs_fs);
}

GRUB_MOD_FINI (k64xfs)
{
  grub_fs_unregister (&grub_k64xfs_fs);
}
