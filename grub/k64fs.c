#include <grub/disk.h>
#include <grub/dl.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/mm.h>
#include <grub/misc.h>

GRUB_MOD_LICENSE ("GPLv3+");
GRUB_MOD_NAME (k64fs);

#define K64FS_MAGIC_0 0x4B363446U
#define K64FS_MAGIC_1 0x00010053U
#define K64FS_VERSION  1U
#define K64FS_TYPE_DIR 1U
#define K64FS_TYPE_FILE 2U

struct grub_k64fs_header
{
  grub_uint32_t magic0;
  grub_uint32_t magic1;
  grub_uint16_t version;
  grub_uint16_t reserved;
  grub_uint32_t entry_count;
  grub_uint32_t entries_offset;
  grub_uint32_t strings_offset;
  grub_uint32_t data_offset;
  grub_uint32_t image_size;
} GRUB_PACKED;

struct grub_k64fs_entry
{
  grub_uint32_t parent_index;
  grub_uint16_t type;
  grub_uint16_t reserved0;
  grub_uint32_t name_offset;
  grub_uint32_t data_offset;
  grub_uint32_t data_size;
  grub_uint32_t reserved1;
} GRUB_PACKED;

struct grub_k64fs_mount
{
  grub_disk_t disk;
  struct grub_k64fs_header header;
  struct grub_k64fs_entry *entries;
  char *strings;
  grub_size_t strings_size;
};

struct grub_k64fs_file
{
  struct grub_k64fs_mount *mount;
  grub_uint32_t data_offset;
  grub_uint32_t data_size;
};

static void
grub_k64fs_bzero (void *buf, grub_size_t size)
{
  char *p = buf;

  while (size-- > 0)
    *p++ = 0;
}

static grub_err_t
grub_k64fs_read (grub_disk_t disk, grub_uint32_t offset, grub_size_t size, void *buf)
{
  grub_disk_addr_t sector = ((grub_disk_addr_t) offset) >> GRUB_DISK_SECTOR_BITS;
  grub_off_t sector_offset = (grub_off_t) (offset & (GRUB_DISK_SECTOR_SIZE - 1));

  return grub_disk_read (disk, sector, sector_offset, size, buf);
}

static grub_size_t
grub_k64fs_strlen (const char *s)
{
  grub_size_t len = 0;

  while (s && s[len] != '\0')
    len++;
  return len;
}

static char *
grub_k64fs_strdup_local (const char *s)
{
  grub_size_t len = grub_k64fs_strlen (s);
  char *copy = grub_malloc (len + 1);
  grub_size_t i;

  if (!copy)
    return NULL;

  for (i = 0; i <= len; i++)
    copy[i] = s[i];
  return copy;
}

static void
grub_k64fs_mount_free (struct grub_k64fs_mount *mount)
{
  if (!mount)
    return;

  grub_free (mount->entries);
  grub_free (mount->strings);
  grub_free (mount);
}

static struct grub_k64fs_mount *
grub_k64fs_mount (grub_disk_t disk)
{
  struct grub_k64fs_mount *mount;
  grub_size_t entries_size;

  mount = grub_zalloc (sizeof (*mount));
  if (!mount)
    return NULL;

  if (grub_k64fs_read (disk, 0, sizeof (mount->header), &mount->header))
    goto fail;

  if (mount->header.magic0 != K64FS_MAGIC_0
      || mount->header.magic1 != K64FS_MAGIC_1
      || mount->header.version != K64FS_VERSION)
    {
      grub_error (GRUB_ERR_BAD_FS, "not a k64fs filesystem");
      goto fail;
    }

  if (mount->header.entry_count == 0
      || mount->header.entries_offset < sizeof (mount->header)
      || mount->header.strings_offset < mount->header.entries_offset
      || mount->header.data_offset < mount->header.strings_offset
      || mount->header.image_size < mount->header.data_offset)
    {
      grub_error (GRUB_ERR_BAD_FS, "invalid k64fs header");
      goto fail;
    }

  entries_size = (grub_size_t) mount->header.entry_count * sizeof (*mount->entries);
  if (mount->header.entries_offset + entries_size > mount->header.strings_offset)
    {
      grub_error (GRUB_ERR_BAD_FS, "invalid k64fs entry table");
      goto fail;
    }

  mount->entries = grub_malloc (entries_size);
  if (!mount->entries)
    goto fail;
  if (grub_k64fs_read (disk, mount->header.entries_offset, entries_size, mount->entries))
    goto fail;

  mount->strings_size = mount->header.data_offset - mount->header.strings_offset;
  mount->strings = grub_malloc (mount->strings_size);
  if (!mount->strings)
    goto fail;
  if (grub_k64fs_read (disk, mount->header.strings_offset, mount->strings_size, mount->strings))
    goto fail;

  mount->disk = disk;
  return mount;

fail:
  grub_k64fs_mount_free (mount);
  return NULL;
}

static const char *
grub_k64fs_name (struct grub_k64fs_mount *mount, grub_uint32_t index)
{
  grub_uint32_t offset;

  if (!mount || index >= mount->header.entry_count)
    return NULL;

  offset = mount->entries[index].name_offset;
  if (offset >= mount->strings_size)
    return NULL;
  return mount->strings + offset;
}

static grub_int32_t
grub_k64fs_find_child (struct grub_k64fs_mount *mount, grub_uint32_t parent, const char *name)
{
  grub_uint32_t i;

  for (i = 1; i < mount->header.entry_count; i++)
    {
      if (mount->entries[i].parent_index != parent)
        continue;
      if (!grub_k64fs_name (mount, i))
        continue;
      if (grub_strcmp (grub_k64fs_name (mount, i), name) == 0)
        return (grub_int32_t) i;
    }
  return -1;
}

static grub_int32_t
grub_k64fs_resolve (struct grub_k64fs_mount *mount, const char *path)
{
  char *scratch;
  char *token;
  char *next;
  grub_int32_t current = 0;

  if (!path || !*path || (path[0] == '/' && path[1] == '\0'))
    return 0;

  while (*path == '/')
    path++;

  scratch = grub_k64fs_strdup_local (path);
  if (!scratch)
    return -1;

  for (token = scratch; token && *token; token = next)
    {
      while (*token == '/')
        token++;
      if (*token == '\0')
        break;

      next = token;
      while (*next && *next != '/')
        next++;
      if (*next == '/')
        {
          *next = '\0';
          next++;
        }
      else
        next = NULL;

      if (grub_strcmp (token, ".") == 0)
        continue;
      if (grub_strcmp (token, "..") == 0)
        {
          current = (grub_int32_t) mount->entries[current].parent_index;
          continue;
        }

      current = grub_k64fs_find_child (mount, (grub_uint32_t) current, token);
      if (current < 0)
        break;
    }

  grub_free (scratch);
  return current;
}

static grub_err_t
grub_k64fs_dir (grub_device_t device, const char *path,
                grub_fs_dir_hook_t hook, void *hook_data)
{
  struct grub_k64fs_mount *mount;
  grub_int32_t dir_index;
  grub_uint32_t i;

  mount = grub_k64fs_mount (device->disk);
  if (!mount)
    return grub_errno;

  dir_index = grub_k64fs_resolve (mount, path);
  if (dir_index < 0)
    {
      grub_k64fs_mount_free (mount);
      return grub_error (GRUB_ERR_FILE_NOT_FOUND, "file not found");
    }
  if (mount->entries[dir_index].type != K64FS_TYPE_DIR)
    {
      grub_k64fs_mount_free (mount);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a directory");
    }

  for (i = 1; i < mount->header.entry_count; i++)
    {
      struct grub_dirhook_info info;
      const char *name;

      if (mount->entries[i].parent_index != (grub_uint32_t) dir_index)
        continue;

      name = grub_k64fs_name (mount, i);
      if (!name)
        {
          grub_k64fs_mount_free (mount);
          return grub_error (GRUB_ERR_BAD_FS, "invalid k64fs filename");
        }

      grub_k64fs_bzero (&info, sizeof (info));
      info.dir = (mount->entries[i].type == K64FS_TYPE_DIR);
      if (hook (name, &info, hook_data))
        {
          break;
        }
    }

  grub_k64fs_mount_free (mount);
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_k64fs_open (grub_file_t file, const char *name)
{
  struct grub_k64fs_mount *mount;
  struct grub_k64fs_file *ctx;
  grub_int32_t index;

  mount = grub_k64fs_mount (file->device->disk);
  if (!mount)
    return grub_errno;

  index = grub_k64fs_resolve (mount, name);
  if (index < 0)
    {
      grub_k64fs_mount_free (mount);
      return grub_error (GRUB_ERR_FILE_NOT_FOUND, "file not found");
    }
  if (mount->entries[index].type != K64FS_TYPE_FILE)
    {
      grub_k64fs_mount_free (mount);
      return grub_error (GRUB_ERR_BAD_FILE_TYPE, "not a regular file");
    }

  ctx = grub_zalloc (sizeof (*ctx));
  if (!ctx)
    {
      grub_k64fs_mount_free (mount);
      return grub_errno;
    }

  ctx->mount = mount;
  ctx->data_offset = mount->header.data_offset + mount->entries[index].data_offset;
  ctx->data_size = mount->entries[index].data_size;
  file->data = ctx;
  file->size = ctx->data_size;
  return GRUB_ERR_NONE;
}

static grub_ssize_t
grub_k64fs_fsread (grub_file_t file, char *buf, grub_size_t len)
{
  struct grub_k64fs_file *ctx = file->data;
  grub_uint32_t remaining;

  if (!ctx)
    return -1;

  if ((grub_uint64_t) file->offset >= ctx->data_size)
    return 0;

  remaining = ctx->data_size - (grub_uint32_t) file->offset;
  if (len > remaining)
    len = remaining;

  if (grub_k64fs_read (ctx->mount->disk, ctx->data_offset + (grub_uint32_t) file->offset, len, buf))
    return -1;

  return (grub_ssize_t) len;
}

static grub_err_t
grub_k64fs_close (grub_file_t file)
{
  struct grub_k64fs_file *ctx = file->data;

  if (ctx)
    {
      grub_k64fs_mount_free (ctx->mount);
      grub_free (ctx);
    }

  return GRUB_ERR_NONE;
}

static struct grub_fs grub_k64fs_fs =
  {
    .name = "k64fs",
    .fs_dir = grub_k64fs_dir,
    .fs_open = grub_k64fs_open,
    .fs_read = grub_k64fs_fsread,
    .fs_close = grub_k64fs_close,
#ifdef GRUB_UTIL
    .reserved_first_sector = 0,
    .blocklist_install = 0,
#endif
  };

GRUB_MOD_INIT (k64fs)
{
  grub_k64fs_fs.mod = mod;
  grub_fs_register (&grub_k64fs_fs);
}

GRUB_MOD_FINI (k64fs)
{
  grub_fs_unregister (&grub_k64fs_fs);
}
