#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../k64_kpm.h"
#include "../k64_fs.h"
#include "../k64_config.h"
#include "../k64_log.h"

uint32_t k64_mb_magic = 0;
uint32_t k64_mb_info = 0;
k64_config_t k64_config;

void k64_log_set_level(k64_loglevel_t level) { (void)level; }
void k64_log(k64_loglevel_t level, const char* msg) { (void)level; (void)msg; }
void k64_term_write(const char* text) { (void)text; }
void k64_term_putc(char ch) { (void)ch; }
void k64_term_write_dec(uint64_t value) { (void)value; }
void k64_term_write_hex(uint64_t value) { (void)value; }
uint64_t k64_pit_get_ticks(void) { static uint64_t t; return ++t; }
void k64_rtl8139_poll(void) {}
void k64_e1000_poll(void) {}
bool k64_system_register_command(const char* owner, const char* command, k64_service_command_fn handler) {
    (void)owner; (void)command; (void)handler; return true;
}
bool k64_net_http_get_raw(const char* host, const char* path, uint16_t port, uint8_t* out, size_t out_capacity, size_t* out_size, const char** state) {
    (void)host; (void)path; (void)port; (void)out; (void)out_capacity; (void)out_size;
    if (state) *state = "stub";
    return false;
}
bool k64_net_http_get_range_raw(const char* host, const char* path, uint16_t port, size_t range_start, size_t range_end, uint8_t* out, size_t out_capacity, size_t* out_size, const char** state) {
    (void)host; (void)path; (void)port; (void)range_start; (void)range_end; (void)out; (void)out_capacity; (void)out_size;
    if (state) *state = "stub";
    return false;
}

static int tests_failed = 0;

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

static uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static size_t make_pkg(uint8_t* out, size_t out_size, uint16_t kind, const char* pkg, const char* version, const char* install, size_t payload_size) {
    kpm_package_header_t* hdr = (kpm_package_header_t*)out;
    uint8_t* payload = out + sizeof(*hdr);
    if (out_size < sizeof(*hdr) + payload_size) {
        return 0;
    }
    memset(out, 0, sizeof(*hdr) + payload_size);
    hdr->magic = KPM_MAGIC;
    hdr->format_version = 1;
    hdr->kind = kind;
    strncpy(hdr->package_name, pkg, sizeof(hdr->package_name) - 1);
    strncpy(hdr->package_version, version, sizeof(hdr->package_version) - 1);
    strncpy(hdr->install_name, install, sizeof(hdr->install_name) - 1);
    hdr->payload_size = payload_size;
    for (size_t i = 0; i < payload_size; ++i) {
        payload[i] = (uint8_t)(i * 31u + kind);
    }
    hdr->payload_crc32 = crc32(payload, payload_size);
    return sizeof(*hdr) + payload_size;
}

int main(void) {
    static uint8_t package[900 * 1024];
    kpm_url_t url;
    kpm_package_header_t hdr;
    k64_fs_stat_t st;
    const uint8_t* bytes;
    size_t size;
    size_t pkg_size;
    char buf[512];

    expect_true("parse url default port", k64_kpm_parse_url("http://repo.k64os.org", &url));
    expect_string("url host", url.host, "repo.k64os.org");
    expect_true("url port", url.port == 80);
    expect_string("url base", url.base_path, "");
    expect_true("parse url port path", k64_kpm_parse_url("http://192.168.1.2:8080/k64repo", &url));
    expect_true("url port 8080", url.port == 8080);
    expect_string("url base path", url.base_path, "/k64repo");
    expect_true("reject https", !k64_kpm_parse_url("https://repo.k64os.org", &url));

    expect_true("json latest", k64_kpm_json_latest_version("{\"name\":\"hello\",\"latest\":\"1.0.0\",\"versions\":[\"0.9.0\",\"1.0.0\"]}", "hello", buf, sizeof(buf)));
    expect_string("json latest value", buf, "1.0.0");
    expect_true("json version exists", k64_kpm_json_version_exists("{\"versions\":[\"0.9.0\",\"1.0.0\"]}", "0.9.0"));
    expect_true("json version missing", !k64_kpm_json_version_exists("{\"versions\":[\"0.9.0\",\"1.0.0\"]}", "2.0.0"));

    expect_true("fs start", k64_fs_driver_start());
    (void)k64_kpm_command("kpm", "source add main http://repo.k64os.org");
    expect_true("sources written", k64_fs_cat("/etc/kpm/sources.cfg", buf, sizeof(buf)));
    expect_true("source contains main", strstr(buf, "main http://repo.k64os.org") != NULL);
    (void)k64_kpm_command("kpm", "source add local http://192.168.56.1:8080/k64repo");
    expect_true("source add base path", k64_fs_cat("/etc/kpm/sources.cfg", buf, sizeof(buf)) && strstr(buf, "local http://192.168.56.1:8080/k64repo") != NULL);

    pkg_size = make_pkg(package, sizeof(package), KPM_KIND_ELF, "hello", "1.0.0", "hello", 700 * 1024);
    expect_true("validate large elf", k64_kpm_validate_package_bytes(package, pkg_size, &hdr));
    expect_true("install large elf", k64_kpm_install_package_bytes(package, pkg_size, "main"));
    expect_true("elf stat", k64_fs_stat("/ex/hello.elf", &st) && st.size == 700 * 1024);
    expect_true("elf raw read", k64_fs_read_file_raw("/ex/hello.elf", &bytes, &size) && size == 700 * 1024 && bytes[123] == (uint8_t)(123 * 31u + KPM_KIND_ELF));

    pkg_size = make_pkg(package, sizeof(package), KPM_KIND_K64S, "svc", "1.0.0", "svc", 4096);
    expect_true("install k64s", k64_kpm_install_package_bytes(package, pkg_size, "main"));
    expect_true("k64s stat", k64_fs_stat("/k64s/svc.k64s", &st) && st.size == 4096);

    pkg_size = make_pkg(package, sizeof(package), KPM_KIND_K64M, "drv", "1.0.0", "drv", 4096);
    expect_true("install k64m", k64_kpm_install_package_bytes(package, pkg_size, "main"));
    expect_true("k64m stat", k64_fs_stat("/k64m/drv.k64m", &st) && st.size == 4096);

    pkg_size = make_pkg(package, sizeof(package), KPM_KIND_ELF, "bad", "1.0.0", "../bad", 64);
    expect_true("reject traversal", !k64_kpm_validate_package_bytes(package, pkg_size, &hdr));

    expect_true("existing final before bad", k64_fs_read_file_raw("/ex/hello.elf", &bytes, &size) && size == 700 * 1024);
    package[sizeof(kpm_package_header_t) + 10] ^= 0x55;
    expect_true("bad crc rejected", !k64_kpm_install_package_bytes(package, pkg_size, "main"));
    expect_true("existing final preserved", k64_fs_read_file_raw("/ex/hello.elf", &bytes, &size) && size == 700 * 1024);

    if (tests_failed) {
        printf("kpm tests failed: %d\n", tests_failed);
        return 1;
    }
    printf("kpm tests passed\n");
    return 0;
}
