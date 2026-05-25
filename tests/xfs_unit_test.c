#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../k64_block.h"
#include "../k64_fs.h"
#include "../k64_log.h"
#include "../k64_xfs.h"

#define TEST_SECTOR_SIZE 512u
#define TEST_SECTORS 4096u

uint32_t k64_mb_magic = 0;
uint32_t k64_mb_info = 0;

static uint8_t test_disk[TEST_SECTORS * TEST_SECTOR_SIZE];
static int tests_failed = 0;

void k64_log_set_level(k64_loglevel_t level) {
    (void)level;
}

void k64_log(k64_loglevel_t level, const char* msg) {
    (void)level;
    (void)msg;
}

static bool disk_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    (void)ctx;
    if (lba + count > TEST_SECTORS) {
        return false;
    }
    memcpy(buffer, test_disk + lba * TEST_SECTOR_SIZE, (size_t)count * TEST_SECTOR_SIZE);
    return true;
}

static bool disk_write(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    (void)ctx;
    if (lba + count > TEST_SECTORS) {
        return false;
    }
    memcpy(test_disk + lba * TEST_SECTOR_SIZE, buffer, (size_t)count * TEST_SECTOR_SIZE);
    return true;
}

static void expect_true(const char* label, int condition) {
    if (!condition) {
        printf("FAIL: %s\n", label);
        tests_failed++;
    }
}

static void expect_string(const char* label, const char* got, const char* expected) {
    if (strcmp(got ? got : "", expected ? expected : "") != 0) {
        printf("FAIL: %s expected='%s' got='%s'\n", label, expected, got ? got : "");
        tests_failed++;
    }
}

static void expect_contains(const char* label, const char* haystack, const char* needle) {
    if (!haystack || !needle || !strstr(haystack, needle)) {
        printf("FAIL: %s missing='%s'\n", label, needle ? needle : "");
        tests_failed++;
    }
}

int main(void) {
    k64_block_device_t* dev;
    k64_xfs_mount_t fs;
    k64_fs_stat_t st;
    k64_xfs_check_report_t report;
    char listing[512];
    uint8_t read_buf[64];
    size_t read = 0;

    memset(test_disk, 0, sizeof(test_disk));
    k64_block_init();
    dev = k64_block_register_device("mem0", "unit", TEST_SECTOR_SIZE, TEST_SECTORS, true, NULL, disk_read, disk_write);
    expect_true("register", dev != NULL);
    expect_true("format", k64_xfs_format(dev, "unitfs"));
    expect_true("mount", k64_xfs_mount(dev, &fs));
    expect_true("stat root", k64_xfs_stat(&fs, "/", &st));
    expect_true("root dir", st.exists && st.is_dir);
    expect_true("mkdir", k64_xfs_mkdir(&fs, "/test", 0750u, 1001u, 1002u));
    expect_true("write", k64_xfs_write_file(&fs, "/test/hello", (const uint8_t*)"hello", 5, 1001u, 1002u));
    expect_true("read", k64_xfs_read_file(&fs, "/test/hello", read_buf, sizeof(read_buf), &read));
    read_buf[read] = 0;
    expect_true("read size", read == 5);
    expect_string("read value", (const char*)read_buf, "hello");
    expect_true("stat file", k64_xfs_stat(&fs, "/test/hello", &st));
    expect_true("stat size", st.size == 5);
    expect_true("stat uid", st.uid == 1001u);
    expect_true("stat gid", st.gid == 1002u);
    expect_true("chmod", k64_xfs_chmod(&fs, "/test/hello", 0600u));
    expect_true("chown", k64_xfs_chown(&fs, "/test/hello", 0u, 0u));
    expect_true("stat chmod", k64_xfs_stat(&fs, "/test/hello", &st));
    expect_true("mode changed", (st.mode & 0777u) == 0600u);
    expect_true("owner changed", st.uid == 0u && st.gid == 0u);
    expect_true("ls", k64_xfs_list_dir(&fs, "/test", listing, sizeof(listing)));
    expect_contains("ls hello", listing, "hello");
    expect_true("check", k64_xfs_check(&fs, &report));
    if (!report.ok) {
        printf("xfs check: errors=%u used_inodes=%u used_blocks=%llu free=%llu msg=%s\n",
               report.errors,
               report.used_inodes,
               (unsigned long long)report.used_blocks,
               (unsigned long long)report.free_blocks,
               report.message);
    }
    expect_true("check ok", report.ok);
    expect_true("reject invalid", !k64_xfs_write_file(&fs, "relative", (const uint8_t*)"x", 1, 0, 0));
    expect_true("reject long name", !k64_xfs_write_file(&fs, "/test/abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", (const uint8_t*)"x", 1, 0, 0));
    expect_true("remove", k64_xfs_remove(&fs, "/test/hello"));
    expect_true("removed", !k64_xfs_stat(&fs, "/test/hello", &st));
    expect_true("sync", k64_xfs_sync(&fs));
    expect_true("unmount", k64_xfs_unmount(&fs));
    expect_true("remount", k64_xfs_mount(dev, &fs));
    expect_true("check remount", k64_xfs_check(&fs, &report) && report.ok);

    if (tests_failed) {
        printf("xfs tests failed: %d\n", tests_failed);
        return 1;
    }
    printf("xfs tests passed\n");
    return 0;
}
