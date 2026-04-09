#include "k64_artifact.h"
#include "k64_elf.h"
#include "k64_config.h"
#include "k64_fs.h"
#include "k64_hotreload.h"
#include "k64_modules.h"
#include "k64_pit.h"
#include "k64_pmm.h"
#include "k64_reload.h"
#include "k64_shell.h"
#include "k64_string.h"
#include "k64_system.h"
#include "k64_terminal.h"
#include "k64_user.h"
#include "k64_version.h"
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

static void servicectl_usage(void) {
    svc_print_line("usage: servicectl <list|stopped|start|stop|restart> [pid]");
}

static void driverctl_usage(void) {
    svc_print_line("usage: driverctl <list|stopped|start|stop|restart> [id]");
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
    (void)command;
    if (!args || !args[0]) {
        svc_print_line("usage: elfrun </path/to/file.elf>");
        return true;
    }
    if (!k64_elf_execute_path(args)) {
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
    uint64_t disk_total = (uint64_t)k64_fs_capacity_bytes();
    uint64_t disk_used = (uint64_t)k64_fs_used_bytes();
    uint64_t disk_free = disk_total > disk_used ? (disk_total - disk_used) : 0;

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
    svc_print_mib_line("Memory Free", free_mem);
    svc_print_mib_line("Memory Total", total_mem);
    svc_print_mib_line("Disk Free", disk_free);
    svc_print_mib_line("Disk Total", disk_total);
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

    k64_term_write("PID   STATE    CLASS   NAME              VM BASE\n");
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
    k64_term_write("[svc] servicectl started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void servicectl_stop(k64_service_t* service) {
    k64_term_write("[svc] servicectl stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static bool driverctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "driverctl", driverctl_command);
    k64_term_write("[svc] driverctl started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static bool sysfetch_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "sysfetch", sysfetch_command);
    k64_term_write("[svc] sysfetch started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void sysfetch_stop(k64_service_t* service) {
    k64_term_write("[svc] sysfetch stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static bool k64cc_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "k64cc", k64cc_command);
    k64_term_write("[svc] k64cc started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void k64cc_stop(k64_service_t* service) {
    k64_term_write("[svc] k64cc stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static bool elfctl_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "elfrun", elfctl_command);
    k64_term_write("[svc] elfctl started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void elfctl_stop(k64_service_t* service) {
    k64_term_write("[svc] elfctl stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static bool uname_start(k64_service_t* service) {
    (void)k64_system_register_command(service->name, "uname", uname_command);
    k64_term_write("[svc] uname started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void uname_stop(k64_service_t* service) {
    k64_term_write("[svc] uname stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static void driverctl_stop(k64_service_t* service) {
    k64_term_write("[svc] driverctl stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static void fsctl_print(const char* text) {
    k64_term_write(text ? text : "");
    k64_term_putc('\n');
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
    if (!k64_fs_ls(args && args[0] ? args : NULL, buf, sizeof(buf))) {
        fsctl_print("ls failed");
        return true;
    }
    k64_term_write(buf);
    return true;
}

static bool fsctl_cd_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !k64_fs_cd(args)) {
        fsctl_print("cd failed");
        return true;
    }
    return true;
}

static bool fsctl_mkdir_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !k64_fs_mkdir(args)) {
        fsctl_print("mkdir failed");
        return true;
    }
    return true;
}

static bool fsctl_touch_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !k64_fs_touch(args)) {
        fsctl_print("touch failed");
        return true;
    }
    return true;
}

static bool fsctl_cat_handler(const char* command, const char* args) {
    char buf[512];
    (void)command;
    if (!args || !args[0] || !k64_fs_cat(args, buf, sizeof(buf))) {
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
    if (!path[0] || !k64_fs_write_file(path, rest)) {
        fsctl_print("write failed");
        return true;
    }
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
    if (!path[0] || !k64_fs_append_file(path, rest)) {
        fsctl_print("append failed");
        return true;
    }
    return true;
}

static bool fsctl_rm_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !k64_fs_remove(args)) {
        fsctl_print("rm failed");
        return true;
    }
    return true;
}

static bool fsctl_rmdir_handler(const char* command, const char* args) {
    (void)command;
    if (!args || !args[0] || !k64_fs_rmdir(args)) {
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
    if (!src[0] || !dst[0] || !k64_fs_move(src, dst)) {
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
    if (!src[0] || !dst[0] || !k64_fs_copy(src, dst)) {
        fsctl_print("cp failed");
        return true;
    }
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
    k64_term_putc('\n');
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
    (void)k64_system_register_command("fsctl", "touch", fsctl_touch_handler);
    (void)k64_system_register_command("fsctl", "cat", fsctl_cat_handler);
    (void)k64_system_register_command("fsctl", "write", fsctl_write_handler);
    (void)k64_system_register_command("fsctl", "append", fsctl_append_handler);
    (void)k64_system_register_command("fsctl", "rm", fsctl_rm_handler);
    (void)k64_system_register_command("fsctl", "rmdir", fsctl_rmdir_handler);
    (void)k64_system_register_command("fsctl", "mv", fsctl_mv_handler);
    (void)k64_system_register_command("fsctl", "cp", fsctl_cp_handler);
    (void)k64_system_register_command("fsctl", "stat", fsctl_stat_handler);
    k64_term_write("[svc] fsctl started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    return true;
}

static void fsctl_stop(k64_service_t* service) {
    k64_term_write("[svc] fsctl stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
}

static bool init_start(k64_service_t* service) {
    k64_service_result_t result;

    k64_term_write("[svc] init started pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');

    result = k64_system_start_service_by_name("servicectl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start servicectl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("driverctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start driverctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("fsctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start fsctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("userctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start userctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("sysfetch");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start sysfetch: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("uname");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start uname: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("k64cc");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start k64cc: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("elfctl");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start elfctl: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    result = k64_system_start_service_by_name("shell");
    if (result != K64_SERVICE_OK && result != K64_SERVICE_ERR_ALREADY_RUNNING) {
        k64_term_write("[svc] init failed to start shell: ");
        k64_term_write(k64_system_result_string(result));
        k64_term_putc('\n');
    }

    return true;
}

static void init_stop(k64_service_t* service) {
    k64_term_write("[svc] init stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
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
