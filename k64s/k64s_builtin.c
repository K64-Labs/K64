#include "k64_artifact.h"
#include "k64_block.h"
#include "k64_elf.h"
#include "k64_config.h"
#include "k64_fs.h"
#include "k64_hotreload.h"
#include "k64_keyboard.h"
#include "k64_kpm.h"
#include "k64_modules.h"
#include "k64_net.h"
#include "k64_pit.h"
#include "k64_pmm.h"
#include "k64_reload.h"
#include "k64_e1000.h"
#include "k64_rtl8139.h"
#include "k64_sched.h"
#include "k64_serial.h"
#include "k64_shell.h"
#include "k64_string.h"
#include "k64_system.h"
#include "k64_terminal.h"
#include "k64_user.h"
#include "k64_version.h"
#include "k64_xfs.h"
#include "k64_autoversion.h"

static void svc_print_line(const char* text) {
    k64_term_write(text ? text : "");
    k64_term_putc('\n');
}

static const char* svc_skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static const char* svc_next_token(const char* s, char* token, int token_size) {
    int i = 0;

    s = svc_skip_ws(s);
    while (s && *s && *s != ' ' && *s != '\t' && i + 1 < token_size) {
        token[i++] = *s++;
    }
    token[i] = '\0';
    return svc_skip_ws(s);
}

static bool svc_parse_u64(const char* text, uint64_t* out) {
    uint64_t value = 0;

    if (!text || !text[0]) {
        return false;
    }
    while (*text) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10 + (uint64_t)(*text - '0');
        text++;
    }
    *out = value;
    return true;
}

static uint32_t svc_read_u32le(const uint8_t* p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void svc_print_size(uint64_t bytes) {
    const char* unit = " B";
    uint64_t whole = bytes;
    uint64_t frac = 0;

    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        unit = " GiB";
        whole = bytes / (1024ULL * 1024ULL * 1024ULL);
        frac = ((bytes % (1024ULL * 1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL * 1024ULL);
    } else if (bytes >= 1024ULL * 1024ULL) {
        unit = " MiB";
        whole = bytes / (1024ULL * 1024ULL);
        frac = ((bytes % (1024ULL * 1024ULL)) * 10ULL) / (1024ULL * 1024ULL);
    } else if (bytes >= 1024ULL) {
        unit = " KiB";
        whole = bytes / 1024ULL;
        frac = ((bytes % 1024ULL) * 10ULL) / 1024ULL;
    }

    k64_term_write_dec(whole);
    if (frac) {
        k64_term_putc('.');
        k64_term_write_dec(frac);
    }
    k64_term_write(unit);
}

static void servicectl_usage(void) {
    svc_print_line("usage: servicectl <list|stopped|start|stop|restart> [pid]");
}

static void driverctl_usage(void) {
    svc_print_line("usage: driverctl <list|stopped|start|stop|restart> [id]");
}

static void storagectl_usage(void) {
    svc_print_line("usage: storagectl <list|partitions|partition|sync|root|install>");
    svc_print_line("       storagectl partitions <device>");
    svc_print_line("       storagectl partition <device> k64 yes");
    svc_print_line("       install");
    svc_print_line("       install wizard");
    svc_print_line("       install user <name> <password> [sudo]");
    svc_print_line("       install <device> yes");
}

static void svc_print_uptime_line(void) {
    uint64_t ticks = k64_pit_get_ticks();
    uint64_t hz = k64_config.pit_hz ? k64_config.pit_hz : 1000;
    uint64_t total_seconds = ticks / hz;
    uint64_t days = total_seconds / 86400;
    uint64_t hours = (total_seconds % 86400) / 3600;
    uint64_t minutes = (total_seconds % 3600) / 60;
    uint64_t seconds = total_seconds % 60;

    k64_term_write("Uptime: ");
    if (days) {
        k64_term_write_dec(days);
        k64_term_write("d ");
    }
    if (days || hours) {
        k64_term_write_dec(hours);
        k64_term_write("h ");
    }
    if (days || hours || minutes) {
        k64_term_write_dec(minutes);
        k64_term_write("m ");
    }
    k64_term_write_dec(seconds);
    k64_term_write("s\n");
}

static void svc_print_mib_line(const char* label, uint64_t bytes) {
    k64_term_write(label);
    k64_term_write(": ");
    k64_term_write_dec(bytes / (1024ULL * 1024ULL));
    k64_term_write(" MiB\n");
}

static void svc_append(char* dst, int dst_size, const char* src) {
    int pos = 0;

    while (dst[pos] && pos + 1 < dst_size) {
        pos++;
    }
    for (int i = 0; src && src[i] && pos + 1 < dst_size; ++i) {
        dst[pos++] = src[i];
    }
    dst[pos] = '\0';
}

static void svc_copy(char* dst, int dst_size, const char* src) {
    int i = 0;

    if (!dst || dst_size <= 0) {
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

static bool svc_contains_char(const char* text, char ch) {
    for (int i = 0; text && text[i]; ++i) {
        if (text[i] == ch) {
            return true;
        }
    }
    return false;
}

static void svc_write_u16le(uint8_t* buf, size_t off, uint16_t value) {
    buf[off] = (uint8_t)(value & 0xFFu);
    buf[off + 1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void svc_write_u32le(uint8_t* buf, size_t off, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        buf[off + i] = (uint8_t)((value >> (i * 8u)) & 0xFFu);
    }
}

static void svc_write_u64le(uint8_t* buf, size_t off, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        buf[off + i] = (uint8_t)((value >> (i * 8u)) & 0xFFu);
    }
}

static void netctl_usage(void) {
    svc_print_line("usage: netctl <status|poll|dhcp|arp|resolve>");
    svc_print_line("       netctl resolve <host>");
    svc_print_line("       ping <ipv4|host>");
    svc_print_line("       kcurl http://host[/path]");
    svc_print_line("       udp send <ipv4> <port> <text>");
}

static void svc_net_poll_many(int count) {
    for (int i = 0; i < count; ++i) {
        k64_rtl8139_poll();
        k64_e1000_poll();
    }
}

static void svc_net_poll_for_ms(uint64_t ms) {
    uint64_t hz = k64_config.pit_hz ? k64_config.pit_hz : 1000;
    uint64_t ticks = (hz * ms) / 1000ULL;
    uint64_t deadline = k64_pit_get_ticks() + (ticks ? ticks : 1);
    uint64_t spins = 0;

    while (k64_pit_get_ticks() < deadline && spins < 250000ULL) {
        svc_net_poll_many(1);
        spins++;
    }
    svc_net_poll_many(16);
}

static bool svc_net_autoconfig(void) {
    k64_net_status_t before;
    k64_net_status_t after;

    if (!k64_net_status(&before) || !before.link_up) {
        return false;
    }
    if (!k64_net_dhcp_discover()) {
        return true;
    }
    svc_net_poll_for_ms(750);
    if (k64_net_status(&after) && after.link_up &&
        (after.ipv4 != before.ipv4 || after.gateway != before.gateway || after.dns_server != before.dns_server)) {
        return true;
    }
    return true;
}

static bool svc_parse_http_url(const char* url, char* host, int host_size, char* path, int path_size, uint16_t* port) {
    const char* p = url;
    int i = 0;
    uint64_t port64 = 80;

    if (!url || !host || !path || !port || host_size <= 0 || path_size <= 0) {
        return false;
    }
    if (k64_strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (k64_strncmp(p, "https://", 8) == 0) {
        return false;
    }
    while (*p && *p != '/' && *p != ':' && i + 1 < host_size) {
        host[i++] = *p++;
    }
    host[i] = '\0';
    if (*p == ':') {
        char port_buf[8];
        int j = 0;
        p++;
        while (*p >= '0' && *p <= '9' && j + 1 < (int)sizeof(port_buf)) {
            port_buf[j++] = *p++;
        }
        port_buf[j] = '\0';
        if (!port_buf[0] || !svc_parse_u64(port_buf, &port64) || port64 == 0 || port64 > 65535) {
            return false;
        }
    }
    if (!host[0]) {
        return false;
    }
    i = 0;
    if (*p == '/') {
        while (*p && i + 1 < path_size) {
            path[i++] = *p++;
        }
    } else {
        path[i++] = '/';
    }
    path[i] = '\0';
    *port = (uint16_t)port64;
    return true;
}

static void svc_print_mac(const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; ++i) {
        char b[3];
        b[0] = hex[(mac[i] >> 4) & 0x0F];
        b[1] = hex[mac[i] & 0x0F];
        b[2] = '\0';
        k64_term_write(b);
        if (i != 5) {
            k64_term_putc(':');
        }
    }
}

static void svc_zero(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

static bool k64cc_build_stub_elf(const char* path) {
    uint8_t elf[0x7B];

    for (size_t i = 0; i < sizeof(elf); ++i) {
        elf[i] = 0;
    }

    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    svc_write_u16le(elf, 16, 2);
    svc_write_u16le(elf, 18, 62);
    svc_write_u32le(elf, 20, 1);
    svc_write_u64le(elf, 24, 0x50000078ULL);
    svc_write_u64le(elf, 32, 64);
    svc_write_u64le(elf, 40, 0);
    svc_write_u32le(elf, 48, 0);
    svc_write_u16le(elf, 52, 64);
    svc_write_u16le(elf, 54, 56);
    svc_write_u16le(elf, 56, 1);
    svc_write_u16le(elf, 58, 0);
    svc_write_u16le(elf, 60, 0);
    svc_write_u16le(elf, 62, 0);

    svc_write_u32le(elf, 64, 1);
    svc_write_u32le(elf, 68, 5);
    svc_write_u64le(elf, 72, 0);
    svc_write_u64le(elf, 80, 0x50000000ULL);
    svc_write_u64le(elf, 88, 0x50000000ULL);
    svc_write_u64le(elf, 96, sizeof(elf));
    svc_write_u64le(elf, 104, sizeof(elf));
    svc_write_u64le(elf, 112, 0x1000ULL);

    elf[0x78] = 0x31;
    elf[0x79] = 0xC0;
    elf[0x7A] = 0xC3;

    return k64_fs_write_file_raw(path, elf, sizeof(elf));
}

static bool k64cc_build_service_module(const char* path,
                                       const char* name,
                                       k64_service_class_t class_id,
                                       const char* entry_path) {
    k64_service_file_t file;

    svc_zero(&file, sizeof(file));
    file.magic = K64_SYSTEM_MAGIC;
    file.version = K64_ARTIFACT_VERSION;
    file.exec_kind = K64_ARTIFACT_EXEC_ELF;
    file.class_id = (uint8_t)class_id;
    file.priority = 1;
    svc_copy(file.name, sizeof(file.name), name);
    svc_copy(file.entry_path, sizeof(file.entry_path), entry_path);
    return k64_fs_write_file_raw(path, (const uint8_t*)&file, sizeof(file));
}

static bool k64cc_build_driver_module(const char* path,
                                      const char* name,
                                      uint8_t type,
                                      const char* entry_path) {
    k64_driver_file_t file;

    svc_zero(&file, sizeof(file));
    file.magic = K64_MODULE_MAGIC;
    file.version = K64_ARTIFACT_VERSION;
    file.exec_kind = K64_ARTIFACT_EXEC_ELF;
    file.type = type;
    file.priority = 1;
    svc_copy(file.name, sizeof(file.name), name);
    svc_copy(file.entry_path, sizeof(file.entry_path), entry_path);
    return k64_fs_write_file_raw(path, (const uint8_t*)&file, sizeof(file));
}

static void k64cc_usage(void) {
    svc_print_line("usage: k64cc <elf|k64s|k64m> ...");
    svc_print_line("  k64cc elf <name|path>");
    svc_print_line("  k64cc k64s <name> [system|root|user]");
    svc_print_line("  k64cc k64m <name> [driver|filesystem|service]");
}

static bool elfctl_command(const char* command, const char* args) {
    char path[96];
    const char* elf_args;

    (void)command;
    if (!args || !args[0]) {
        svc_print_line("usage: elfrun </path/to/file.elf>");
        return true;
    }
    elf_args = svc_next_token(args, path, sizeof(path));
    if (!path[0] || !k64_elf_spawn_user_path_args(path, elf_args)) {
        svc_print_line("elfrun failed");
        return true;
    }
    return true;
}

static bool svc_get_kernel_identity(char* kernel_name, int kernel_name_size, char* kernel_id, int kernel_id_size) {
    int len;

    if (!k64_fs_find_boot_kernel(kernel_name, kernel_name_size)) {
        return false;
    }

    len = (int)k64_strlen(kernel_name);
    if (len >= 4 && k64_strcmp(kernel_name + len - 4, ".elf") == 0) {
        int copy_len = len - 4;
        if (copy_len >= kernel_id_size) {
            copy_len = kernel_id_size - 1;
        }
        for (int i = 0; i < copy_len; ++i) {
            kernel_id[i] = kernel_name[i];
        }
        kernel_id[copy_len] = '\0';
    } else {
        int copy_len = len;
        if (copy_len >= kernel_id_size) {
            copy_len = kernel_id_size - 1;
        }
        for (int i = 0; i < copy_len; ++i) {
            kernel_id[i] = kernel_name[i];
        }
        kernel_id[copy_len] = '\0';
    }
    return true;
}

static bool sysfetch_command(const char* command, const char* args) {
    char kernel_name[64];
    char kernel_id[64];
    uint64_t total_mem = (uint64_t)k64_pmm_total_frames() * 4096ULL;
    uint64_t used_mem = (uint64_t)k64_pmm_used_frames() * 4096ULL;
    uint64_t free_mem = total_mem > used_mem ? (total_mem - used_mem) : 0;
    uint64_t fs_total = (uint64_t)k64_fs_capacity_bytes();
    uint64_t fs_used = (uint64_t)k64_fs_used_bytes();
    uint64_t fs_free = fs_total > fs_used ? (fs_total - fs_used) : 0;

    (void)command;
    (void)args;

    if (!svc_get_kernel_identity(kernel_name, sizeof(kernel_name), kernel_id, sizeof(kernel_id))) {
        kernel_id[0] = '\0';
    }

    k64_term_setcolor(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLACK);
    k64_term_write("                                _____      \n");
    k64_term_write("                               /    /      \n");
    k64_term_write("     .                        /    /       \n");
    k64_term_write("   .'|          .-''''-.     /    /        \n");
    k64_term_write(" .'  |         /  .--.  \\   /    /         \n");
    k64_term_write("<    |        /  /    '-'  /    /  __      \n");
    k64_term_write(" |   | ____  /  /.--.     /    /  |  |     \n");
    k64_term_write(" |   | \\ .' /  ' _   \\   /    '   |  |     \n");
    k64_term_write(" |   |/  . /   .' )   | /    '----|  |---. \n");
    k64_term_write(" |    /\\  \\|   (_.'   //          |  |   | \n");
    k64_term_write(" |   |  \\  \\\\       '  '----------|  |---' \n");
    k64_term_write(" '    \\  \\  \\ `----'              |  |     \n");
    k64_term_write("'------'  '---'                  /____\\    \n");
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_putc('\n');

    k64_term_write("User: ");
    k64_term_write(k64_user_effective_name());
    k64_term_putc('\n');
    k64_term_write("Kernel: ");
    if (kernel_id[0]) {
        k64_term_write(kernel_id);
    } else {
        k64_term_write("unknown");
    }
    k64_term_putc('\n');
    svc_print_uptime_line();
    svc_print_mib_line("Memory Available", free_mem);
    svc_print_mib_line("Memory Total", total_mem);
    svc_print_mib_line("K64XFS Available", fs_free);
    svc_print_mib_line("K64XFS Volume Total", fs_total);
    svc_print_mib_line("K64XFS Used", fs_used);
    svc_print_mib_line("K64XFS Runtime Read Limit", (uint64_t)k64_fs_image_limit_bytes());
    k64_term_write("Drivers Loaded: ");
    k64_term_write_dec(k64_modules_driver_count());
    k64_term_putc('\n');
    k64_term_write("Services Loaded: ");
    k64_term_write_dec(k64_system_service_count());
    k64_term_putc('\n');
    return true;
}

static bool uname_command(const char* command, const char* args) {
    char kernel_name[64];
    char kernel_id[64];

    (void)command;
    (void)args;

    if (!svc_get_kernel_identity(kernel_name, sizeof(kernel_name), kernel_id, sizeof(kernel_id))) {
        svc_print_line("K64 unknown");
        return true;
    }

    k64_term_write("K64 ");
    k64_term_write(kernel_id);
    k64_term_write(" ");
    k64_term_write(K64_KERNEL_ARCH);
    k64_term_putc('\n');
    return true;
}

static bool k64cc_command(const char* command, const char* args) {
    char subcmd[16];
    char name[64];
    char arg2[32];
    char path[128];
    char entry_path[128];
    k64_service_class_t class_id;
    uint8_t driver_type;

    (void)command;
    args = svc_next_token(args, subcmd, sizeof(subcmd));
    if (!subcmd[0]) {
        k64cc_usage();
        return true;
    }

    if (k64_streq(subcmd, "elf")) {
        args = svc_next_token(args, name, sizeof(name));
        if (!name[0]) {
            k64cc_usage();
            return true;
        }
        if (svc_contains_char(name, '/')) {
            svc_copy(path, sizeof(path), name);
        } else {
            path[0] = '\0';
            svc_append(path, sizeof(path), "/usr/");
            svc_append(path, sizeof(path), k64_user_effective_name());
            svc_append(path, sizeof(path), "/");
            svc_append(path, sizeof(path), name);
            if (!svc_contains_char(name, '.')) {
                svc_append(path, sizeof(path), ".elf");
            }
        }
        if (!k64cc_build_stub_elf(path)) {
            svc_print_line("k64cc: elf build failed");
            return true;
        }
        k64_term_write("k64cc: wrote ");
        k64_term_write(path);
        k64_term_putc('\n');
        return true;
    }

    if (k64_streq(subcmd, "k64s")) {
        args = svc_next_token(args, name, sizeof(name));
        args = svc_next_token(args, arg2, sizeof(arg2));
        if (!name[0]) {
            k64cc_usage();
            return true;
        }
        path[0] = '\0';
        svc_append(path, sizeof(path), "/k64s/");
        svc_append(path, sizeof(path), name);
        svc_append(path, sizeof(path), ".k64s");
        entry_path[0] = '\0';
        svc_append(entry_path, sizeof(entry_path), "/ex/");
        svc_append(entry_path, sizeof(entry_path), name);
        svc_append(entry_path, sizeof(entry_path), ".elf");
        class_id = K64_SERVICE_CLASS_SYSTEM;
        if (k64_streq(arg2, "root")) {
            class_id = K64_SERVICE_CLASS_ROOT;
        } else if (k64_streq(arg2, "user")) {
            class_id = K64_SERVICE_CLASS_USER;
        }
        if (!k64cc_build_service_module(path, name, class_id, entry_path)) {
            svc_print_line("k64cc: k64s build failed");
            return true;
        }
        k64_term_write("k64cc: wrote ");
        k64_term_write(path);
        k64_term_putc('\n');
        return true;
    }

    if (k64_streq(subcmd, "k64m")) {
        args = svc_next_token(args, name, sizeof(name));
        args = svc_next_token(args, arg2, sizeof(arg2));
        if (!name[0]) {
            k64cc_usage();
            return true;
        }
        path[0] = '\0';
        svc_append(path, sizeof(path), "/k64m/");
        svc_append(path, sizeof(path), name);
        svc_append(path, sizeof(path), ".k64m");
        entry_path[0] = '\0';
        svc_append(entry_path, sizeof(entry_path), "/ex/");
        svc_append(entry_path, sizeof(entry_path), name);
        svc_append(entry_path, sizeof(entry_path), ".elf");
        driver_type = K64_MODULE_TYPE_DRIVER;
        if (k64_streq(arg2, "filesystem")) {
            driver_type = K64_MODULE_TYPE_FS;
        } else if (k64_streq(arg2, "service")) {
            driver_type = K64_MODULE_TYPE_SERVICE;
        }
        if (!k64cc_build_driver_module(path, name, driver_type, entry_path)) {
            svc_print_line("k64cc: k64m build failed");
            return true;
        }
        k64_term_write("k64cc: wrote ");
        k64_term_write(path);
        k64_term_putc('\n');
        return true;
    }

    k64cc_usage();
    return true;
}

static void servicectl_list(bool stopped_only) {
    size_t count = k64_system_service_count();

    k64_term_write("PID   STATE    CLASS   HOST   NAME              VM BASE\n");
    for (size_t i = 0; i < count; ++i) {
        k64_service_t* service = k64_system_service_at(i);
        if (!service) {
            continue;
        }
        if (stopped_only && service->state != K64_SERVICE_STATE_STOPPED) {
            continue;
        }

        k64_term_write_dec(service->managed_pid);
        k64_term_write("  ");
        k64_term_write(k64_system_state_name(service->state));
        if (service->state == K64_SERVICE_STATE_RUNNING) {
            k64_term_write("  ");
        }
        k64_term_write("  ");
        k64_term_write(k64_system_class_name(service->class_id));
        if (service->class_id == K64_SERVICE_CLASS_USER) {
            k64_term_write("    ");
        } else {
            k64_term_write("  ");
        }
        k64_term_write("  ");
        if (service->class_id == K64_SERVICE_CLASS_KERNEL) {
            k64_term_write("ring0 ");
        } else if (service->ring3_required && service->ring3_verified) {
            k64_term_write("ring3 ");
        } else {
            k64_term_write("blocked");
        }
        k64_term_write("  ");
        k64_term_write(service->name);
        for (size_t pad = k64_strlen(service->name); pad < 16; ++pad) {
            k64_term_putc(' ');
        }
        k64_term_write("  ");
        k64_term_write_hex(service->vm_space.root_base);
        k64_term_putc('\n');
    }
}

static void driverctl_list(bool stopped_only) {
    size_t count = k64_modules_driver_count();

    k64_term_write("ID    STATE    NAME              SOURCE\n");
    for (size_t i = 0; i < count; ++i) {
        k64_driver_t* driver = k64_modules_driver_at(i);
        if (!driver) {
            continue;
        }
        if (stopped_only && driver->state != K64_DRIVER_STATE_STOPPED) {
            continue;
        }

        k64_term_write_dec(driver->id);
        k64_term_write("  ");
        k64_term_write(k64_modules_state_name(driver->state));
        if (driver->state == K64_DRIVER_STATE_RUNNING) {
            k64_term_write("  ");
        }
        k64_term_write("  ");
        k64_term_write(driver->name);
        for (size_t pad = k64_strlen(driver->name); pad < 16; ++pad) {
            k64_term_putc(' ');
        }
        k64_term_write("  ");
        k64_term_write(driver->source);
        k64_term_putc('\n');
    }
}

static bool servicectl_command(const char* command, const char* args) {
    char subcmd[16];
    char id_buf[24];
    uint64_t pid = 0;
    k64_service_result_t result;
    k64_service_t* service;

    (void)command;
    args = svc_next_token(args, subcmd, sizeof(subcmd));
    if (!subcmd[0]) {
        servicectl_usage();
        return true;
    }
    if (k64_streq(subcmd, "list")) {
        char extra[16];
        svc_next_token(args, extra, sizeof(extra));
        servicectl_list(k64_streq(extra, "stopped"));
        return true;
    }
    if (k64_streq(subcmd, "stopped")) {
        servicectl_list(true);
        return true;
    }
    args = svc_next_token(args, id_buf, sizeof(id_buf));
    if (!svc_parse_u64(id_buf, &pid)) {
        servicectl_usage();
        return true;
    }

    service = k64_system_find_service(pid);
    if (!service) {
        svc_print_line("service not found");
        return true;
    }
    if (!k64_user_can_manage_service(service)) {
        svc_print_line("permission denied");
        return true;
    }

    if (k64_streq(subcmd, "start")) {
        result = k64_system_start_service(pid);
    } else if (k64_streq(subcmd, "stop")) {
        result = k64_system_stop_service(pid);
    } else if (k64_streq(subcmd, "restart")) {
        result = k64_system_restart_service(pid);
    } else {
        servicectl_usage();
        return true;
    }

    if (result == K64_SERVICE_OK) {
        svc_print_line("service action completed");
    } else {
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }
    return true;
}

static bool driverctl_command(const char* command, const char* args) {
    char subcmd[16];
    char id_buf[24];
    uint64_t id = 0;
    k64_driver_result_t result;

    (void)command;
    args = svc_next_token(args, subcmd, sizeof(subcmd));
    if (!subcmd[0]) {
        driverctl_usage();
        return true;
    }
    if (k64_streq(subcmd, "list")) {
        char extra[16];
        svc_next_token(args, extra, sizeof(extra));
        driverctl_list(k64_streq(extra, "stopped"));
        return true;
    }
    if (k64_streq(subcmd, "stopped")) {
        driverctl_list(true);
        return true;
    }
    if (!k64_user_can_manage_drivers()) {
        svc_print_line("permission denied");
        return true;
    }
    args = svc_next_token(args, id_buf, sizeof(id_buf));
    if (!svc_parse_u64(id_buf, &id)) {
        driverctl_usage();
        return true;
    }

    if (k64_streq(subcmd, "start")) {
        result = k64_modules_start_driver(id);
    } else if (k64_streq(subcmd, "stop")) {
        result = k64_modules_stop_driver(id);
    } else if (k64_streq(subcmd, "restart")) {
        result = k64_modules_restart_driver(id);
    } else {
        driverctl_usage();
        return true;
    }

    if (result == K64_DRIVER_OK) {
        svc_print_line("driver action completed");
    } else {
        k64_term_write(k64_modules_result_string(result));
        k64_term_putc('\n');
    }
    return true;
}

static bool reload_command(const char* command, const char* args) {
    char target[16];
    k64_reload_mode_t mode;

    (void)command;
    if (!k64_user_is_root()) {
        svc_print_line("permission denied");
        return true;
    }

    args = svc_next_token(args, target, sizeof(target));
    if (!target[0]) {
        svc_print_line("usage: reload <drivers|kernel>");
        return true;
    }
    if (k64_streq(target, "drivers")) {
        mode = K64_RELOAD_DRIVERS;
    } else if (k64_streq(target, "kernel")) {
        mode = K64_RELOAD_KERNEL;
    } else {
        svc_print_line("usage: reload <drivers|kernel>");
        return true;
    }
    if (!k64_reload_request(mode)) {
        svc_print_line("reload request failed");
        return true;
    }
    svc_print_line("reload request submitted");
    return true;
}

static bool servicectl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "servicectl", servicectl_command);
    return true;
}

static void servicectl_stop(k64_service_t* service) {
    (void)service;
}

static bool driverctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "driverctl", driverctl_command);
    return true;
}

static bool sysfetch_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "sysfetch", sysfetch_command);
    return true;
}

static void sysfetch_stop(k64_service_t* service) {
    (void)service;
}

static bool k64cc_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "k64cc", k64cc_command);
    return true;
}

static void k64cc_stop(k64_service_t* service) {
    (void)service;
}

static bool elfctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "elfrun", elfctl_command);
    return true;
}

static void elfctl_stop(k64_service_t* service) {
    (void)service;
}

static bool uname_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "uname", uname_command);
    return true;
}

static void uname_stop(k64_service_t* service) {
    (void)service;
}

static void driverctl_stop(k64_service_t* service) {
    (void)service;
}

static void fsctl_print(const char* text) {
    k64_term_write(text ? text : "");
    k64_term_putc('\n');
}

static bool fsctl_can_access_path(const char* path, uint32_t mask) {
    k64_fs_stat_t st;

    if (!path || !path[0] || !k64_fs_stat(path, &st)) {
        return false;
    }
    return k64_user_can_access(st.uid, st.gid, st.mode, mask);
}

static bool fsctl_require_access(const char* path, uint32_t mask) {
    if (fsctl_can_access_path(path, mask)) {
        return true;
    }
    fsctl_print("permission denied");
    return false;
}

static bool fsctl_parent_path(const char* path, char* parent, int parent_size) {
    int len;

    if (!path || !path[0] || !parent || parent_size <= 0) {
        return false;
    }
    svc_copy(parent, parent_size, path);
    len = (int)k64_strlen(parent);
    while (len > 1 && parent[len - 1] == '/') {
        parent[--len] = '\0';
    }
    while (len > 0 && parent[len - 1] != '/') {
        parent[--len] = '\0';
    }
    if (len == 0) {
        svc_copy(parent, parent_size, ".");
    } else if (len == 1) {
        parent[1] = '\0';
    } else {
        parent[len - 1] = '\0';
    }
    return true;
}

static bool fsctl_require_create_access(const char* path) {
    char parent[128];

    if (!fsctl_parent_path(path, parent, sizeof(parent))) {
        fsctl_print("permission denied");
        return false;
    }
    return fsctl_require_access(parent, K64_ACCESS_WRITE | K64_ACCESS_EXEC);
}

static void fsctl_apply_new_owner(const char* path) {
    (void)k64_fs_chown(path, k64_user_effective_uid(), k64_user_effective_gid());
}

typedef struct {
    char name[32];
    uint64_t bytes;
} k64_installer_disk_t;

typedef struct {
    int step;
    int focus;
    int disk_index;
    int disk_count;
    bool sudoer;
    bool confirm;
    bool quit;
    bool installed;
    char username[32];
    char password[48];
    char message[96];
    k64_installer_disk_t disks[8];
} k64_installer_wizard_t;

static uint8_t installer_color(k64_color_t fg, k64_color_t bg) {
    return ((uint8_t)bg << 4) | ((uint8_t)fg & 0x0F);
}

static int installer_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

static void installer_text_at(int x, int y, const char* text, uint8_t color) {
    int i = 0;
    while (text && text[i] && x + i < k64_term_cols()) {
        k64_term_write_cell(x + i, y, text[i], color);
        i++;
    }
}

static void installer_center(int y, const char* text, uint8_t color) {
    installer_text_at((k64_term_cols() - installer_strlen(text)) / 2, y, text, color);
}

static void installer_fill(uint8_t color) {
    for (int y = 0; y < k64_term_rows(); ++y) {
        for (int x = 0; x < k64_term_cols(); ++x) {
            k64_term_write_cell(x, y, ' ', color);
        }
    }
}

static void installer_box(int x, int y, int w, int h, uint8_t border, uint8_t fill) {
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            bool edge = row == 0 || row == h - 1 || col == 0 || col == w - 1;
            char ch = ' ';
            uint8_t color = fill;
            if (edge) {
                color = border;
                ch = (row == 0 || row == h - 1) ? '-' : '|';
                if ((row == 0 || row == h - 1) && (col == 0 || col == w - 1)) {
                    ch = '+';
                }
            }
            k64_term_write_cell(x + col, y + row, ch, color);
        }
    }
}

static void installer_copy(char* dst, int dst_size, const char* src) {
    int i = 0;
    if (!dst || dst_size <= 0) {
        return;
    }
    while (src && src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void installer_append_char(char* dst, int dst_size, char ch) {
    int len = installer_strlen(dst);
    if (len + 1 >= dst_size || ch < 32 || ch > 126) {
        return;
    }
    dst[len] = ch;
    dst[len + 1] = '\0';
}

static void installer_backspace(char* dst) {
    int len = installer_strlen(dst);
    if (len > 0) {
        dst[len - 1] = '\0';
    }
}

static void installer_button(int x, int y, const char* label, bool selected) {
    uint8_t color = selected ? installer_color(K64_COLOR_WHITE, K64_COLOR_GREEN)
                             : installer_color(K64_COLOR_WHITE, K64_COLOR_BLUE);
    installer_text_at(x, y, "[ ", color);
    installer_text_at(x + 2, y, label, color);
    installer_text_at(x + 2 + installer_strlen(label), y, " ]", color);
}

static void installer_field(int x, int y, const char* label, const char* value, bool secret, bool selected) {
    uint8_t label_color = installer_color(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLUE);
    uint8_t field_color = selected ? installer_color(K64_COLOR_BLACK, K64_COLOR_WHITE)
                                   : installer_color(K64_COLOR_WHITE, K64_COLOR_BLACK);
    char masked[48];
    int len = installer_strlen(value);

    installer_text_at(x, y, label, label_color);
    for (int i = 0; i < 28; ++i) {
        k64_term_write_cell(x + 16 + i, y, ' ', field_color);
    }
    if (secret) {
        for (int i = 0; i < len && i + 1 < (int)sizeof(masked); ++i) {
            masked[i] = '*';
            masked[i + 1] = '\0';
        }
        if (len == 0) {
            masked[0] = '\0';
        }
        installer_text_at(x + 17, y, masked, field_color);
    } else {
        installer_text_at(x + 17, y, value ? value : "", field_color);
    }
}

static void installer_size_text(uint64_t bytes, char* out, int out_size) {
    uint64_t whole = bytes;
    const char* unit = "B";
    char rev[24];
    int r = 0;
    int pos = 0;

    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        whole = bytes / (1024ULL * 1024ULL * 1024ULL);
        unit = "GiB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        whole = bytes / (1024ULL * 1024ULL);
        unit = "MiB";
    } else if (bytes >= 1024ULL) {
        whole = bytes / 1024ULL;
        unit = "KiB";
    }
    if (whole == 0) {
        rev[r++] = '0';
    }
    while (whole && r < (int)sizeof(rev)) {
        rev[r++] = (char)('0' + (whole % 10));
        whole /= 10;
    }
    while (r > 0 && pos + 1 < out_size) {
        out[pos++] = rev[--r];
    }
    if (pos + 1 < out_size) {
        out[pos++] = ' ';
    }
    for (int i = 0; unit[i] && pos + 1 < out_size; ++i) {
        out[pos++] = unit[i];
    }
    if (out_size > 0) {
        out[pos] = '\0';
    }
}

static void installer_refresh_disks(k64_installer_wizard_t* w) {
    w->disk_count = 0;
    for (size_t i = 0; i < k64_block_device_count() && w->disk_count < 8; ++i) {
        k64_block_device_t* dev = k64_block_device_at(i);
        if (!dev || dev->is_partition || !dev->online || !dev->writable) {
            continue;
        }
        if (k64_streq(dev->name, "rootmod") || k64_streq(dev->source, "multiboot") ||
            k64_streq(dev->source, "memory")) {
            continue;
        }
        installer_copy(w->disks[w->disk_count].name, sizeof(w->disks[w->disk_count].name), dev->name);
        w->disks[w->disk_count].bytes = dev->block_count * (uint64_t)dev->block_size;
        w->disk_count++;
    }
    if (w->disk_index >= w->disk_count) {
        w->disk_index = w->disk_count > 0 ? w->disk_count - 1 : 0;
    }
}

static void installer_draw(const k64_installer_wizard_t* w) {
    uint8_t bg = installer_color(K64_COLOR_BLUE, K64_COLOR_BLACK);
    uint8_t panel = installer_color(K64_COLOR_BLACK, K64_COLOR_WHITE);
    uint8_t border = installer_color(K64_COLOR_WHITE, K64_COLOR_BLUE);
    uint8_t title = installer_color(K64_COLOR_WHITE, K64_COLOR_BLUE);
    uint8_t text = installer_color(K64_COLOR_BLACK, K64_COLOR_WHITE);
    uint8_t muted = installer_color(K64_COLOR_DARK_GREY, K64_COLOR_WHITE);
    uint8_t selected = installer_color(K64_COLOR_WHITE, K64_COLOR_GREEN);
    char size[24];

    installer_fill(bg);
    installer_box(4, 2, 72, 20, border, panel);
    installer_center(3, "K64 Installer", title);
    installer_text_at(8, 5, "Arrows move. Tab advances. Enter selects. N next. B back. Esc cancels.", muted);
    installer_text_at(9, 7, w->step == 0 ? "[1] Disk" : " 1  Disk", w->step == 0 ? selected : muted);
    installer_text_at(24, 7, w->step == 1 ? "[2] User" : " 2  User", w->step == 1 ? selected : muted);
    installer_text_at(39, 7, w->step == 2 ? "[3] Install" : " 3  Install", w->step == 2 ? selected : muted);

    if (w->step == 0) {
        installer_text_at(10, 9, "Choose the disk that will become bootable K64:", text);
        if (w->disk_count == 0) {
            installer_text_at(12, 12, "No writable disk was detected.", installer_color(K64_COLOR_RED, K64_COLOR_WHITE));
        }
        for (int i = 0; i < w->disk_count; ++i) {
            uint8_t row_color = (i == w->disk_index && w->focus == 0) ? selected : text;
            installer_size_text(w->disks[i].bytes, size, sizeof(size));
            installer_text_at(12, 11 + i, i == w->disk_index ? "(*) " : "( ) ", row_color);
            installer_text_at(16, 11 + i, w->disks[i].name, row_color);
            installer_text_at(28, 11 + i, size, row_color);
        }
        installer_button(49, 19, "Next", w->focus == 1);
        installer_text_at(12, 18, "Enter or N continues with the selected disk.", muted);
    } else if (w->step == 1) {
        installer_text_at(10, 9, "Create the first installed-system account:", text);
        installer_field(13, 11, "Username", w->username, false, w->focus == 0);
        installer_field(13, 13, "Password", w->password, true, w->focus == 1);
        installer_text_at(13, 15, w->sudoer ? "[x] Add to sudo group" : "[ ] Add to sudo group", w->focus == 2 ? selected : text);
        installer_text_at(13, 17, "Tab moves fields. Space toggles sudo.", muted);
        installer_button(13, 19, "Back", w->focus == 3);
        installer_button(49, 19, "Next", w->focus == 4);
    } else {
        installer_text_at(10, 9, "Ready to write a bootable K64XFS system disk.", text);
        installer_text_at(13, 11, "Target disk:", muted);
        installer_text_at(28, 11, w->disk_count ? w->disks[w->disk_index].name : "(none)", text);
        installer_text_at(13, 12, "New user:", muted);
        installer_text_at(28, 12, w->username[0] ? w->username : "(skip)", text);
        installer_text_at(13, 14, "This overwrites the target disk boot area and root partition.", installer_color(K64_COLOR_RED, K64_COLOR_WHITE));
        installer_text_at(13, 16, w->confirm ? "[x] I understand and want to install" : "[ ] I understand and want to install",
                          w->focus == 0 ? selected : text);
        installer_text_at(13, 18, "Space checks the box. Enter on Install writes the disk.", muted);
        installer_button(13, 19, "Back", w->focus == 1);
        installer_button(45, 19, "Install", w->focus == 2);
    }
    installer_text_at(8, 22, w->message[0] ? w->message : "Scripted installs still work: install <device> yes.",
                      installer_color(K64_COLOR_LIGHT_CYAN, K64_COLOR_BLACK));
    k64_term_set_cursor(0, 24);
}

static bool installer_serial_event(k64_key_event_t* event) {
    static int esc_state = 0;
    char c = 0;

    if (!k64_serial_get_char(&c)) {
        return false;
    }
    event->type = K64_KEY_NONE;
    event->ch = 0;
    if (esc_state == 0) {
        if (c == 27) {
            esc_state = 1;
            return false;
        }
        if (c == '\r' || c == '\n') {
            event->type = K64_KEY_ENTER;
            event->ch = '\n';
        } else if (c == '\b' || c == 127) {
            event->type = K64_KEY_BACKSPACE;
            event->ch = '\b';
        } else if (c == '\t') {
            event->type = K64_KEY_TAB;
            event->ch = '\t';
        } else {
            event->type = K64_KEY_CHAR;
            event->ch = c;
        }
        return true;
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            return false;
        }
        esc_state = 0;
        event->type = K64_KEY_ESCAPE;
        event->ch = 27;
        return true;
    }
    esc_state = 0;
    switch (c) {
        case 'A': event->type = K64_KEY_UP; return true;
        case 'B': event->type = K64_KEY_DOWN; return true;
        case 'C': event->type = K64_KEY_RIGHT; return true;
        case 'D': event->type = K64_KEY_LEFT; return true;
        default: return false;
    }
}

static bool installer_poll_event(k64_key_event_t* event) {
    if (k64_keyboard_get_event(event)) {
        return true;
    }
    return installer_serial_event(event);
}

static void installer_focus_next(k64_installer_wizard_t* w, int dir) {
    int max_focus = w->step == 1 ? 4 : (w->step == 2 ? 2 : 1);
    w->focus += dir;
    if (w->focus < 0) {
        w->focus = max_focus;
    }
    if (w->focus > max_focus) {
        w->focus = 0;
    }
}

static bool installer_focus_is_last(const k64_installer_wizard_t* w) {
    if (w->step == 1) {
        return w->focus == 4;
    }
    if (w->step == 2) {
        return w->focus == 2;
    }
    return w->focus == 1;
}

static void installer_next_step(k64_installer_wizard_t* w) {
    w->message[0] = '\0';
    if (w->step == 0 && w->disk_count == 0) {
        installer_copy(w->message, sizeof(w->message), "No writable target disk found.");
        return;
    }
    if (w->step < 2) {
        w->step++;
        w->focus = 0;
    }
}

static void installer_prev_step(k64_installer_wizard_t* w) {
    w->message[0] = '\0';
    if (w->step > 0) {
        w->step--;
        w->focus = 0;
    }
}

static void installer_run_install(k64_installer_wizard_t* w) {
    const char* dev_name;

    if (!w->confirm) {
        installer_copy(w->message, sizeof(w->message), "Check the confirmation box before installing.");
        return;
    }
    if (w->disk_count == 0) {
        installer_copy(w->message, sizeof(w->message), "No target disk selected.");
        return;
    }
    dev_name = w->disks[w->disk_index].name;
    if (w->username[0] && w->password[0] && !k64_user_create_account(w->username, w->password, w->sudoer)) {
        installer_copy(w->message, sizeof(w->message), "User creation failed.");
        return;
    }
    installer_copy(w->message, sizeof(w->message), "Installing K64XFS system. Please wait...");
    installer_draw(w);
    if (!k64_fs_install_to_block_device(dev_name)) {
        installer_copy(w->message, sizeof(w->message), "Install failed. Check target disk and try again.");
        return;
    }
    w->installed = true;
    installer_copy(w->message, sizeof(w->message), "Install complete. Remove ISO and reboot from disk.");
}

static void installer_handle_event(k64_installer_wizard_t* w, const k64_key_event_t* e) {
    if (e->type == K64_KEY_ESCAPE) {
        w->quit = true;
    } else if (e->type == K64_KEY_TAB) {
        if (installer_focus_is_last(w)) {
            if (w->step < 2) {
                installer_next_step(w);
            } else {
                installer_run_install(w);
            }
        } else {
            installer_focus_next(w, 1);
        }
    } else if (e->type == K64_KEY_UP) {
        if (w->step == 0 && w->focus == 0 && w->disk_index > 0) w->disk_index--; else installer_focus_next(w, -1);
    } else if (e->type == K64_KEY_DOWN) {
        if (w->step == 0 && w->focus == 0 && w->disk_index + 1 < w->disk_count) w->disk_index++; else installer_focus_next(w, 1);
    } else if (e->type == K64_KEY_LEFT) {
        installer_prev_step(w);
    } else if (e->type == K64_KEY_RIGHT) {
        installer_next_step(w);
    } else if (e->type == K64_KEY_BACKSPACE) {
        if (w->step == 1 && w->focus == 0) installer_backspace(w->username);
        if (w->step == 1 && w->focus == 1) installer_backspace(w->password);
    } else if (e->type == K64_KEY_CHAR) {
        if (e->ch == 'n' || e->ch == 'N') installer_next_step(w);
        else if (e->ch == 'b' || e->ch == 'B') installer_prev_step(w);
        else if ((e->ch == 'x' || e->ch == 'X') && w->step == 1 && w->focus == 2) w->sudoer = !w->sudoer;
        else if ((e->ch == 'x' || e->ch == 'X') && w->step == 2 && w->focus == 0) w->confirm = !w->confirm;
        else if (e->ch == ' ' && w->step == 1 && w->focus == 2) w->sudoer = !w->sudoer;
        else if (e->ch == ' ' && w->step == 2 && w->focus == 0) w->confirm = !w->confirm;
        else if (w->step == 1 && w->focus == 0) installer_append_char(w->username, sizeof(w->username), e->ch);
        else if (w->step == 1 && w->focus == 1) installer_append_char(w->password, sizeof(w->password), e->ch);
    } else if (e->type == K64_KEY_ENTER) {
        if (w->installed) w->quit = true;
        else if (w->step == 0) installer_next_step(w);
        else if (w->step == 1 && w->focus == 2) w->sudoer = !w->sudoer;
        else if (w->step == 1 && w->focus == 3) installer_prev_step(w);
        else if (w->step == 1) installer_next_step(w);
        else if (w->step == 2 && w->focus == 0) w->confirm = !w->confirm;
        else if (w->step == 2 && w->focus == 1) installer_prev_step(w);
        else if (w->step == 2) installer_run_install(w);
    }
}

static bool installer_wizard_run(void) {
    k64_installer_wizard_t wizard;
    k64_key_event_t event;

    memset(&wizard, 0, sizeof(wizard));
    wizard.sudoer = true;
    installer_copy(wizard.username, sizeof(wizard.username), "user");
    installer_refresh_disks(&wizard);

    k64_term_set_mirror_serial(false);
    k64_term_screen_start();
    k64_term_set_cursor_visible(false);
    k64_term_clear();
    k64_serial_write("K64 installer wizard started. Use arrows, tab, enter, and escape.\n");
    while (!wizard.quit) {
        installer_refresh_disks(&wizard);
        installer_draw(&wizard);
        while (!installer_poll_event(&event)) {
            k64_sched_sleep(1);
        }
        installer_handle_event(&wizard, &event);
    }
    k64_term_set_mirror_serial(true);
    k64_term_set_cursor_visible(true);
    k64_term_clear();
    if (wizard.installed) {
        svc_print_line("installer: root filesystem installed");
        svc_print_line("installer: BIOS boot area installed");
        svc_print_line("installer: remove the ISO and boot from the target disk");
    } else {
        svc_print_line("installer: cancelled");
    }
    return true;
}

static bool storagectl_command(const char* command, const char* args) {
    char sub[32];
    char dev_name[32];
    char confirm[16];

    if (command && k64_streq(command, "sync")) {
        sub[0] = 's';
        sub[1] = 'y';
        sub[2] = 'n';
        sub[3] = 'c';
        sub[4] = '\0';
    } else if (command && k64_streq(command, "install")) {
        sub[0] = 'i';
        sub[1] = 'n';
        sub[2] = 's';
        sub[3] = 't';
        sub[4] = 'a';
        sub[5] = 'l';
        sub[6] = 'l';
        sub[7] = '\0';
    } else {
        args = svc_next_token(args, sub, sizeof(sub));
    }
    if (!sub[0]) {
        storagectl_usage();
        return true;
    }

    if (k64_streq(sub, "list")) {
        for (size_t i = 0; i < k64_block_device_count(); ++i) {
            k64_block_device_t* dev = k64_block_device_at(i);
            uint64_t bytes;
            if (!dev) {
                continue;
            }
            bytes = dev->block_count * (uint64_t)dev->block_size;
            k64_term_write(dev->name);
            k64_term_write(" id=");
            k64_term_write_dec(dev->id);
            k64_term_write(dev->is_partition ? " kind=partition" : " kind=disk");
            k64_term_write(" state=");
            k64_term_write(dev->online ? "online" : "offline");
            k64_term_write(" mode=");
            k64_term_write(dev->writable ? "rw" : "ro");
            k64_term_write(" size=");
            svc_print_size(bytes);
            k64_term_write(" blocks=");
            k64_term_write_dec(dev->block_count);
            k64_term_write(" block_size=");
            k64_term_write_dec(dev->block_size);
            if (dev->is_partition) {
                k64_term_write(" start_lba=");
                k64_term_write_dec(dev->start_lba);
                k64_term_write(" type=");
                k64_term_write_hex(dev->partition_type);
            }
            k64_term_putc('\n');
        }
        return true;
    }

    if (k64_streq(sub, "partitions") || k64_streq(sub, "partition")) {
        k64_block_device_t* dev;
        uint8_t mbr[512];
        uint64_t used_blocks = 0;
        uint64_t free_blocks;

        args = svc_next_token(args, dev_name, sizeof(dev_name));
        if (!dev_name[0]) {
            storagectl_usage();
            return true;
        }
        dev = k64_block_find_device_by_name(dev_name);
        if (!dev || dev->is_partition || !dev->online || dev->block_size != 512) {
            svc_print_line("storagectl: disk not found");
            return true;
        }

        if (k64_streq(sub, "partition")) {
            char scheme[16];
            args = svc_next_token(args, scheme, sizeof(scheme));
            args = svc_next_token(args, confirm, sizeof(confirm));
            if (!k64_streq(scheme, "k64") || !k64_streq(confirm, "yes")) {
                svc_print_line("usage: storagectl partition <device> k64 yes");
                return true;
            }
            if (!k64_block_write_k64_mbr(dev)) {
                svc_print_line("storagectl: partition failed");
                return true;
            }
            svc_print_line("storagectl: wrote one K64 partition using remaining disk space");
            return true;
        }

        if (!k64_block_read(dev, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA) {
            k64_term_write(dev->name);
            k64_term_write(": no MBR partition table, free=");
            svc_print_size(dev->block_count * (uint64_t)dev->block_size);
            k64_term_putc('\n');
            return true;
        }

        k64_term_write(dev->name);
        k64_term_write(": size=");
        svc_print_size(dev->block_count * (uint64_t)dev->block_size);
        k64_term_putc('\n');
        for (uint8_t i = 0; i < 4; ++i) {
            size_t off = 446u + (size_t)i * 16u;
            uint8_t type = mbr[off + 4];
            uint32_t start = svc_read_u32le(mbr + off + 8);
            uint32_t count = svc_read_u32le(mbr + off + 12);
            if (type == 0 || count == 0) {
                continue;
            }
            used_blocks += count;
            k64_term_write("  p");
            k64_term_write_dec(i + 1);
            k64_term_write(" type=");
            k64_term_write_hex(type);
            k64_term_write(" start=");
            k64_term_write_dec(start);
            k64_term_write(" blocks=");
            k64_term_write_dec(count);
            k64_term_write(" size=");
            svc_print_size((uint64_t)count * dev->block_size);
            k64_term_putc('\n');
        }
        free_blocks = dev->block_count > used_blocks ? dev->block_count - used_blocks : 0;
        k64_term_write("  unallocated=");
        svc_print_size(free_blocks * (uint64_t)dev->block_size);
        k64_term_putc('\n');
        return true;
    }

    if (k64_streq(sub, "install")) {
        args = svc_next_token(args, dev_name, sizeof(dev_name));
        args = svc_next_token(args, confirm, sizeof(confirm));
        if (!dev_name[0]) {
            svc_print_line("+------------------------------------------------+");
            svc_print_line("| K64 installer                                  |");
            svc_print_line("+------------------------------------------------+");
            svc_print_line("| This installer writes a K64XFS system disk.    |");
            svc_print_line("| It can also create a local login account.      |");
            svc_print_line("+------------------------------------------------+");
            svc_print_line("");
            svc_print_line("Step 1 - choose a target disk:");
            for (size_t i = 0; i < k64_block_device_count(); ++i) {
                k64_block_device_t* dev = k64_block_device_at(i);
                if (!dev || dev->is_partition || !dev->online || !dev->writable) {
                    continue;
                }
                k64_term_write("  ");
                k64_term_write(dev->name);
                k64_term_write(dev->is_partition ? " partition" : " disk");
                k64_term_write(" size=");
                svc_print_size(dev->block_count * (uint64_t)dev->block_size);
                k64_term_write(" blocks=");
                k64_term_write_dec(dev->block_count);
                k64_term_write(" block_size=");
                k64_term_write_dec(dev->block_size);
                k64_term_putc('\n');
            }
            svc_print_line("");
            svc_print_line("Step 2 - create your user:");
            svc_print_line("  install user <name> <password> [sudo]");
            svc_print_line("");
            svc_print_line("Step 3 - install bootable K64:");
            svc_print_line("  install <device> yes");
            svc_print_line("");
            svc_print_line("Interactive mode:");
            svc_print_line("  install wizard");
            svc_print_line("Example:");
            svc_print_line("  install user lino secret sudo");
            svc_print_line("  install ata0 yes");
            return true;
        }
        if (k64_streq(dev_name, "wizard")) {
            return installer_wizard_run();
        }
        if (k64_streq(dev_name, "user")) {
            char user[32];
            char pass[48];
            char role[16];
            bool sudoer;

            svc_copy(user, sizeof(user), confirm);
            args = svc_next_token(args, pass, sizeof(pass));
            args = svc_next_token(args, role, sizeof(role));
            if (!user[0] || !pass[0]) {
                svc_print_line("usage: install user <name> <password> [sudo]");
                return true;
            }
            sudoer = role[0] == '\0' || k64_streq(role, "sudo");
            if (!k64_user_create_account(user, pass, sudoer)) {
                svc_print_line("installer: user creation failed");
                return true;
            }
            k64_term_write("installer: user created: ");
            k64_term_write(user);
            k64_term_write(sudoer ? " (sudo)\n" : "\n");
            svc_print_line("installer: you can log in after boot with that account");
            return true;
        }
        if (!k64_streq(confirm, "yes")) {
            svc_print_line("installer: refusing without confirmation");
            svc_print_line("Run exactly: install <device> yes");
            return true;
        }
        k64_term_write("installer: writing K64XFS root to ");
        k64_term_write(dev_name);
        k64_term_write("...\n");
        if (!k64_fs_install_to_block_device(dev_name)) {
            svc_print_line("installer: failed");
            return true;
        }
        svc_print_line("installer: root filesystem installed");
        svc_print_line("installer: BIOS boot area installed");
        svc_print_line("installer: remove the ISO and boot from the target disk");
        return true;
    }

    if (k64_streq(sub, "sync")) {
        if (!k64_fs_sync()) {
            svc_print_line("sync failed");
            return true;
        }
        svc_print_line("sync complete");
        return true;
    }

    if (k64_streq(sub, "root")) {
        char source[32];
        uint64_t total = (uint64_t)k64_fs_capacity_bytes();
        uint64_t used = (uint64_t)k64_fs_used_bytes();
        k64_term_write("rootfs source: ");
        if (k64_fs_mount_source(source, sizeof(source))) {
            k64_term_write(source);
        } else {
            k64_term_write("unknown");
        }
        k64_term_write(" mode=");
        k64_term_write(k64_fs_is_persistent() ? "persistent" : "ephemeral");
        k64_term_write(" used=");
        svc_print_size(used);
        k64_term_write(" free=");
        svc_print_size(total > used ? total - used : 0);
        k64_term_write(" total=");
        svc_print_size(total);
        k64_term_write(" image_limit=");
        svc_print_size((uint64_t)k64_fs_image_limit_bytes());
        k64_term_putc('\n');
        return true;
    }

    storagectl_usage();
    return true;
}

static bool netctl_command(const char* command, const char* args) {
    char sub[16];
    char arg1[32];
    char arg2[16];
    char host[64];
    char path[128];
    char response[1024];
    uint32_t ip;
    uint16_t http_port;
    uint64_t port64;
    k64_net_status_t st;
    bool pending;
    const char* state;

    if (command && k64_streq(command, "ping")) {
        sub[0] = 'p';
        sub[1] = 'i';
        sub[2] = 'n';
        sub[3] = 'g';
        sub[4] = '\0';
    } else if (command && k64_streq(command, "udp")) {
        sub[0] = 'u';
        sub[1] = 'd';
        sub[2] = 'p';
        sub[3] = '\0';
    } else if (command && k64_streq(command, "kcurl")) {
        sub[0] = 'k';
        sub[1] = 'c';
        sub[2] = 'u';
        sub[3] = 'r';
        sub[4] = 'l';
        sub[5] = '\0';
    } else {
        args = svc_next_token(args, sub, sizeof(sub));
    }

    if (!sub[0] || k64_streq(sub, "status")) {
        char ipbuf[24];
        if (!k64_net_status(&st) || !st.link_up) {
            svc_print_line("net: no link");
            return true;
        }
        k64_term_write("net: driver=");
        k64_term_write(st.driver[0] ? st.driver : "unknown");
        k64_term_write(" mac=");
        svc_print_mac(st.mac);
        k64_net_format_ipv4(st.ipv4, ipbuf, sizeof(ipbuf));
        k64_term_write(" ip=");
        k64_term_write(ipbuf);
        k64_net_format_ipv4(st.gateway, ipbuf, sizeof(ipbuf));
        k64_term_write(" gateway=");
        k64_term_write(ipbuf);
        k64_net_format_ipv4(st.dns_server, ipbuf, sizeof(ipbuf));
        k64_term_write(" dns=");
        k64_term_write(ipbuf);
        k64_term_putc('\n');
        k64_term_write("net: tx=");
        k64_term_write_dec(st.tx_frames);
        k64_term_write(" rx=");
        k64_term_write_dec(st.rx_frames);
        k64_term_write(" arp=");
        k64_term_write_dec(st.rx_arp);
        k64_term_write(" ipv4=");
        k64_term_write_dec(st.rx_ipv4);
        k64_term_write(" udp=");
        k64_term_write_dec(st.rx_udp);
        k64_term_write(" icmp=");
        k64_term_write_dec(st.rx_icmp);
        k64_term_write(" tcp=");
        k64_term_write_dec(st.rx_tcp);
        k64_term_write(" dns=");
        k64_term_write_dec(st.rx_dns);
        k64_term_write(" ping_replies=");
        k64_term_write_dec(st.ping_replies);
        k64_term_write(" last_eth=");
        k64_term_write_hex(st.last_ethertype);
        k64_term_putc('\n');
        return true;
    }

    if (k64_streq(sub, "poll")) {
        k64_rtl8139_poll();
        k64_e1000_poll();
        svc_print_line("net: poll complete");
        return true;
    }

    if (k64_streq(sub, "dhcp")) {
        if (!k64_net_dhcp_discover()) {
            svc_print_line("dhcp: discover failed");
            return true;
        }
        for (int i = 0; i < 128; ++i) {
            svc_net_poll_many(16);
        }
        svc_print_line("dhcp: complete");
        return true;
    }

    if (k64_streq(sub, "resolve")) {
        args = svc_next_token(args, host, sizeof(host));
        if (!host[0]) {
            netctl_usage();
            return true;
        }
        if (!k64_net_parse_ipv4(host, &ip)) {
            (void)svc_net_autoconfig();
        }
        for (int i = 0; i < 24; ++i) {
            if (k64_net_resolve_host(host, &ip, &pending)) {
                char ipbuf[24];
                k64_net_format_ipv4(ip, ipbuf, sizeof(ipbuf));
                k64_term_write("resolve: ");
                k64_term_write(host);
                k64_term_write(" -> ");
                k64_term_write(ipbuf);
                k64_term_putc('\n');
                return true;
            }
            svc_net_poll_for_ms(125);
        }
        svc_print_line("resolve: no DNS answer");
        return true;
    }

    if (k64_streq(sub, "arp")) {
        args = svc_next_token(args, arg1, sizeof(arg1));
        if (!k64_net_parse_ipv4(arg1, &ip)) {
            netctl_usage();
            return true;
        }
        if (!k64_net_send_arp_request(ip)) {
            svc_print_line("net: arp send failed");
            return true;
        }
        svc_print_line("net: arp request sent");
        return true;
    }

    if (k64_streq(sub, "ping")) {
        args = svc_next_token(args, arg1, sizeof(arg1));
        if (!arg1[0]) {
            netctl_usage();
            return true;
        }
        if (!k64_net_parse_ipv4(arg1, &ip)) {
            (void)svc_net_autoconfig();
        }
        if (!k64_net_resolve_host(arg1, &ip, &pending)) {
            svc_net_poll_for_ms(500);
        }
        if (!k64_net_resolve_host(arg1, &ip, &pending)) {
            svc_print_line("ping: resolve failed");
            return true;
        }
        if (!k64_net_send_ping(ip, 1)) {
            svc_net_poll_for_ms(500);
            if (!k64_net_send_ping(ip, 1)) {
                svc_print_line("ping: resolving target, retry after netctl poll");
                return true;
            }
        }
        for (int i = 0; i < 128; ++i) {
            svc_net_poll_many(4);
        }
        svc_print_line("ping: icmp echo sent");
        return true;
    }

    if (k64_streq(sub, "kcurl")) {
        args = svc_next_token(args, arg1, sizeof(arg1));
        if (!svc_parse_http_url(arg1, host, sizeof(host), path, sizeof(path), &http_port)) {
            svc_print_line("kcurl: only plain http://host[:port]/path URLs are supported");
            return true;
        }
        (void)svc_net_autoconfig();
        for (int i = 0; i < 80; ++i) {
            if (k64_net_http_get(host, path, http_port, response, sizeof(response), &state)) {
                k64_term_write(response[0] ? response : "kcurl: empty response");
                if (response[0] && response[k64_strlen(response) - 1] != '\n') {
                    k64_term_putc('\n');
                }
                return true;
            }
            svc_net_poll_for_ms(100);
        }
        k64_term_write("kcurl: ");
        k64_term_write(state ? state : "no response");
        k64_term_putc('\n');
        if (response[0]) {
            k64_term_write(response);
            if (response[k64_strlen(response) - 1] != '\n') {
                k64_term_putc('\n');
            }
        }
        return true;
    }

    if (k64_streq(sub, "udp")) {
        args = svc_next_token(args, arg1, sizeof(arg1));
        if (!k64_streq(arg1, "send")) {
            netctl_usage();
            return true;
        }
        args = svc_next_token(args, arg1, sizeof(arg1));
        args = svc_next_token(args, arg2, sizeof(arg2));
        if (!k64_net_parse_ipv4(arg1, &ip) || !svc_parse_u64(arg2, &port64) || port64 == 0 || port64 > 65535) {
            netctl_usage();
            return true;
        }
        if (!k64_net_send_udp(ip, (uint16_t)port64, args ? args : "")) {
            svc_print_line("udp: resolving target, retry after netctl poll");
            return true;
        }
        svc_print_line("udp: packet sent");
        return true;
    }

    netctl_usage();
    return true;
}

static bool fsctl_pwd_handler(const char* command, const char* args) {
    char buf[256];
    (void)command;
    (void)args;
    if (!k64_fs_pwd(buf, sizeof(buf))) {
        fsctl_print("pwd failed");
        return true;
    }
    fsctl_print(buf);
    return true;
}

static bool fsctl_ls_handler(const char* command, const char* args) {
    char buf[512];
    (void)command;
    if (!fsctl_require_access(args && args[0] ? args : ".", K64_ACCESS_READ | K64_ACCESS_EXEC)) {
        return true;
    }
    if (!k64_fs_ls(args && args[0] ? args : NULL, buf, sizeof(buf))) {
        fsctl_print("ls failed");
        return true;
    }
    k64_term_write(buf);
    return true;
}

static bool fsctl_cd_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !fsctl_require_access(args, K64_ACCESS_EXEC) || !k64_fs_cd(args)) {
        fsctl_print("cd failed");
        return true;
    }
    return true;
}

static bool fsctl_mkdir_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !fsctl_require_create_access(args) || !k64_fs_mkdir(args)) {
        fsctl_print("mkdir failed");
        return true;
    }
    fsctl_apply_new_owner(args);
    return true;
}

static bool fsctl_touch_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] ||
        (!k64_fs_stat(args, &(k64_fs_stat_t){0}) && !fsctl_require_create_access(args)) ||
        (k64_fs_stat(args, &(k64_fs_stat_t){0}) && !fsctl_require_access(args, K64_ACCESS_WRITE)) ||
        !k64_fs_touch(args)) {
        fsctl_print("touch failed");
        return true;
    }
    fsctl_apply_new_owner(args);
    return true;
}

static bool fsctl_cat_handler(const char* command, const char* args) {
    char buf[512];
    (void)command;
    if (!args || !args[0] || !fsctl_require_access(args, K64_ACCESS_READ) || !k64_fs_cat(args, buf, sizeof(buf))) {
        fsctl_print("cat failed");
        return true;
    }
    fsctl_print(buf);
    return true;
}

static bool fsctl_write_handler(const char* command, const char* args) {
    char path[64];
    const char* rest;
    (void)command;

    if (!args || !args[0]) {
        fsctl_print("write failed");
        return true;
    }
    rest = args;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(path)) {
            path[i++] = *rest++;
        }
        path[i] = '\0';
    }
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    if (!path[0] ||
        (k64_fs_stat(path, &(k64_fs_stat_t){0}) && !fsctl_require_access(path, K64_ACCESS_WRITE)) ||
        (!k64_fs_stat(path, &(k64_fs_stat_t){0}) && !fsctl_require_create_access(path)) ||
        !k64_fs_write_file(path, rest)) {
        fsctl_print("write failed");
        return true;
    }
    fsctl_apply_new_owner(path);
    return true;
}

static bool fsctl_mkdirp_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !fsctl_require_create_access(args) || !k64_fs_mkdir_p(args)) {
        fsctl_print("mkdirp failed");
        return true;
    }
    fsctl_apply_new_owner(args);
    return true;
}

static bool fsctl_append_handler(const char* command, const char* args) {
    char path[64];
    const char* rest;
    (void)command;

    if (!args || !args[0]) {
        fsctl_print("append failed");
        return true;
    }
    rest = args;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(path)) {
            path[i++] = *rest++;
        }
        path[i] = '\0';
    }
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    if (!path[0] ||
        (k64_fs_stat(path, &(k64_fs_stat_t){0}) && !fsctl_require_access(path, K64_ACCESS_WRITE)) ||
        (!k64_fs_stat(path, &(k64_fs_stat_t){0}) && !fsctl_require_create_access(path)) ||
        !k64_fs_append_file(path, rest)) {
        fsctl_print("append failed");
        return true;
    }
    fsctl_apply_new_owner(path);
    return true;
}

static bool fsctl_rm_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !fsctl_require_access(args, K64_ACCESS_WRITE) || !k64_fs_remove(args)) {
        fsctl_print("rm failed");
        return true;
    }
    return true;
}

static bool fsctl_rmdir_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !fsctl_require_access(args, K64_ACCESS_WRITE) || !k64_fs_rmdir(args)) {
        fsctl_print("rmdir failed");
        return true;
    }
    return true;
}

static bool fsctl_mv_handler(const char* command, const char* args) {
    char src[64];
    char dst[64];
    const char* rest;
    (void)command;

    if (!args || !args[0]) {
        fsctl_print("mv failed");
        return true;
    }
    rest = args;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(src)) {
            src[i++] = *rest++;
        }
        src[i] = '\0';
    }
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(dst)) {
            dst[i++] = *rest++;
        }
        dst[i] = '\0';
    }
    if (!src[0] || !dst[0] ||
        !fsctl_require_access(src, K64_ACCESS_WRITE) ||
        !fsctl_require_create_access(dst) ||
        !k64_fs_move(src, dst)) {
        fsctl_print("mv failed");
        return true;
    }
    return true;
}

static bool fsctl_cp_handler(const char* command, const char* args) {
    char src[64];
    char dst[64];
    const char* rest;
    (void)command;

    if (!args || !args[0]) {
        fsctl_print("cp failed");
        return true;
    }
    rest = args;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(src)) {
            src[i++] = *rest++;
        }
        src[i] = '\0';
    }
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    {
        int i = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && i + 1 < (int)sizeof(dst)) {
            dst[i++] = *rest++;
        }
        dst[i] = '\0';
    }
    if (!src[0] || !dst[0] ||
        !fsctl_require_access(src, K64_ACCESS_READ) ||
        !fsctl_require_create_access(dst) ||
        !k64_fs_copy(src, dst)) {
        fsctl_print("cp failed");
        return true;
    }
    fsctl_apply_new_owner(dst);
    return true;
}

static bool svc_parse_octal_mode(const char* text, uint32_t* out) {
    uint32_t value = 0;

    if (!text || !text[0] || !out) {
        return false;
    }
    while (*text) {
        if (*text < '0' || *text > '7') {
            return false;
        }
        value = (value << 3) | (uint32_t)(*text - '0');
        text++;
    }
    *out = value & 07777u;
    return true;
}

static bool fsctl_chmod_handler(const char* command, const char* args) {
    char mode_text[16];
    char path[128];
    uint32_t mode;
    (void)command;

    if (!k64_user_is_root()) {
        fsctl_print("permission denied");
        return true;
    }
    args = svc_next_token(args, mode_text, sizeof(mode_text));
    args = svc_next_token(args, path, sizeof(path));
    if (!svc_parse_octal_mode(mode_text, &mode) || !path[0] || !k64_fs_chmod(path, mode)) {
        fsctl_print("usage: chmod <mode> <path>");
        return true;
    }
    fsctl_print("mode updated");
    return true;
}

static bool fsctl_chown_handler(const char* command, const char* args) {
    char owner[32];
    char path[128];
    char user[16];
    char group[16];
    uint32_t uid;
    uint32_t gid;
    int split = -1;
    (void)command;

    if (!k64_user_is_root()) {
        fsctl_print("permission denied");
        return true;
    }
    args = svc_next_token(args, owner, sizeof(owner));
    args = svc_next_token(args, path, sizeof(path));
    for (int i = 0; owner[i]; ++i) {
        if (owner[i] == ':') {
            split = i;
            break;
        }
    }
    if (split < 0 || !path[0]) {
        fsctl_print("usage: chown <user>:<group> <path>");
        return true;
    }
    owner[split] = '\0';
    svc_copy(user, sizeof(user), owner);
    svc_copy(group, sizeof(group), owner + split + 1);
    if (!k64_user_name_to_uid(user, &uid) ||
        !k64_user_group_to_gid(group, &gid) ||
        !k64_fs_chown(path, uid, gid)) {
        fsctl_print("chown failed");
        return true;
    }
    fsctl_print("owner updated");
    return true;
}

static bool fsctl_stat_handler(const char* command, const char* args) {
    k64_fs_stat_t st;
    (void)command;

    if (!k64_fs_stat(args && args[0] ? args : ".", &st)) {
        fsctl_print("stat failed");
        return true;
    }

    k64_term_write(st.is_dir ? "dir  " : "file ");
    k64_term_write(st.path);
    k64_term_write(" size=");
    k64_term_write_dec(st.size);
    k64_term_write(" mode=");
    k64_term_write_hex(st.mode);
    k64_term_write(" uid=");
    k64_term_write_dec(st.uid);
    k64_term_write(" gid=");
    k64_term_write_dec(st.gid);
    k64_term_write(" gen=");
    k64_term_write_dec(st.generation);
    k64_term_putc('\n');
    return true;
}

static bool fsctl_sync_handler(const char* command, const char* args) {
    (void)command;
    (void)args;
    if (!k64_fs_sync()) {
        fsctl_print("sync failed");
        return true;
    }
    fsctl_print("sync complete");
    return true;
}

static bool fsctl_grow_handler(const char* command, const char* args) {
    (void)command;
    if (args && args[0] && !k64_streq(args, "/")) {
        fsctl_print("usage: grow /");
        return true;
    }
    if (!k64_fs_grow_root()) {
        fsctl_print("grow failed");
        return true;
    }
    fsctl_print("grow complete");
    return true;
}

static bool fsctl_start(k64_service_t* service) {
    (void)service;
    if (!k64_modules_is_driver_running("fs")) {
        fsctl_print("fsctl: fs driver is not running");
        return false;
    }
    (void)k64_system_register_command("fsctl", "pwd", fsctl_pwd_handler);
    (void)k64_system_register_command("fsctl", "ls", fsctl_ls_handler);
    (void)k64_system_register_command("fsctl", "cd", fsctl_cd_handler);
    (void)k64_system_register_command("fsctl", "mkdir", fsctl_mkdir_handler);
    (void)k64_system_register_command("fsctl", "mkdirp", fsctl_mkdirp_handler);
    (void)k64_system_register_command("fsctl", "touch", fsctl_touch_handler);
    (void)k64_system_register_command("fsctl", "cat", fsctl_cat_handler);
    (void)k64_system_register_command("fsctl", "write", fsctl_write_handler);
    (void)k64_system_register_command("fsctl", "append", fsctl_append_handler);
    (void)k64_system_register_command("fsctl", "rm", fsctl_rm_handler);
    (void)k64_system_register_command("fsctl", "rmdir", fsctl_rmdir_handler);
    (void)k64_system_register_command("fsctl", "mv", fsctl_mv_handler);
    (void)k64_system_register_command("fsctl", "cp", fsctl_cp_handler);
    (void)k64_system_register_command("fsctl", "chmod", fsctl_chmod_handler);
    (void)k64_system_register_command("fsctl", "chown", fsctl_chown_handler);
    (void)k64_system_register_command("fsctl", "stat", fsctl_stat_handler);
    (void)k64_system_register_command("fsctl", "sync", fsctl_sync_handler);
    (void)k64_system_register_command("fsctl", "grow", fsctl_grow_handler);
    return true;
}

static void fsctl_stop(k64_service_t* service) {
    (void)service;
}

static k64_xfs_mount_t xfsctl_mount;
static bool xfsctl_mounted = false;

static const char* xfsctl_path(const char* path) {
    if (!path || !path[0]) {
        return "/";
    }
    if (k64_strncmp(path, "/x/", 3) == 0) {
        return path + 2;
    }
    if (k64_streq(path, "/x")) {
        return "/";
    }
    return path;
}

static bool xfsctl_require_mount(void) {
    if (xfsctl_mounted && xfsctl_mount.mounted) {
        return true;
    }
    svc_print_line("xfsctl: not mounted");
    return false;
}

static bool xfsctl_can_access(const char* path, uint32_t mask) {
    k64_fs_stat_t st;

    if (!xfsctl_mounted || !k64_xfs_stat(&xfsctl_mount, xfsctl_path(path), &st)) {
        return false;
    }
    return k64_user_can_access(st.uid, st.gid, st.mode, mask);
}

static bool xfsctl_require_access(const char* path, uint32_t mask) {
    if (xfsctl_can_access(path, mask)) {
        return true;
    }
    svc_print_line("permission denied");
    return false;
}

static bool xfsctl_parent_path(const char* path, char* parent, int parent_size) {
    int len;

    if (!path || !path[0] || !parent || parent_size <= 0) {
        return false;
    }
    svc_copy(parent, parent_size, path);
    len = (int)k64_strlen(parent);
    while (len > 1 && parent[len - 1] == '/') {
        parent[--len] = '\0';
    }
    while (len > 0 && parent[len - 1] != '/') {
        parent[--len] = '\0';
    }
    if (len == 0) {
        svc_copy(parent, parent_size, "/");
    } else if (len == 1) {
        parent[1] = '\0';
    } else {
        parent[len - 1] = '\0';
    }
    return true;
}

static bool xfsctl_require_create(const char* path) {
    char parent[128];

    if (!xfsctl_parent_path(xfsctl_path(path), parent, sizeof(parent))) {
        svc_print_line("permission denied");
        return false;
    }
    return xfsctl_require_access(parent, K64_ACCESS_WRITE | K64_ACCESS_EXEC);
}

static void xfsctl_usage(void) {
    svc_print_line("usage: xfsctl <devices|format|mount|unmount|info|ls|stat|mkdir|write|cat|rm|rmdir|chmod|chown|sync|check>");
}

static bool xfsctl_command(const char* command, const char* args) {
    char sub[32];
    char a[128];
    (void)command;

    args = svc_next_token(args, sub, sizeof(sub));
    if (!sub[0] || k64_streq(sub, "help")) {
        xfsctl_usage();
        return true;
    }
    if (k64_streq(sub, "devices")) {
        for (size_t i = 0; i < k64_block_device_count(); ++i) {
            k64_block_device_t* dev = k64_block_device_at(i);
            if (!dev || !dev->online) {
                continue;
            }
            k64_term_write(dev->name);
            k64_term_write(" size=");
            k64_term_write_dec(dev->block_count * (uint64_t)dev->block_size);
            k64_term_write(dev->writable ? " writable" : " readonly");
            k64_term_putc('\n');
        }
        return true;
    }
    if (k64_streq(sub, "format")) {
        char label[32];
        char yes[8];
        k64_block_device_t* dev;

        if (!k64_user_is_root()) {
            svc_print_line("permission denied");
            return true;
        }
        args = svc_next_token(args, a, sizeof(a));
        args = svc_next_token(args, label, sizeof(label));
        args = svc_next_token(args, yes, sizeof(yes));
        dev = k64_block_find_device_by_name(a);
        if (!dev || !k64_streq(yes, "yes") || !k64_xfs_format(dev, label)) {
            svc_print_line("xfsctl: format failed");
            return true;
        }
        svc_print_line("K64XFS format: OK");
        return true;
    }
    if (k64_streq(sub, "mount")) {
        k64_block_device_t* dev;
        char mountpoint[16];

        args = svc_next_token(args, a, sizeof(a));
        args = svc_next_token(args, mountpoint, sizeof(mountpoint));
        dev = k64_block_find_device_by_name(a);
        if (!dev || !k64_streq(mountpoint, "/x") || !k64_xfs_mount(dev, &xfsctl_mount)) {
            svc_print_line("xfsctl: mount failed");
            return true;
        }
        xfsctl_mounted = true;
        svc_print_line("K64XFS mounted at /x");
        return true;
    }
    if (k64_streq(sub, "unmount")) {
        if (!xfsctl_require_mount() || !k64_xfs_unmount(&xfsctl_mount)) {
            svc_print_line("xfsctl: unmount failed");
            return true;
        }
        xfsctl_mounted = false;
        svc_print_line("K64XFS unmounted");
        return true;
    }
    if (k64_streq(sub, "info")) {
        if (!xfsctl_require_mount()) {
            return true;
        }
        k64_term_write("K64XFS label=");
        k64_term_write(xfsctl_mount.super.label);
        k64_term_write(" blocks=");
        k64_term_write_dec(xfsctl_mount.super.total_blocks);
        k64_term_write(" free=");
        k64_term_write_dec(xfsctl_mount.super.free_blocks);
        k64_term_putc('\n');
        return true;
    }
    if (k64_streq(sub, "ls")) {
        char out[512];
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() ||
            !xfsctl_require_access(a[0] ? a : "/x", K64_ACCESS_READ | K64_ACCESS_EXEC) ||
            !k64_xfs_list_dir(&xfsctl_mount, xfsctl_path(a[0] ? a : "/x"), out, sizeof(out))) {
            svc_print_line("xfsctl: ls failed");
            return true;
        }
        k64_term_write(out);
        return true;
    }
    if (k64_streq(sub, "stat")) {
        k64_fs_stat_t st;
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !k64_xfs_stat(&xfsctl_mount, xfsctl_path(a), &st)) {
            svc_print_line("xfsctl: stat failed");
            return true;
        }
        k64_term_write(st.is_dir ? "dir  " : "file ");
        k64_term_write(a);
        k64_term_write(" size=");
        k64_term_write_dec(st.size);
        k64_term_write(" mode=");
        k64_term_write_hex(st.mode);
        k64_term_write(" uid=");
        k64_term_write_dec(st.uid);
        k64_term_write(" gid=");
        k64_term_write_dec(st.gid);
        k64_term_putc('\n');
        return true;
    }
    if (k64_streq(sub, "mkdir")) {
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !xfsctl_require_create(a) ||
            !k64_xfs_mkdir(&xfsctl_mount, xfsctl_path(a), 0755u, k64_user_effective_uid(), k64_user_effective_gid())) {
            svc_print_line("xfsctl: mkdir failed");
            return true;
        }
        return true;
    }
    if (k64_streq(sub, "write") || k64_streq(sub, "append")) {
        char path[128];
        const char* text;
        uint8_t combined[512];
        size_t old_len = 0;
        size_t text_len;

        args = svc_next_token(args, path, sizeof(path));
        text = args ? args : "";
        text_len = k64_strlen(text);
        if (!xfsctl_require_mount() ||
            (k64_xfs_stat(&xfsctl_mount, xfsctl_path(path), &(k64_fs_stat_t){0}) &&
             !xfsctl_require_access(path, K64_ACCESS_WRITE)) ||
            (!k64_xfs_stat(&xfsctl_mount, xfsctl_path(path), &(k64_fs_stat_t){0}) &&
             !xfsctl_require_create(path))) {
            svc_print_line("xfsctl: write failed");
            return true;
        }
        if (k64_streq(sub, "append")) {
            (void)k64_xfs_read_file(&xfsctl_mount, xfsctl_path(path), combined, sizeof(combined), &old_len);
        }
        if (old_len + text_len > sizeof(combined)) {
            svc_print_line("xfsctl: write failed");
            return true;
        }
        memcpy(combined + old_len, text, text_len);
        if (!k64_xfs_write_file(&xfsctl_mount,
                                xfsctl_path(path),
                                combined,
                                old_len + text_len,
                                k64_user_effective_uid(),
                                k64_user_effective_gid())) {
            svc_print_line("xfsctl: write failed");
        }
        return true;
    }
    if (k64_streq(sub, "cat")) {
        uint8_t out[512];
        size_t read = 0;
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !xfsctl_require_access(a, K64_ACCESS_READ) ||
            !k64_xfs_read_file(&xfsctl_mount, xfsctl_path(a), out, sizeof(out) - 1u, &read)) {
            svc_print_line("xfsctl: cat failed");
            return true;
        }
        out[read] = 0;
        svc_print_line((const char*)out);
        return true;
    }
    if (k64_streq(sub, "rm")) {
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !xfsctl_require_access(a, K64_ACCESS_WRITE) ||
            !k64_xfs_remove(&xfsctl_mount, xfsctl_path(a))) {
            svc_print_line("xfsctl: rm failed");
        }
        return true;
    }
    if (k64_streq(sub, "rmdir")) {
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !xfsctl_require_access(a, K64_ACCESS_WRITE) ||
            !k64_xfs_rmdir(&xfsctl_mount, xfsctl_path(a))) {
            svc_print_line("xfsctl: rmdir failed");
        }
        return true;
    }
    if (k64_streq(sub, "chmod")) {
        char mode_text[16];
        uint32_t mode;
        if (!k64_user_is_root()) {
            svc_print_line("permission denied");
            return true;
        }
        args = svc_next_token(args, mode_text, sizeof(mode_text));
        args = svc_next_token(args, a, sizeof(a));
        if (!xfsctl_require_mount() || !svc_parse_octal_mode(mode_text, &mode) ||
            !k64_xfs_chmod(&xfsctl_mount, xfsctl_path(a), mode)) {
            svc_print_line("xfsctl: chmod failed");
        }
        return true;
    }
    if (k64_streq(sub, "chown")) {
        char owner[32];
        char user[16];
        char group[16];
        int split = -1;
        uint32_t uid;
        uint32_t gid;
        if (!k64_user_is_root()) {
            svc_print_line("permission denied");
            return true;
        }
        args = svc_next_token(args, owner, sizeof(owner));
        args = svc_next_token(args, a, sizeof(a));
        for (int i = 0; owner[i]; ++i) {
            if (owner[i] == ':') {
                split = i;
                break;
            }
        }
        if (split < 0) {
            svc_print_line("xfsctl: chown failed");
            return true;
        }
        owner[split] = '\0';
        svc_copy(user, sizeof(user), owner);
        svc_copy(group, sizeof(group), owner + split + 1);
        if (!xfsctl_require_mount() || !k64_user_name_to_uid(user, &uid) ||
            !k64_user_group_to_gid(group, &gid) ||
            !k64_xfs_chown(&xfsctl_mount, xfsctl_path(a), uid, gid)) {
            svc_print_line("xfsctl: chown failed");
        }
        return true;
    }
    if (k64_streq(sub, "sync")) {
        if (!xfsctl_require_mount() || !k64_xfs_sync(&xfsctl_mount)) {
            svc_print_line("xfsctl: sync failed");
            return true;
        }
        svc_print_line("K64XFS sync: OK");
        return true;
    }
    if (k64_streq(sub, "check")) {
        k64_xfs_check_report_t report;
        if (!xfsctl_require_mount() || !k64_xfs_check(&xfsctl_mount, &report)) {
            svc_print_line("xfsctl: check failed");
            return true;
        }
        svc_print_line(report.message);
        return true;
    }
    xfsctl_usage();
    return true;
}

static bool xfsctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "xfsctl", xfsctl_command);
    return true;
}

static void xfsctl_stop(k64_service_t* service) {
    (void)service;
}

static bool init_start(k64_service_t* service) {
    k64_service_result_t result;

    result = k64_system_start_service_by_name("servicectl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start servicectl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("driverctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start driverctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("storagectl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start storagectl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("netctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start netctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("kpm");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start kpm: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("fsctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start fsctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("xfsctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start xfsctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("userctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start userctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("sysfetch");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start sysfetch: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("uname");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start uname: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("k64cc");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start k64cc: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("elfctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start elfctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("shell");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("init: failed to start shell: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    return true;
}

static void init_stop(k64_service_t* service) {
    (void)service;
}

static bool reload_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "reload", reload_command);
    k64_term_write("[svc] reload started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void reload_stop(k64_service_t* service) {
    k64_term_write("[svc] reload stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static void reload_poll(k64_service_t* service, uint64_t now_ticks) {
    k64_reload_mode_t mode;

    (void)now_ticks;

    mode = k64_reload_take_request();
    if (mode == K64_RELOAD_NONE) {
        (void)k64_system_stop_service(service->pid);
        return;
    }

    if (mode == K64_RELOAD_DRIVERS) {
        k64_term_write("[svc] reload: reloading drivers\n");
        k64_modules_reload_all();
    } else if (mode == K64_RELOAD_KERNEL) {
        k64_term_write("[svc] reload: hot reloading kernel image\n");
        if (!k64_hotreload_kernel()) {
            k64_term_write("[svc] reload: hot reload failed\n");
        }
    }

    (void)k64_system_stop_service(service->pid);
}

static bool storagectl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "storagectl", storagectl_command);
    (void)k64_system_register_command(service->name, "sync", storagectl_command);
    (void)k64_system_register_command(service->name, "install", storagectl_command);
    return true;
}

static void storagectl_stop(k64_service_t* service) {
    (void)service;
}

static bool netctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "netctl", netctl_command);
    (void)k64_system_register_command(service->name, "ping", netctl_command);
    (void)k64_system_register_command(service->name, "kcurl", netctl_command);
    (void)k64_system_register_command(service->name, "udp", netctl_command);
    return true;
}

static void netctl_stop(k64_service_t* service) {
    (void)service;
}

void k64s_register_builtin_services(void) {
    k64_system_register_service("init",
                                "k64s/init.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                init_start,
                                init_stop,
                                NULL,
                                NULL);

    k64_system_register_service("servicectl",
                                "k64s/servicectl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                servicectl_start,
                                servicectl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("driverctl",
                                "k64s/driverctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                driverctl_start,
                                driverctl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("storagectl",
                                "k64s/storagectl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                storagectl_start,
                                storagectl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("netctl",
                                "k64s/netctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                netctl_start,
                                netctl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("kpm",
                                "k64s/kpm.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                k64_kpm_service_start,
                                k64_kpm_service_stop,
                                NULL,
                                NULL);

    k64_system_register_service("reload",
                                "k64s/reload.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                K64_SERVICE_FLAG_ASYNC,
                                1,
                                0,
                                true,
                                reload_start,
                                reload_stop,
                                reload_poll,
                                NULL);

    k64_system_register_service("fsctl",
                                "k64s/fsctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                fsctl_start,
                                fsctl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("xfsctl",
                                "k64s/xfsctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                xfsctl_start,
                                xfsctl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("userctl",
                                "k64s/userctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                k64_user_service_start,
                                k64_user_service_stop,
                                NULL,
                                NULL);

    k64_system_register_service("sysfetch",
                                "k64s/sysfetch.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                sysfetch_start,
                                sysfetch_stop,
                                NULL,
                                NULL);

    k64_system_register_service("uname",
                                "k64s/uname.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                uname_start,
                                uname_stop,
                                NULL,
                                NULL);

    k64_system_register_service("k64cc",
                                "k64s/k64cc.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                k64cc_start,
                                k64cc_stop,
                                NULL,
                                NULL);

    k64_system_register_service("elfctl",
                                "k64s/elfctl.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                0,
                                1,
                                0,
                                true,
                                elfctl_start,
                                elfctl_stop,
                                NULL,
                                NULL);

    k64_system_register_service("shell",
                                "k64s/shell.k64s",
                                K64_SERVICE_CLASS_SYSTEM,
                                K64_SERVICE_FLAG_ASYNC,
                                1,
                                0,
                                true,
                                k64_shell_service_start,
                                k64_shell_service_stop,
                                k64_shell_service_poll,
                                NULL);
}
