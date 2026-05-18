#include "k64_kpm.h"
#include "k64_fs.h"
#include "k64_net.h"
#include "k64_pit.h"
#include "k64_config.h"
#include "k64_rtl8139.h"
#include "k64_e1000.h"
#include "k64_string.h"
#include "k64_terminal.h"

#define KPM_SOURCES_PATH "/etc/kpm/sources.cfg"
#define KPM_INSTALLED_PATH "/var/lib/kpm/installed.db"
#define KPM_TMP_DIR "/tmp/kpm"
#define KPM_TEXT_MAX 8192
#define KPM_MAX_SOURCES 8
#define KPM_HTTP_TRIES 120
#define KPM_DOWNLOAD_CHUNK (16u * 1024u)

typedef struct {
    char name[32];
    char baseurl[160];
    kpm_url_t url;
} kpm_source_t;

static char kpm_text[KPM_TEXT_MAX];
static char kpm_text2[KPM_TEXT_MAX];
static uint8_t kpm_pkg_buf[16u * 1024u * 1024u];
static const char* kpm_validate_error = "unknown";
static uint32_t kpm_validate_expected_crc = 0;
static uint32_t kpm_validate_actual_crc = 0;

static void kpm_print(const char* text) {
    k64_term_write(text ? text : "");
    k64_term_putc('\n');
}

static void kpm_copy(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static size_t kpm_len(const char* s) {
    return k64_strlen(s ? s : "");
}

static void kpm_append(char* dst, size_t dst_size, const char* src) {
    size_t pos = kpm_len(dst);
    size_t i = 0;
    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static const char* kpm_skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    return s;
}

static const char* kpm_next_token(const char* s, char* token, size_t token_size) {
    size_t i = 0;
    s = kpm_skip_ws(s);
    while (s && *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && i + 1 < token_size) {
        token[i++] = *s++;
    }
    token[i] = '\0';
    return kpm_skip_ws(s);
}

static bool kpm_streq(const char* a, const char* b) {
    return k64_streq(a ? a : "", b ? b : "");
}

static void kpm_ensure_dirs(void) {
    (void)k64_fs_mkdir("/etc");
    (void)k64_fs_mkdir("/etc/kpm");
    (void)k64_fs_mkdir("/var");
    (void)k64_fs_mkdir("/var/lib");
    (void)k64_fs_mkdir("/var/lib/kpm");
    (void)k64_fs_mkdir("/tmp");
    (void)k64_fs_mkdir(KPM_TMP_DIR);
    (void)k64_fs_mkdir("/ex");
    (void)k64_fs_mkdir("/k64s");
    (void)k64_fs_mkdir("/k64m");
}

bool k64_kpm_parse_url(const char* url, kpm_url_t* out) {
    const char* p = url;
    size_t i = 0;
    uint64_t port = 80;

    if (!url || !out) {
        return false;
    }
    if (k64_strncmp(p, "https://", 8) == 0) {
        return false;
    }
    if (k64_strncmp(p, "http://", 7) != 0) {
        return false;
    }
    p += 7;
    kpm_copy(out->scheme, sizeof(out->scheme), "http");
    while (*p && *p != ':' && *p != '/' && i + 1 < sizeof(out->host)) {
        out->host[i++] = *p++;
    }
    out->host[i] = '\0';
    if (!out->host[0]) {
        return false;
    }
    if (*p == ':') {
        port = 0;
        p++;
        if (*p < '0' || *p > '9') {
            return false;
        }
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint64_t)(*p - '0');
            if (port > 65535u) {
                return false;
            }
            p++;
        }
    }
    if (*p && *p != '/') {
        return false;
    }
    i = 0;
    if (*p == '/') {
        while (*p && i + 1 < sizeof(out->base_path)) {
            out->base_path[i++] = *p++;
        }
    }
    out->base_path[i] = '\0';
    out->port = (uint16_t)port;
    return true;
}

static void kpm_join_path(char* out, size_t out_size, const char* base, const char* suffix) {
    out[0] = '\0';
    if (base && base[0]) {
        kpm_append(out, out_size, base);
    }
    if (!out[0] || out[kpm_len(out) - 1] != '/') {
        kpm_append(out, out_size, "/");
    }
    if (suffix && suffix[0] == '/') {
        suffix++;
    }
    kpm_append(out, out_size, suffix);
}

static bool kpm_load_sources(kpm_source_t* sources, size_t* count) {
    const char* p;
    size_t n = 0;

    if (count) {
        *count = 0;
    }
    if (!sources || !count) {
        return false;
    }
    if (!k64_fs_cat(KPM_SOURCES_PATH, kpm_text, sizeof(kpm_text))) {
        return true;
    }
    p = kpm_text;
    while (*p && n < KPM_MAX_SOURCES) {
        char name[32];
        char url[160];
        const char* line_end = p;
        const char* q;
        while (*line_end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }
        q = p;
        q = kpm_next_token(q, name, sizeof(name));
        q = kpm_next_token(q, url, sizeof(url));
        if (name[0] && url[0] && k64_kpm_parse_url(url, &sources[n].url)) {
            kpm_copy(sources[n].name, sizeof(sources[n].name), name);
            kpm_copy(sources[n].baseurl, sizeof(sources[n].baseurl), url);
            n++;
        }
        p = line_end;
        while (*p == '\n' || *p == '\r') {
            p++;
        }
    }
    *count = n;
    return true;
}

static bool kpm_save_sources(kpm_source_t* sources, size_t count) {
    kpm_text[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        kpm_append(kpm_text, sizeof(kpm_text), sources[i].name);
        kpm_append(kpm_text, sizeof(kpm_text), " ");
        kpm_append(kpm_text, sizeof(kpm_text), sources[i].baseurl);
        kpm_append(kpm_text, sizeof(kpm_text), "\n");
    }
    kpm_ensure_dirs();
    return k64_fs_write_file(KPM_SOURCES_PATH, kpm_text);
}

static bool kpm_json_get_string_after(const char* json, const char* key, char* out, size_t out_size) {
    size_t key_len = kpm_len(key);
    const char* p = json;
    if (!json || !key || !out || out_size == 0) {
        return false;
    }
    while (*p) {
        if (*p == '"' && k64_strncmp(p + 1, key, key_len) == 0 && p[1 + key_len] == '"') {
            p += key_len + 2;
            p = kpm_skip_ws(p);
            if (*p != ':') {
                continue;
            }
            p = kpm_skip_ws(p + 1);
            if (*p != '"') {
                continue;
            }
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < out_size) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return *p == '"';
        }
        p++;
    }
    return false;
}

bool k64_kpm_json_latest_version(const char* json, const char* package, char* out, size_t out_size) {
    char name[64];
    if (!kpm_json_get_string_after(json, "latest", out, out_size)) {
        return false;
    }
    if (package && package[0] && kpm_json_get_string_after(json, "name", name, sizeof(name)) && !kpm_streq(name, package)) {
        return false;
    }
    return true;
}

bool k64_kpm_json_version_exists(const char* json, const char* version) {
    const char* p = json;
    size_t len = kpm_len(version);
    if (!json || !version || !version[0]) {
        return false;
    }
    while (*p) {
        if (*p == '"' && k64_strncmp(p + 1, version, len) == 0 && p[1 + len] == '"') {
            return true;
        }
        p++;
    }
    return false;
}

static uint32_t kpm_crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool kpm_bounded_cstr(const char* s, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (s[i] == '\0') {
            return true;
        }
    }
    return false;
}

static bool kpm_valid_install_name(const char* name) {
    if (!name || !name[0]) {
        return false;
    }
    for (size_t i = 0; name[i]; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 32 || ch == '/' || ch == '\\') {
            return false;
        }
        if (ch == '.' && name[i + 1] == '.') {
            return false;
        }
    }
    return true;
}

bool k64_kpm_validate_package_bytes(const uint8_t* data, size_t size, kpm_package_header_t* out_header) {
    const kpm_package_header_t* hdr;
    const uint8_t* payload;

    if (!data || size < sizeof(kpm_package_header_t)) {
        kpm_validate_error = "package too small";
        return false;
    }
    hdr = (const kpm_package_header_t*)data;
    if (hdr->magic != KPM_MAGIC || hdr->format_version != 1 ||
        (hdr->kind != KPM_KIND_ELF && hdr->kind != KPM_KIND_K64S && hdr->kind != KPM_KIND_K64M)) {
        kpm_validate_error = "bad header";
        return false;
    }
    if (!kpm_bounded_cstr(hdr->package_name, sizeof(hdr->package_name)) ||
        !kpm_bounded_cstr(hdr->package_version, sizeof(hdr->package_version)) ||
        !kpm_bounded_cstr(hdr->install_name, sizeof(hdr->install_name)) ||
        !kpm_valid_install_name(hdr->install_name)) {
        kpm_validate_error = "bad names";
        return false;
    }
    if (hdr->payload_size != size - sizeof(kpm_package_header_t)) {
        kpm_validate_error = "payload size mismatch";
        return false;
    }
    payload = data + sizeof(kpm_package_header_t);
    kpm_validate_expected_crc = hdr->payload_crc32;
    kpm_validate_actual_crc = kpm_crc32(payload, (size_t)hdr->payload_size);
    if (kpm_validate_actual_crc != kpm_validate_expected_crc) {
        kpm_validate_error = "payload crc mismatch";
        return false;
    }
    if (out_header) {
        *out_header = *hdr;
    }
    kpm_validate_error = "ok";
    return true;
}

static const char* kpm_kind_name(uint16_t kind) {
    if (kind == KPM_KIND_ELF) return "ELF";
    if (kind == KPM_KIND_K64S) return "K64S";
    if (kind == KPM_KIND_K64M) return "K64M";
    return "UNKNOWN";
}

static void kpm_install_path(char* out, size_t out_size, const kpm_package_header_t* hdr) {
    out[0] = '\0';
    if (hdr->kind == KPM_KIND_ELF) {
        kpm_append(out, out_size, "/ex/");
        kpm_append(out, out_size, hdr->install_name);
        kpm_append(out, out_size, ".elf");
    } else if (hdr->kind == KPM_KIND_K64S) {
        kpm_append(out, out_size, "/k64s/");
        kpm_append(out, out_size, hdr->install_name);
        kpm_append(out, out_size, ".k64s");
    } else {
        kpm_append(out, out_size, "/k64m/");
        kpm_append(out, out_size, hdr->install_name);
        kpm_append(out, out_size, ".k64m");
    }
}

static bool kpm_update_installed_db(const kpm_package_header_t* hdr, const char* path, const char* source_name) {
    if (!k64_fs_cat(KPM_INSTALLED_PATH, kpm_text, sizeof(kpm_text))) {
        kpm_text[0] = '\0';
    }
    kpm_append(kpm_text, sizeof(kpm_text), hdr->package_name);
    kpm_append(kpm_text, sizeof(kpm_text), " ");
    kpm_append(kpm_text, sizeof(kpm_text), hdr->package_version);
    kpm_append(kpm_text, sizeof(kpm_text), " ");
    kpm_append(kpm_text, sizeof(kpm_text), kpm_kind_name(hdr->kind));
    kpm_append(kpm_text, sizeof(kpm_text), " ");
    kpm_append(kpm_text, sizeof(kpm_text), path);
    kpm_append(kpm_text, sizeof(kpm_text), " ");
    kpm_append(kpm_text, sizeof(kpm_text), source_name ? source_name : "unknown");
    kpm_append(kpm_text, sizeof(kpm_text), "\n");
    return k64_fs_write_file(KPM_INSTALLED_PATH, kpm_text);
}

bool k64_kpm_install_package_bytes(const uint8_t* data, size_t size, const char* source_name) {
    kpm_package_header_t hdr;
    char final_path[128];
    char temp_path[160];
    char backup_path[160];
    k64_fs_stat_t old_st;
    bool had_old;

    if (!k64_kpm_validate_package_bytes(data, size, &hdr)) {
        return false;
    }
    kpm_ensure_dirs();
    kpm_install_path(final_path, sizeof(final_path), &hdr);
    kpm_copy(temp_path, sizeof(temp_path), final_path);
    kpm_append(temp_path, sizeof(temp_path), ".kpmtmp");
    kpm_copy(backup_path, sizeof(backup_path), KPM_TMP_DIR "/backup.bin");
    (void)k64_fs_remove(temp_path);
    (void)k64_fs_remove(backup_path);

    if (!k64_fs_write_file_raw(temp_path, data + sizeof(kpm_package_header_t), (size_t)hdr.payload_size)) {
        return false;
    }
    had_old = k64_fs_stat(final_path, &old_st) && old_st.exists && !old_st.is_dir;
    if (had_old && !k64_fs_copy(final_path, backup_path)) {
        (void)k64_fs_remove(temp_path);
        return false;
    }
    if (had_old && !k64_fs_remove(final_path)) {
        (void)k64_fs_remove(temp_path);
        (void)k64_fs_remove(backup_path);
        return false;
    }
    if (!k64_fs_move(temp_path, final_path)) {
        if (had_old) {
            (void)k64_fs_copy(backup_path, final_path);
        }
        (void)k64_fs_remove(temp_path);
        (void)k64_fs_remove(backup_path);
        return false;
    }
    (void)k64_fs_remove(backup_path);
    if (!kpm_update_installed_db(&hdr, final_path, source_name)) {
        return false;
    }
    return k64_fs_sync();
}

static void kpm_poll_net_ms(uint64_t ms) {
    uint64_t hz = k64_config.pit_hz ? k64_config.pit_hz : 1000;
    uint64_t ticks = (hz * ms) / 1000ULL;
    uint64_t deadline = k64_pit_get_ticks() + (ticks ? ticks : 1);
    uint64_t spins = 0;
    while (k64_pit_get_ticks() < deadline && spins < 250000ULL) {
        k64_rtl8139_poll();
        k64_e1000_poll();
        spins++;
    }
}

static bool kpm_fetch_text(kpm_source_t* src, const char* suffix, char* out, size_t out_size) {
    char path[192];
    const char* state = NULL;
    size_t size = 0;

    kpm_join_path(path, sizeof(path), src->url.base_path, suffix);
    for (int i = 0; i < KPM_HTTP_TRIES; ++i) {
        if (k64_net_http_get_raw(src->url.host, path, src->url.port, (uint8_t*)out, out_size - 1, &size, &state)) {
            out[size < out_size ? size : out_size - 1] = '\0';
            return true;
        }
        kpm_poll_net_ms(100);
    }
    k64_term_write("kpm: warning: fetch failed from ");
    k64_term_write(src->name);
    k64_term_write(": ");
    k64_term_write(state ? state : "no response");
    k64_term_putc('\n');
    return false;
}

static bool kpm_fetch_package(kpm_source_t* src, const char* package, const char* version, size_t* out_size) {
    char suffix[192];
    char path[224];
    const char* state = NULL;
    size_t got = 0;
    size_t total = 0;

    suffix[0] = '\0';
    kpm_append(suffix, sizeof(suffix), package);
    kpm_append(suffix, sizeof(suffix), "/");
    kpm_append(suffix, sizeof(suffix), version);
    kpm_append(suffix, sizeof(suffix), "/package.kpg");
    kpm_join_path(path, sizeof(path), src->url.base_path, suffix);

    for (int i = 0; i < KPM_HTTP_TRIES; ++i) {
        if (k64_net_http_get_raw(src->url.host, path, src->url.port, kpm_pkg_buf, sizeof(kpm_pkg_buf), &got, &state) &&
            got >= sizeof(kpm_package_header_t)) {
            const kpm_package_header_t* hdr = (const kpm_package_header_t*)kpm_pkg_buf;
            if (hdr->magic == KPM_MAGIC &&
                hdr->payload_size <= sizeof(kpm_pkg_buf) - sizeof(kpm_package_header_t) &&
                got == sizeof(kpm_package_header_t) + (size_t)hdr->payload_size) {
                *out_size = got;
                return true;
            }
        }
        kpm_poll_net_ms(100);
    }

    got = 0;
    state = NULL;
    for (int i = 0; i < KPM_HTTP_TRIES; ++i) {
        if (k64_net_http_get_range_raw(src->url.host, path, src->url.port, 0, sizeof(kpm_package_header_t) - 1u,
                                       kpm_pkg_buf, sizeof(kpm_package_header_t), &got, &state) &&
            got == sizeof(kpm_package_header_t)) {
            break;
        }
        kpm_poll_net_ms(100);
    }
    if (got != sizeof(kpm_package_header_t)) {
        k64_term_write("kpm: download failed: ");
        k64_term_write(state ? state : "header unavailable");
        k64_term_putc('\n');
        return false;
    }
    {
        const kpm_package_header_t* hdr = (const kpm_package_header_t*)kpm_pkg_buf;
        if (hdr->magic != KPM_MAGIC || hdr->payload_size > sizeof(kpm_pkg_buf) - sizeof(kpm_package_header_t)) {
            kpm_print("kpm: invalid package header");
            return false;
        }
        total = sizeof(kpm_package_header_t) + (size_t)hdr->payload_size;
    }
    got = sizeof(kpm_package_header_t);
    while (got < total) {
        size_t chunk = total - got;
        size_t chunk_got = 0;
        bool ok = false;
        if (chunk > KPM_DOWNLOAD_CHUNK) {
            chunk = KPM_DOWNLOAD_CHUNK;
        }
        for (int i = 0; i < KPM_HTTP_TRIES; ++i) {
            if (k64_net_http_get_range_raw(src->url.host, path, src->url.port, got, got + chunk - 1u,
                                           kpm_pkg_buf + got, chunk, &chunk_got, &state) &&
                chunk_got == chunk) {
                ok = true;
                break;
            }
            kpm_poll_net_ms(100);
        }
        if (!ok) {
            k64_term_write("kpm: download failed: ");
            k64_term_write(state ? state : "range unavailable");
            k64_term_putc('\n');
            return false;
        }
        got += chunk;
    }
    *out_size = total;
    return true;

}

static void kpm_cmd_sources(void) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    (void)kpm_load_sources(sources, &count);
    kpm_print("Sources:");
    for (size_t i = 0; i < count; ++i) {
        k64_term_write("  ");
        k64_term_write(sources[i].name);
        k64_term_write("  ");
        k64_term_write(sources[i].baseurl);
        k64_term_putc('\n');
    }
}

static void kpm_cmd_source_add(const char* args) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    char name[32];
    char url[160];
    kpm_url_t parsed;

    args = kpm_next_token(args, name, sizeof(name));
    (void)kpm_next_token(args, url, sizeof(url));
    if (!name[0] || !url[0] || !k64_kpm_parse_url(url, &parsed)) {
        kpm_print("usage: kpm source add <name> <http://host[:port][/path]>");
        return;
    }
    kpm_ensure_dirs();
    (void)kpm_load_sources(sources, &count);
    for (size_t i = 0; i < count; ++i) {
        if (kpm_streq(sources[i].name, name)) {
            kpm_print("kpm: source name already exists");
            return;
        }
        if (kpm_streq(sources[i].baseurl, url)) {
            kpm_print("kpm: source URL already exists");
            return;
        }
    }
    if (count >= KPM_MAX_SOURCES) {
        kpm_print("kpm: too many sources");
        return;
    }
    kpm_copy(sources[count].name, sizeof(sources[count].name), name);
    kpm_copy(sources[count].baseurl, sizeof(sources[count].baseurl), url);
    sources[count].url = parsed;
    count++;
    if (!kpm_save_sources(sources, count)) {
        kpm_print("kpm: failed to save sources");
        return;
    }
    kpm_print("kpm: source added");
}

static void kpm_cmd_source_del(const char* args) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    char name[32];
    bool found = false;
    args = kpm_next_token(args, name, sizeof(name));
    if (!name[0]) {
        kpm_print("usage: kpm source del <name>");
        return;
    }
    (void)kpm_load_sources(sources, &count);
    for (size_t i = 0; i < count; ++i) {
        if (kpm_streq(sources[i].name, name)) {
            found = true;
        }
        if (found && i + 1 < count) {
            sources[i] = sources[i + 1];
        }
    }
    if (!found) {
        kpm_print("kpm: source not found");
        return;
    }
    if (!kpm_save_sources(sources, count - 1)) {
        kpm_print("kpm: failed to save sources");
        return;
    }
    kpm_print("kpm: source removed");
}

static void kpm_cmd_list(void) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    (void)kpm_load_sources(sources, &count);
    for (size_t i = 0; i < count; ++i) {
        k64_term_write("Source: ");
        k64_term_write(sources[i].name);
        k64_term_putc('\n');
        if (kpm_fetch_text(&sources[i], "packages.json", kpm_text, sizeof(kpm_text))) {
            const char* p = kpm_text;
            while ((p = k64_strncmp(p, "", 0) == 0 ? p : NULL) && *p) {
                char name[64];
                char latest[32];
                char desc[96];
                if (!kpm_json_get_string_after(p, "name", name, sizeof(name))) break;
                (void)kpm_json_get_string_after(p, "latest", latest, sizeof(latest));
                (void)kpm_json_get_string_after(p, "description", desc, sizeof(desc));
                k64_term_write("  ");
                k64_term_write(name);
                k64_term_write("  ");
                k64_term_write(latest[0] ? latest : "-");
                k64_term_write("  ");
                k64_term_write(desc[0] ? desc : "");
                k64_term_putc('\n');
                p++;
                while (*p && k64_strncmp(p, "\"name\"", 6) != 0) p++;
            }
        }
    }
}

static void kpm_cmd_versions(const char* package) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    char suffix[128];
    if (!package || !package[0]) {
        kpm_print("usage: kpm versions <package>");
        return;
    }
    (void)kpm_load_sources(sources, &count);
    for (size_t i = 0; i < count; ++i) {
        suffix[0] = '\0';
        kpm_append(suffix, sizeof(suffix), package);
        kpm_append(suffix, sizeof(suffix), "/versions.json");
        if (kpm_fetch_text(&sources[i], suffix, kpm_text, sizeof(kpm_text))) {
            char latest[32];
            const char* p = kpm_text;
            (void)k64_kpm_json_latest_version(kpm_text, package, latest, sizeof(latest));
            k64_term_write(package);
            k64_term_write(":\n");
            while (*p) {
                if (*p == '"') {
                    char ver[32];
                    size_t n = 0;
                    p++;
                    while (*p && *p != '"' && n + 1 < sizeof(ver)) ver[n++] = *p++;
                    ver[n] = '\0';
                    if (ver[0] >= '0' && ver[0] <= '9') {
                        k64_term_write("  ");
                        k64_term_write(ver);
                        if (kpm_streq(ver, latest)) k64_term_write(" latest");
                        k64_term_putc('\n');
                    }
                }
                if (*p) p++;
            }
        }
    }
}

static void kpm_cmd_install(const char* args) {
    kpm_source_t sources[KPM_MAX_SOURCES];
    size_t count = 0;
    char package[64];
    char opt[16];
    char version[32];
    bool explicit_version = false;

    args = kpm_next_token(args, package, sizeof(package));
    args = kpm_next_token(args, opt, sizeof(opt));
    if (kpm_streq(opt, "-v")) {
        (void)kpm_next_token(args, version, sizeof(version));
        explicit_version = version[0] != '\0';
    } else {
        version[0] = '\0';
    }
    if (!package[0]) {
        kpm_print("usage: kpm install <package> [-v <version>]");
        return;
    }
    kpm_print("kpm: resolving package");
    (void)k64_fs_grow_root();
    (void)kpm_load_sources(sources, &count);
    for (size_t i = 0; i < count; ++i) {
        char suffix[128];
        size_t package_size = 0;
        kpm_package_header_t hdr;
        suffix[0] = '\0';
        kpm_append(suffix, sizeof(suffix), package);
        kpm_append(suffix, sizeof(suffix), "/versions.json");
        if (!kpm_fetch_text(&sources[i], suffix, kpm_text, sizeof(kpm_text))) {
            continue;
        }
        if (!explicit_version && !k64_kpm_json_latest_version(kpm_text, package, version, sizeof(version))) {
            continue;
        }
        if (!k64_kpm_json_version_exists(kpm_text, version)) {
            kpm_print("kpm: requested version not found");
            return;
        }
        k64_term_write("kpm: selected version ");
        k64_term_write(version);
        k64_term_putc('\n');
        kpm_print("kpm: downloading package");
        if (!kpm_fetch_package(&sources[i], package, version, &package_size)) {
            return;
        }
        kpm_ensure_dirs();
        kpm_copy(kpm_text2, sizeof(kpm_text2), KPM_TMP_DIR "/");
        kpm_append(kpm_text2, sizeof(kpm_text2), package);
        kpm_append(kpm_text2, sizeof(kpm_text2), "-");
        kpm_append(kpm_text2, sizeof(kpm_text2), version);
        kpm_append(kpm_text2, sizeof(kpm_text2), ".kpg");
        (void)k64_fs_write_file_raw(kpm_text2, kpm_pkg_buf, package_size);
        kpm_print("kpm: validating KPG1 package");
        if (!k64_kpm_validate_package_bytes(kpm_pkg_buf, package_size, &hdr)) {
            k64_term_write("kpm: invalid package: ");
            k64_term_write(kpm_validate_error);
            if (kpm_streq(kpm_validate_error, "payload crc mismatch")) {
                k64_term_write(" expected=");
                k64_term_write_hex(kpm_validate_expected_crc);
                k64_term_write(" actual=");
                k64_term_write_hex(kpm_validate_actual_crc);
            }
            k64_term_putc('\n');
            return;
        }
        k64_term_write("kpm: installing ");
        k64_term_write(kpm_kind_name(hdr.kind));
        k64_term_write(" to ");
        kpm_install_path(kpm_text2, sizeof(kpm_text2), &hdr);
        k64_term_write(kpm_text2);
        k64_term_putc('\n');
        if (!k64_kpm_install_package_bytes(kpm_pkg_buf, package_size, sources[i].name)) {
            kpm_print("kpm: install failed");
            return;
        }
        kpm_print("kpm: syncing rootfs");
        k64_term_write("kpm: installed ");
        k64_term_write(hdr.package_name);
        k64_term_write(" ");
        k64_term_write(hdr.package_version);
        k64_term_putc('\n');
        if (hdr.kind == KPM_KIND_K64M) {
            kpm_print("Installed driver. Run 'reload drivers' to activate.");
        } else if (hdr.kind == KPM_KIND_K64S) {
            kpm_print("Installed service. Reboot or reload services when service reload exists.");
        } else {
            k64_term_write("Installed executable to ");
            kpm_install_path(kpm_text2, sizeof(kpm_text2), &hdr);
            k64_term_write(kpm_text2);
            k64_term_putc('\n');
        }
        return;
    }
    kpm_print("kpm: package not found in configured sources");
}

static void kpm_usage(void) {
    kpm_print("usage: kpm <list|versions|install|sources|source>");
    kpm_print("       kpm source add <name> <baseurl>");
    kpm_print("       kpm source del <name>");
}

bool k64_kpm_command(const char* command, const char* args) {
    char sub[32];
    char arg[64];
    (void)command;
    args = kpm_next_token(args, sub, sizeof(sub));
    if (kpm_streq(sub, "sources")) {
        kpm_cmd_sources();
        return true;
    }
    if (kpm_streq(sub, "source")) {
        args = kpm_next_token(args, arg, sizeof(arg));
        if (kpm_streq(arg, "list")) kpm_cmd_sources();
        else if (kpm_streq(arg, "add")) kpm_cmd_source_add(args);
        else if (kpm_streq(arg, "del")) kpm_cmd_source_del(args);
        else kpm_usage();
        return true;
    }
    if (kpm_streq(sub, "list")) {
        kpm_cmd_list();
        return true;
    }
    if (kpm_streq(sub, "versions")) {
        args = kpm_next_token(args, arg, sizeof(arg));
        kpm_cmd_versions(arg);
        return true;
    }
    if (kpm_streq(sub, "install")) {
        kpm_cmd_install(args);
        return true;
    }
    kpm_usage();
    return true;
}

bool k64_kpm_service_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "kpm", k64_kpm_command);
    return true;
}

void k64_kpm_service_stop(k64_service_t* service) {
    (void)service;
}
