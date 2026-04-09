// k64_shell.c
#include "k64_shell.h"
#include "k64_elf.h"
#include "k64_fs.h"
#include "k64_keyboard.h"
#include "k64_log.h"
#include "k64_modules.h"
#include "k64_pit.h"
#include "k64_power.h"
#include "k64_reload.h"
#include "k64_sched.h"
#include "k64_serial.h"
#include "k64_shell_cmd.h"
#include "k64_string.h"
#include "k64_system.h"
#include "k64_terminal.h"
#include "k64_user.h"

#define SHELL_MAX_LINE 128
#define SHELL_HISTORY_MAX 16
#define SHELL_EVENTS_PER_POLL 32

typedef struct {
    char line[SHELL_MAX_LINE];
    int len;
    int cursor;
    int rendered_len;
} shell_editor_t;

typedef struct {
    bool active;
    bool banner_printed;
    shell_editor_t editor;
} shell_runtime_t;

static shell_runtime_t shell_runtime;
static char shell_history[SHELL_HISTORY_MAX][SHELL_MAX_LINE];
static int shell_history_count = 0;
static int shell_history_next = 0;
static int shell_history_index = -1;
static char shell_saved_line[SHELL_MAX_LINE];
static bool shell_saved_line_valid = false;

static void shell_prompt(void) {
    char cwd[128];

    k64_term_write("[");
    k64_term_write(k64_user_effective_name());
    k64_term_write("]@");
    k64_term_setcolor(K64_COLOR_LIGHT_GREEN, K64_COLOR_BLACK);
    k64_term_write("K64");
    k64_term_setcolor(K64_COLOR_LIGHT_GREY, K64_COLOR_BLACK);
    k64_term_write(" ~");
    if (k64_fs_pwd(cwd, sizeof(cwd))) {
        k64_term_write(cwd);
    } else {
        k64_term_write("/");
    }
    k64_term_write(" >>> ");
}

static int shell_line_len(const char* s) {
    int len = 0;

    while (s && s[len]) {
        len++;
    }
    return len;
}

static void shell_copy_line(char* dst, const char* src) {
    int i = 0;

    if (!dst) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i < SHELL_MAX_LINE - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool shell_parse_u64(const char* s, uint64_t* out) {
    uint64_t value = 0;

    if (!s || !*s) {
        return false;
    }

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return false;
    }

    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        value = value * 10 + (uint64_t)(*s - '0');
        s++;
    }

    *out = value;
    return true;
}

static bool shell_try_execute_elf(const char* name) {
    char path[96];

    if (!name || !name[0]) {
        return false;
    }

    path[0] = '\0';
    {
        const char* prefix = "/ex/";
        int pos = 0;
        for (int i = 0; prefix[i] && pos + 1 < (int)sizeof(path); ++i) {
            path[pos++] = prefix[i];
        }
        for (int i = 0; name[i] && pos + 1 < (int)sizeof(path); ++i) {
            path[pos++] = name[i];
        }
        if (!k64_streq(name + (k64_strlen(name) >= 4 ? k64_strlen(name) - 4 : 0), ".elf")) {
            const char* suffix = ".elf";
            for (int i = 0; suffix[i] && pos + 1 < (int)sizeof(path); ++i) {
                path[pos++] = suffix[i];
            }
        }
        path[pos] = '\0';
    }

    return k64_elf_execute_path(path);
}

static const char* shell_next_token(const char* s, char* token, int token_size) {
    int i = 0;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    while (*s && *s != ' ' && *s != '\t' && i + 1 < token_size) {
        token[i++] = *s++;
    }
    token[i] = '\0';
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool shell_layout_from_name(const char* name, k64_keyboard_layout_t* out) {
    if (k64_streq(name, "us")) {
        *out = K64_KEYBOARD_LAYOUT_US;
        return true;
    }
    if (k64_streq(name, "de")) {
        *out = K64_KEYBOARD_LAYOUT_DE;
        return true;
    }
    return false;
}

static void shell_print_help(void) {
    k64_term_write("Commands:\n");
    k64_term_write("  help             - show this help\n");
    k64_term_write("  clear            - clear the console\n");
    k64_term_write("  sysfetch         - show system summary and splash\n");
    k64_term_write("  uname            - show kernel identity\n");
    k64_term_write("  k64cc            - build stub ELF files and K64 manifests\n");
    k64_term_write("  elfrun <path>    - execute an ELF file directly\n");
    k64_term_write("  ticks            - show PIT tick counter\n");
    k64_term_write("  task             - print current task id\n");
    k64_term_write("  serial           - print serial availability\n");
    k64_term_write("  sched            - dump scheduler stats\n");
    k64_term_write("  echo <text>      - print text back to the console\n");
    k64_term_write("  layout [us|de]   - show or switch keyboard layout\n");
    k64_term_write("  servicectl <cmd> - manage services\n");
    k64_term_write("  driverctl <cmd>  - manage drivers\n");
    k64_term_write("  reload <target>  - reload drivers or kernel runtime\n");
    k64_term_write("  whoami id users groups - inspect users, groups, and the current session\n");
    k64_term_write("  login logout su sudo   - switch or elevate the current session\n");
    k64_term_write("  passwd useradd userdel - manage user accounts\n");
    k64_term_write("  usermod groupadd groupdel gpasswd - manage roles and groups\n");
    k64_term_write("  reboot           - reboot the machine\n");
    k64_term_write("  shutdown         - power down the machine\n");
    k64_term_write("  pwd ls cd mkdir touch write append cat stat rm rmdir mv cp - filesystem commands from fsctl\n");
    k64_term_write("  yield            - give up the current timeslice\n");
    k64_term_write("  panic            - trigger kernel panic\n");
}

static void shell_history_add(const char* line) {
    int last;

    if (!line || !line[0]) {
        return;
    }

    if (shell_history_count > 0) {
        last = (shell_history_next + SHELL_HISTORY_MAX - 1) % SHELL_HISTORY_MAX;
        if (k64_streq(shell_history[last], line)) {
            return;
        }
    }

    shell_copy_line(shell_history[shell_history_next], line);
    shell_history_next = (shell_history_next + 1) % SHELL_HISTORY_MAX;
    if (shell_history_count < SHELL_HISTORY_MAX) {
        shell_history_count++;
    }
}

static void shell_editor_set(shell_editor_t* editor, const char* line) {
    shell_copy_line(editor->line, line);
    editor->len = shell_line_len(editor->line);
    editor->cursor = editor->len;
}

static void shell_editor_reset(shell_editor_t* editor) {
    editor->line[0] = '\0';
    editor->len = 0;
    editor->cursor = 0;
    editor->rendered_len = 0;
}

static void shell_editor_render(shell_editor_t* editor) {
    int clear_count;

    if (!shell_runtime.active) {
        return;
    }

    k64_term_putc('\r');
    shell_prompt();

    for (int i = 0; i < editor->len; ++i) {
        k64_term_putc(editor->line[i]);
    }

    clear_count = editor->rendered_len - editor->len;
    while (clear_count-- > 0) {
        k64_term_putc(' ');
    }

    k64_term_putc('\r');
    shell_prompt();
    for (int i = 0; i < editor->cursor; ++i) {
        k64_term_putc(editor->line[i]);
    }

    editor->rendered_len = editor->len;
}

static void shell_editor_insert(shell_editor_t* editor, char ch) {
    if (editor->len >= SHELL_MAX_LINE - 1) {
        return;
    }
    for (int i = editor->len; i > editor->cursor; --i) {
        editor->line[i] = editor->line[i - 1];
    }
    editor->line[editor->cursor] = ch;
    editor->len++;
    editor->cursor++;
    editor->line[editor->len] = '\0';
}

static void shell_editor_backspace(shell_editor_t* editor) {
    if (editor->cursor <= 0) {
        return;
    }
    for (int i = editor->cursor - 1; i < editor->len; ++i) {
        editor->line[i] = editor->line[i + 1];
    }
    editor->cursor--;
    editor->len--;
}

static void shell_editor_delete(shell_editor_t* editor) {
    if (editor->cursor >= editor->len) {
        return;
    }
    for (int i = editor->cursor; i < editor->len; ++i) {
        editor->line[i] = editor->line[i + 1];
    }
    editor->len--;
}

static void shell_history_step(shell_editor_t* editor, int direction) {
    int oldest;

    if (shell_history_count == 0) {
        return;
    }

    oldest = (shell_history_next - shell_history_count + SHELL_HISTORY_MAX) % SHELL_HISTORY_MAX;

    if (direction < 0) {
        if (shell_history_index < 0) {
            shell_copy_line(shell_saved_line, editor->line);
            shell_saved_line_valid = true;
            shell_history_index = (shell_history_next + SHELL_HISTORY_MAX - 1) % SHELL_HISTORY_MAX;
        } else if (shell_history_index != oldest) {
            shell_history_index = (shell_history_index + SHELL_HISTORY_MAX - 1) % SHELL_HISTORY_MAX;
        }
        shell_editor_set(editor, shell_history[shell_history_index]);
        return;
    }

    if (shell_history_index < 0) {
        return;
    }

    if (shell_history_index == (shell_history_next + SHELL_HISTORY_MAX - 1) % SHELL_HISTORY_MAX) {
        shell_history_index = -1;
        if (shell_saved_line_valid) {
            shell_editor_set(editor, shell_saved_line);
        } else {
            shell_editor_reset(editor);
        }
        return;
    }

    shell_history_index = (shell_history_index + 1) % SHELL_HISTORY_MAX;
    shell_editor_set(editor, shell_history[shell_history_index]);
}

static void shell_servicectl_usage(void) {
    k64_term_write("Usage:\n");
    k64_term_write("  servicectl list\n");
    k64_term_write("  servicectl list stopped\n");
    k64_term_write("  servicectl stopped\n");
    k64_term_write("  servicectl start <pid>\n");
    k64_term_write("  servicectl stop <pid>\n");
    k64_term_write("  servicectl restart <pid>\n");
}

static void shell_driverctl_usage(void) {
    k64_term_write("Usage:\n");
    k64_term_write("  driverctl list\n");
    k64_term_write("  driverctl list stopped\n");
    k64_term_write("  driverctl stopped\n");
    k64_term_write("  driverctl start <id>\n");
    k64_term_write("  driverctl stop <id>\n");
    k64_term_write("  driverctl restart <id>\n");
}

static void shell_servicectl_list(bool stopped_only) {
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
        for (int pad = shell_line_len(service->name); pad < 16; ++pad) {
            k64_term_putc(' ');
        }
        k64_term_write("  ");
        k64_term_write_hex(service->vm_space.root_base);
        k64_term_putc('\n');
    }
}

static void shell_driverctl_list(bool stopped_only) {
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
        for (int pad = shell_line_len(driver->name); pad < 16; ++pad) {
            k64_term_putc(' ');
        }
        k64_term_write("  ");
        k64_term_write(driver->source);
        k64_term_putc('\n');
    }
}

static void shell_servicectl_action(const char* action, uint64_t pid) {
    k64_service_result_t result;
    k64_service_t* service = k64_system_find_service(pid);

    if (!service) {
        k64_term_write("Service action failed for pid ");
        k64_term_write_dec(pid);
        k64_term_write(": service not found\n");
        return;
    }
    if (!k64_user_can_manage_service(service)) {
        k64_term_write("Service action failed for pid ");
        k64_term_write_dec(pid);
        k64_term_write(": permission denied\n");
        return;
    }

    if (k64_streq(action, "start")) {
        result = k64_system_start_service(pid);
    } else if (k64_streq(action, "stop")) {
        result = k64_system_stop_service(pid);
    } else if (k64_streq(action, "restart")) {
        result = k64_system_restart_service(pid);
    } else {
        shell_servicectl_usage();
        return;
    }

    if (result == K64_SERVICE_OK) {
        k64_term_write("Service action completed for pid ");
        k64_term_write_dec(pid);
        k64_term_putc('\n');
        return;
    }

    k64_term_write("Service action failed for pid ");
    k64_term_write_dec(pid);
    k64_term_write(": ");
    k64_term_write(k64_system_result_string(result));
    k64_term_putc('\n');
}

static void shell_driverctl_action(const char* action, uint64_t id) {
    k64_driver_result_t result;

    if (!k64_user_can_manage_drivers()) {
        k64_term_write("Driver action failed for id ");
        k64_term_write_dec(id);
        k64_term_write(": permission denied\n");
        return;
    }

    if (k64_streq(action, "start")) {
        result = k64_modules_start_driver(id);
    } else if (k64_streq(action, "stop")) {
        result = k64_modules_stop_driver(id);
    } else if (k64_streq(action, "restart")) {
        result = k64_modules_restart_driver(id);
    } else {
        shell_driverctl_usage();
        return;
    }

    if (result == K64_DRIVER_OK) {
        k64_term_write("Driver action completed for id ");
        k64_term_write_dec(id);
        k64_term_putc('\n');
        return;
    }

    k64_term_write("Driver action failed for id ");
    k64_term_write_dec(id);
    k64_term_write(": ");
    k64_term_write(k64_modules_result_string(result));
    k64_term_putc('\n');
}

static void shell_handle_servicectl(const char* args) {
    char cmd[16];
    char pid_buf[24];
    uint64_t pid;

    if (!k64_system_control_plane_online()) {
        if (!args || !args[0]) {
            k64_service_result_t result = k64_system_start_service_by_name("servicectl");
            if (result == K64_SERVICE_OK || result == K64_SERVICE_ERR_ALREADY_RUNNING) {
                k64_term_write("Started service: servicectl\n");
                return;
            }
            k64_term_write("Failed to start servicectl: ");
            k64_term_write(k64_system_result_string(result));
            k64_term_putc('\n');
            return;
        }
        k64_term_write("servicectl service is stopped. Type 'servicectl' to start it.\n");
        return;
    }

    args = shell_next_token(args, cmd, sizeof(cmd));
    if (!cmd[0]) {
        shell_servicectl_usage();
        return;
    }

    if (k64_streq(cmd, "list")) {
        char extra[16];
        shell_next_token(args, extra, sizeof(extra));
        shell_servicectl_list(k64_streq(extra, "stopped"));
        return;
    }
    if (k64_streq(cmd, "stopped")) {
        shell_servicectl_list(true);
        return;
    }

    args = shell_next_token(args, pid_buf, sizeof(pid_buf));
    if (!shell_parse_u64(pid_buf, &pid)) {
        shell_servicectl_usage();
        return;
    }

    shell_servicectl_action(cmd, pid);
}

static void shell_handle_driverctl(const char* args) {
    char cmd[16];
    char id_buf[24];
    uint64_t id;

    if (!k64_system_is_service_running("driverctl")) {
        if (!args || !args[0]) {
            k64_service_result_t result = k64_system_start_service_by_name("driverctl");
            if (result == K64_SERVICE_OK || result == K64_SERVICE_ERR_ALREADY_RUNNING) {
                k64_term_write("Started service: driverctl\n");
                return;
            }
        }
        k64_term_write("driverctl service is stopped. Type 'driverctl' to start it.\n");
        return;
    }

    args = shell_next_token(args, cmd, sizeof(cmd));
    if (!cmd[0]) {
        shell_driverctl_usage();
        return;
    }

    if (k64_streq(cmd, "list")) {
        char extra[16];
        shell_next_token(args, extra, sizeof(extra));
        shell_driverctl_list(k64_streq(extra, "stopped"));
        return;
    }
    if (k64_streq(cmd, "stopped")) {
        shell_driverctl_list(true);
        return;
    }

    args = shell_next_token(args, id_buf, sizeof(id_buf));
    if (!shell_parse_u64(id_buf, &id)) {
        shell_driverctl_usage();
        return;
    }

    shell_driverctl_action(cmd, id);
}

static void shell_handle_reload(const char* args) {
    char target[16];
    k64_reload_mode_t mode;

    if (!k64_user_is_root()) {
        k64_term_write("permission denied\n");
        return;
    }

    args = shell_next_token(args, target, sizeof(target));
    if (!target[0]) {
        k64_term_write("Usage: reload <drivers|kernel>\n");
        return;
    }

    if (k64_streq(target, "drivers")) {
        mode = K64_RELOAD_DRIVERS;
    } else if (k64_streq(target, "kernel")) {
        mode = K64_RELOAD_KERNEL;
    } else {
        k64_term_write("Usage: reload <drivers|kernel>\n");
        return;
    }

    if (!k64_reload_request(mode)) {
        k64_term_write("Reload request failed.\n");
        return;
    }

    k64_term_write("Reload request submitted for ");
    k64_term_write(target);
    k64_term_putc('\n');
}

static bool shell_serial_get_event(k64_key_event_t* event) {
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
        if (c == '\n' || c == '\r') {
            event->type = K64_KEY_ENTER;
            event->ch = '\n';
            return true;
        }
        if (c == '\b' || c == 127) {
            event->type = K64_KEY_BACKSPACE;
            event->ch = '\b';
            return true;
        }
        event->type = K64_KEY_CHAR;
        event->ch = c;
        return true;
    }

    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            return false;
        }
        esc_state = 0;
        event->type = K64_KEY_CHAR;
        event->ch = c;
        return true;
    }

    esc_state = 0;
    switch (c) {
        case 'A': event->type = K64_KEY_UP; return true;
        case 'B': event->type = K64_KEY_DOWN; return true;
        case 'C': event->type = K64_KEY_RIGHT; return true;
        case 'D': event->type = K64_KEY_LEFT; return true;
        case '3': {
            char tail = 0;
            if (k64_serial_get_char(&tail) && tail == '~') {
                event->type = K64_KEY_DELETE;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool shell_poll_event(k64_key_event_t* event) {
    if (k64_keyboard_get_event(event)) {
        return true;
    }
    return shell_serial_get_event(event);
}

static void shell_handle_command(const char* cmd) {
    const char* arg = "";
    k64_shell_cmd_t shell_cmd = k64_shell_parse_command(cmd, &arg);
    k64_keyboard_layout_t layout;

    switch (shell_cmd) {
        case K64_SHELL_CMD_EMPTY:
            return;
        case K64_SHELL_CMD_HELP:
            shell_print_help();
            return;
        case K64_SHELL_CMD_CLEAR:
            k64_term_clear();
            return;
        case K64_SHELL_CMD_TICKS:
            k64_term_write("PIT ticks: ");
            k64_term_write_dec(k64_pit_get_ticks());
            k64_term_putc('\n');
            return;
        case K64_SHELL_CMD_TASK: {
            k64_task_t* task = k64_sched_current_task();
            k64_term_write("Current task id: ");
            k64_term_write_dec(task ? task->id : 0);
            k64_term_putc('\n');
            return;
        }
        case K64_SHELL_CMD_SERIAL:
            k64_term_write("Serial: ");
            k64_term_write(k64_serial_is_ready() ? "ready" : "unavailable");
            k64_term_putc('\n');
            return;
        case K64_SHELL_CMD_SCHED:
            k64_sched_dump_stats();
            return;
        case K64_SHELL_CMD_PANIC:
            k64_panic("Shell requested panic");
            return;
        case K64_SHELL_CMD_ECHO:
            k64_term_write(arg);
            k64_term_putc('\n');
            return;
        case K64_SHELL_CMD_YIELD:
            k64_sched_yield();
            return;
        case K64_SHELL_CMD_LAYOUT:
            if (!arg[0]) {
                k64_term_write("Current layout: ");
                k64_term_write(k64_keyboard_layout_name());
                k64_term_putc('\n');
                return;
            }
            if (!shell_layout_from_name(arg, &layout)) {
                k64_term_write("Unknown layout. Available: us, de\n");
                return;
            }
            k64_keyboard_set_layout(layout);
            k64_term_write("Keyboard layout switched to ");
            k64_term_write(k64_keyboard_layout_name());
            k64_term_putc('\n');
            return;
        case K64_SHELL_CMD_SERVICECTL:
            shell_handle_servicectl(arg);
            return;
        case K64_SHELL_CMD_DRIVERCTL:
            shell_handle_driverctl(arg);
            return;
        case K64_SHELL_CMD_RELOAD:
            shell_handle_reload(arg);
            return;
        case K64_SHELL_CMD_REBOOT:
            k64_term_write("Rebooting...\n");
            k64_power_reboot();
        case K64_SHELL_CMD_SHUTDOWN:
            k64_term_write("Shutting down...\n");
            k64_power_shutdown();
        case K64_SHELL_CMD_UNKNOWN: {
            char unknown_cmd[32];
            const char* unknown_args = shell_next_token(cmd, unknown_cmd, sizeof(unknown_cmd));

            if (k64_system_dispatch_command(unknown_cmd, unknown_args)) {
                return;
            }
            {
                k64_service_t* target_service = k64_system_find_service_by_name(unknown_cmd);
                k64_service_result_t service_result;
                if (target_service && !k64_user_can_manage_service(target_service)) {
                    k64_term_write("Permission denied: ");
                    k64_term_write(unknown_cmd);
                    k64_term_putc('\n');
                    return;
                }
                service_result = k64_system_start_service_by_name(unknown_cmd);
                if (service_result == K64_SERVICE_OK || service_result == K64_SERVICE_ERR_ALREADY_RUNNING) {
                    if (service_result == K64_SERVICE_ERR_ALREADY_RUNNING) {
                        k64_term_write("Service already running: ");
                    } else {
                        k64_term_write("Started service: ");
                    }
                    k64_term_write(unknown_cmd);
                    k64_term_putc('\n');
                    return;
                }
            }

            {
                k64_driver_result_t driver_result = k64_modules_start_driver_by_name(unknown_cmd);
                if (driver_result == K64_DRIVER_OK || driver_result == K64_DRIVER_ERR_ALREADY_RUNNING) {
                    if (driver_result == K64_DRIVER_ERR_ALREADY_RUNNING) {
                        k64_term_write("Driver already running: ");
                    } else {
                        k64_term_write("Started driver: ");
                    }
                    k64_term_write(unknown_cmd);
                    k64_term_putc('\n');
                    return;
                }
            }

            if (shell_try_execute_elf(unknown_cmd)) {
                return;
            }

            k64_term_write("Unknown command: ");
            k64_term_write(unknown_cmd);
            k64_term_putc('\n');
            return;
        }
    }
}

static void shell_process_event(k64_key_event_t* event) {
    shell_editor_t* editor = &shell_runtime.editor;

    switch (event->type) {
        case K64_KEY_ENTER:
            k64_term_putc('\n');
            shell_history_add(editor->line);
            shell_handle_command(editor->line);
            if (!shell_runtime.active) {
                return;
            }
            shell_editor_reset(editor);
            shell_history_index = -1;
            shell_saved_line_valid = false;
            shell_editor_render(editor);
            break;
        case K64_KEY_BACKSPACE:
            shell_editor_backspace(editor);
            shell_editor_render(editor);
            break;
        case K64_KEY_DELETE:
            shell_editor_delete(editor);
            shell_editor_render(editor);
            break;
        case K64_KEY_LEFT:
            if (editor->cursor > 0) {
                editor->cursor--;
                shell_editor_render(editor);
            }
            break;
        case K64_KEY_RIGHT:
            if (editor->cursor < editor->len) {
                editor->cursor++;
                shell_editor_render(editor);
            }
            break;
        case K64_KEY_UP:
            shell_history_step(editor, -1);
            shell_editor_render(editor);
            break;
        case K64_KEY_DOWN:
            shell_history_step(editor, 1);
            shell_editor_render(editor);
            break;
        case K64_KEY_CHAR:
            shell_editor_insert(editor, event->ch);
            shell_editor_render(editor);
            break;
        case K64_KEY_TAB:
        case K64_KEY_NONE:
        default:
            break;
    }
}

bool k64_shell_service_start(struct k64_service* service) {
    (void)service;

    shell_runtime.active = true;
    shell_runtime.banner_printed = false;
    shell_editor_reset(&shell_runtime.editor);
    shell_history_index = -1;
    shell_saved_line_valid = false;
    return true;
}

void k64_shell_service_stop(struct k64_service* service) {
    k64_term_putc('\n');
    k64_term_write("[svc] shell stopped pid=");
    k64_term_write_dec(service->pid);
    k64_term_putc('\n');
    shell_runtime.active = false;
}

void k64_shell_service_poll(struct k64_service* service, uint64_t now_ticks) {
    k64_key_event_t event;

    (void)service;
    (void)now_ticks;

    if (!shell_runtime.active) {
        return;
    }

    if (!shell_runtime.banner_printed) {
        k64_term_write("K64 shell started. Type 'help' for commands.\n");
        shell_editor_render(&shell_runtime.editor);
        shell_runtime.banner_printed = true;
    }

    for (int i = 0; i < SHELL_EVENTS_PER_POLL; ++i) {
        if (!shell_poll_event(&event)) {
            break;
        }
        shell_process_event(&event);
        if (!shell_runtime.active) {
            break;
        }
    }
}
