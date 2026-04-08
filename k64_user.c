#include "k64_user.h"
#include "k64_fs.h"
#include "k64_string.h"
#include "k64_terminal.h"

#define K64_USER_MAX_ACCOUNTS 8
#define K64_USER_NAME_MAX 16
#define K64_USER_PASS_MAX 40
#define K64_GROUP_MAX 16
#define K64_GROUP_NAME_MAX 16
#define K64_USER_DB_PATH "/etc/users.k64"
#define K64_GROUP_DB_PATH "/etc/groups.k64"

typedef struct {
    char name[K64_USER_NAME_MAX];
    char password[K64_USER_PASS_MAX];
    char primary_group[K64_GROUP_NAME_MAX];
    bool is_root;
    bool sudoer;
} k64_user_account_t;

typedef struct {
    char name[K64_GROUP_NAME_MAX];
    bool members[K64_USER_MAX_ACCOUNTS];
} k64_user_group_t;

typedef struct {
    bool ready;
    int count;
    int group_count;
    int current_index;
    bool sudo_active;
    k64_user_account_t accounts[K64_USER_MAX_ACCOUNTS];
    k64_user_group_t groups[K64_GROUP_MAX];
} k64_user_state_t;

static k64_user_state_t user_state;
static void user_copy(char* dst, int dst_size, const char* src);
static int user_len(const char* s);

static bool user_hash_is_encoded(const char* value) {
    return value && k64_strncmp(value, "k64$", 4) == 0;
}

static void user_hash_password(const char* user_name, const char* password, char* out, int out_size) {
    static const char* hex = "0123456789abcdef";
    uint64_t hash = 1469598103934665603ULL;
    const char* pepper = "K64:userctl:v1";
    int pos = 0;

    if (!out || out_size <= 0) {
        return;
    }

    for (int i = 0; pepper[i]; ++i) {
        hash ^= (uint64_t)(unsigned char)pepper[i];
        hash *= 1099511628211ULL;
    }
    for (int i = 0; user_name && user_name[i]; ++i) {
        hash ^= (uint64_t)(unsigned char)user_name[i];
        hash *= 1099511628211ULL;
    }
    hash ^= (uint64_t)':';
    hash *= 1099511628211ULL;
    for (int i = 0; password && password[i]; ++i) {
        hash ^= (uint64_t)(unsigned char)password[i];
        hash *= 1099511628211ULL;
    }

    if (out_size < 5) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = 'k';
    out[1] = '6';
    out[2] = '4';
    out[3] = '$';
    pos = 4;
    for (int shift = 60; shift >= 0 && pos + 1 < out_size; shift -= 4) {
        out[pos++] = hex[(hash >> shift) & 0xFULL];
    }
    out[pos] = '\0';
}

static void user_store_password(char* dst,
                                int dst_size,
                                const char* user_name,
                                const char* password_value,
                                bool already_hashed) {
    if (already_hashed && user_hash_is_encoded(password_value)) {
        user_copy(dst, dst_size, password_value);
        return;
    }
    user_hash_password(user_name ? user_name : "", password_value ? password_value : "", dst, dst_size);
}

static bool user_password_matches(const k64_user_account_t* account, const char* candidate) {
    char encoded[K64_USER_PASS_MAX];

    if (!account || !candidate) {
        return false;
    }
    if (!user_hash_is_encoded(account->password)) {
        return k64_streq(account->password, candidate);
    }

    user_hash_password(account->name, candidate, encoded, sizeof(encoded));
    return k64_streq(account->password, encoded);
}

static void user_copy(char* dst, int dst_size, const char* src) {
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

static int user_len(const char* s) {
    int n = 0;

    while (s && s[n]) {
        n++;
    }
    return n;
}

static void user_append(char* dst, int dst_size, const char* src) {
    int pos = user_len(dst);
    int i = 0;

    while (src && src[i] && pos + 1 < dst_size) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static const char* user_skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static const char* user_next_token(const char* s, char* token, int token_size) {
    int i = 0;

    s = user_skip_ws(s);
    while (s && *s && *s != ' ' && *s != '\t' && i + 1 < token_size) {
        token[i++] = *s++;
    }
    token[i] = '\0';
    return user_skip_ws(s);
}

static void user_print_line(const char* line) {
    k64_term_write(line ? line : "");
    k64_term_putc('\n');
}

static bool user_dir_exists(const char* path) {
    char buf[64];
    return k64_fs_ls(path, buf, sizeof(buf));
}

static void user_ensure_dir(const char* path) {
    if (!user_dir_exists(path)) {
        (void)k64_fs_mkdir(path);
    }
}

static void user_home_path(const char* name, char* out, int out_size) {
    out[0] = '\0';
    user_append(out, out_size, "/usr/");
    user_append(out, out_size, name);
}

static int user_find(const char* name) {
    for (int i = 0; i < user_state.count; ++i) {
        if (k64_streq(user_state.accounts[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int group_find(const char* name) {
    for (int i = 0; i < user_state.group_count; ++i) {
        if (k64_streq(user_state.groups[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool group_add_internal(const char* name) {
    int idx;

    if (!name || !name[0]) {
        return false;
    }
    idx = group_find(name);
    if (idx >= 0) {
        return true;
    }
    if (user_state.group_count >= K64_GROUP_MAX) {
        return false;
    }

    idx = user_state.group_count++;
    user_copy(user_state.groups[idx].name, K64_GROUP_NAME_MAX, name);
    for (int i = 0; i < K64_USER_MAX_ACCOUNTS; ++i) {
        user_state.groups[idx].members[i] = false;
    }
    return true;
}

static void group_set_membership(int group_idx, int user_idx, bool member) {
    if (group_idx < 0 || group_idx >= user_state.group_count ||
        user_idx < 0 || user_idx >= user_state.count) {
        return;
    }
    user_state.groups[group_idx].members[user_idx] = member;
}

static bool group_is_member(int group_idx, int user_idx) {
    if (group_idx < 0 || group_idx >= user_state.group_count ||
        user_idx < 0 || user_idx >= user_state.count) {
        return false;
    }
    return user_state.groups[group_idx].members[user_idx];
}

static bool user_add_to_group(const char* user_name, const char* group_name) {
    int user_idx = user_find(user_name);
    int group_idx;

    if (user_idx < 0) {
        return false;
    }
    if (!group_add_internal(group_name)) {
        return false;
    }
    group_idx = group_find(group_name);
    if (group_idx < 0) {
        return false;
    }
    group_set_membership(group_idx, user_idx, true);
    return true;
}

static bool user_remove_from_group(const char* user_name, const char* group_name) {
    int user_idx = user_find(user_name);
    int group_idx = group_find(group_name);

    if (user_idx < 0 || group_idx < 0) {
        return false;
    }
    if (k64_streq(user_state.accounts[user_idx].primary_group, group_name)) {
        return false;
    }
    group_set_membership(group_idx, user_idx, false);
    return true;
}

static void user_collect_groups(int user_idx, char* out, int out_size) {
    bool first = true;

    out[0] = '\0';
    if (user_idx < 0 || user_idx >= user_state.count) {
        return;
    }

    for (int i = 0; i < user_state.group_count; ++i) {
        if (!group_is_member(i, user_idx)) {
            continue;
        }
        if (!first) {
            user_append(out, out_size, ",");
        }
        user_append(out, out_size, user_state.groups[i].name);
        first = false;
    }
}

static bool user_save_groups_db(void);

static bool user_save_db(void) {
    char buf[1024];

    buf[0] = '\0';
    for (int i = 0; i < user_state.count; ++i) {
        user_append(buf, sizeof(buf), user_state.accounts[i].name);
        user_append(buf, sizeof(buf), ":");
        user_append(buf, sizeof(buf), user_state.accounts[i].password);
        user_append(buf, sizeof(buf), ":");
        user_append(buf, sizeof(buf), user_state.accounts[i].is_root ? "root" : "user");
        user_append(buf, sizeof(buf), ":");
        user_append(buf, sizeof(buf), user_state.accounts[i].sudoer ? "1" : "0");
        user_append(buf, sizeof(buf), ":");
        user_append(buf, sizeof(buf), user_state.accounts[i].primary_group);
        user_append(buf, sizeof(buf), "\n");
    }

    (void)k64_fs_touch(K64_USER_DB_PATH);
    return k64_fs_write_file(K64_USER_DB_PATH, buf);
}

static bool user_save_groups_db(void) {
    char buf[1024];

    buf[0] = '\0';
    for (int i = 0; i < user_state.group_count; ++i) {
        bool first = true;

        user_append(buf, sizeof(buf), user_state.groups[i].name);
        user_append(buf, sizeof(buf), ":");
        for (int j = 0; j < user_state.count; ++j) {
            if (!user_state.groups[i].members[j]) {
                continue;
            }
            if (!first) {
                user_append(buf, sizeof(buf), ",");
            }
            user_append(buf, sizeof(buf), user_state.accounts[j].name);
            first = false;
        }
        user_append(buf, sizeof(buf), "\n");
    }

    (void)k64_fs_touch(K64_GROUP_DB_PATH);
    return k64_fs_write_file(K64_GROUP_DB_PATH, buf);
}

static bool user_save_state(void) {
    return user_save_db() && user_save_groups_db();
}

static void user_reset_state(void) {
    user_state.count = 0;
    user_state.group_count = 0;
    user_state.current_index = -1;
    user_state.sudo_active = false;
}

static bool user_add_account(const char* name,
                             const char* password,
                             bool is_root,
                             bool sudoer,
                             const char* primary_group,
                             bool password_is_hashed) {
    int idx;

    if (!name || !name[0] || !password || !password[0]) {
        return false;
    }
    if (primary_group && primary_group[0] && !group_add_internal(primary_group)) {
        return false;
    }

    idx = user_find(name);
    if (idx >= 0) {
        user_store_password(user_state.accounts[idx].password,
                            K64_USER_PASS_MAX,
                            name,
                            password,
                            password_is_hashed);
        user_state.accounts[idx].is_root = is_root;
        user_state.accounts[idx].sudoer = sudoer || is_root;
        user_copy(user_state.accounts[idx].primary_group,
                  K64_GROUP_NAME_MAX,
                  (primary_group && primary_group[0]) ? primary_group : name);
        return true;
    }
    if (user_state.count >= K64_USER_MAX_ACCOUNTS) {
        return false;
    }

    idx = user_state.count++;
    user_copy(user_state.accounts[idx].name, K64_USER_NAME_MAX, name);
    user_store_password(user_state.accounts[idx].password,
                        K64_USER_PASS_MAX,
                        name,
                        password,
                        password_is_hashed);
    user_state.accounts[idx].is_root = is_root;
    user_state.accounts[idx].sudoer = sudoer || is_root;
    user_copy(user_state.accounts[idx].primary_group,
              K64_GROUP_NAME_MAX,
              (primary_group && primary_group[0]) ? primary_group : name);
    return true;
}

static void user_sync_role_groups(void) {
    int root_group;
    int sudo_group;

    (void)group_add_internal("root");
    (void)group_add_internal("sudo");
    root_group = group_find("root");
    sudo_group = group_find("sudo");

    for (int i = 0; i < user_state.count; ++i) {
        if (user_state.accounts[i].is_root) {
            group_set_membership(root_group, i, true);
        } else {
            group_set_membership(root_group, i, false);
        }
        if (user_state.accounts[i].sudoer || user_state.accounts[i].is_root) {
            group_set_membership(sudo_group, i, true);
        } else {
            group_set_membership(sudo_group, i, false);
        }
    }
}

static void user_ensure_primary_groups(void) {
    for (int i = 0; i < user_state.count; ++i) {
        if (!user_state.accounts[i].primary_group[0]) {
            user_copy(user_state.accounts[i].primary_group,
                      K64_GROUP_NAME_MAX,
                      user_state.accounts[i].name);
        }
        (void)group_add_internal(user_state.accounts[i].primary_group);
        group_set_membership(group_find(user_state.accounts[i].primary_group), i, true);
    }
}

static void user_seed_defaults(void) {
    user_reset_state();
    (void)group_add_internal("root");
    (void)group_add_internal("sudo");
    (void)user_add_account("root", "root", true, true, "root", false);
    (void)user_add_account("guest", "guest", false, true, "guest", false);
    user_ensure_primary_groups();
    user_sync_role_groups();
    user_state.current_index = user_find("guest");
    user_state.sudo_active = false;
}

static bool user_load_db(void) {
    char buf[1024];
    const char* p;

    if (!k64_fs_cat(K64_USER_DB_PATH, buf, sizeof(buf)) || !buf[0]) {
        user_seed_defaults();
        return user_save_state();
    }

    user_reset_state();
    p = buf;
    while (*p) {
        char line[160];
        char fields[5][32];
        int li = 0;
        int field = 0;
        int fi = 0;

        for (int i = 0; i < 5; ++i) {
            fields[i][0] = '\0';
        }

        while (*p && *p != '\n' && li + 1 < (int)sizeof(line)) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') {
            p++;
        }
        if (!line[0]) {
            continue;
        }

        for (int i = 0; ; ++i) {
            char ch = line[i];
            if (ch == ':' || ch == '\0') {
                if (field < 5) {
                    if (fi < (int)sizeof(fields[field])) {
                        fields[field][fi] = '\0';
                    } else {
                        fields[field][sizeof(fields[field]) - 1] = '\0';
                    }
                }
                field++;
                fi = 0;
                if (ch == '\0') {
                    break;
                }
                continue;
            }
            if (field < 5 && fi + 1 < (int)sizeof(fields[field])) {
                fields[field][fi++] = ch;
            }
        }

        if (!fields[0][0] || !fields[1][0]) {
            continue;
        }
        (void)user_add_account(fields[0],
                               fields[1],
                               k64_streq(fields[2], "root"),
                               fields[3][0] == '1',
                               fields[4][0] ? fields[4] : fields[0],
                               user_hash_is_encoded(fields[1]));
    }

    if (user_state.count == 0) {
        user_seed_defaults();
        return user_save_state();
    }

    return true;
}

static bool user_load_groups_db(void) {
    char buf[1024];
    const char* p;

    if (!k64_fs_cat(K64_GROUP_DB_PATH, buf, sizeof(buf)) || !buf[0]) {
        user_ensure_primary_groups();
        user_sync_role_groups();
        return user_save_groups_db();
    }

    p = buf;
    while (*p) {
        char line[160];
        char group_name[K64_GROUP_NAME_MAX];
        char members[96];
        int li = 0;
        int split = -1;

        group_name[0] = '\0';
        members[0] = '\0';

        while (*p && *p != '\n' && li + 1 < (int)sizeof(line)) {
            line[li++] = *p++;
        }
        line[li] = '\0';
        if (*p == '\n') {
            p++;
        }
        if (!line[0]) {
            continue;
        }

        for (int i = 0; line[i]; ++i) {
            if (line[i] == ':') {
                split = i;
                break;
            }
        }
        if (split < 0) {
            user_copy(group_name, sizeof(group_name), line);
        } else {
            line[split] = '\0';
            user_copy(group_name, sizeof(group_name), line);
            user_copy(members, sizeof(members), line + split + 1);
        }
        if (!group_name[0]) {
            continue;
        }
        if (!group_add_internal(group_name)) {
            continue;
        }

        if (members[0]) {
            const char* mp = members;
            while (*mp) {
                char user_name[K64_USER_NAME_MAX];
                int user_idx;
                int ti = 0;

                while (*mp == ',' || *mp == ' ') {
                    mp++;
                }
                while (*mp && *mp != ',' && ti + 1 < (int)sizeof(user_name)) {
                    user_name[ti++] = *mp++;
                }
                user_name[ti] = '\0';
                while (*mp && *mp != ',') {
                    mp++;
                }
                if (*mp == ',') {
                    mp++;
                }
                user_idx = user_find(user_name);
                if (user_idx >= 0) {
                    group_set_membership(group_find(group_name), user_idx, true);
                }
            }
        }
    }

    user_ensure_primary_groups();
    user_sync_role_groups();
    return true;
}

static void user_ensure_layout(void) {
    char path[64];

    user_ensure_dir("/bin");
    user_ensure_dir("/dev");
    user_ensure_dir("/etc");
    user_ensure_dir("/home");
    user_ensure_dir("/lib");
    user_ensure_dir("/mnt");
    user_ensure_dir("/proc");
    user_ensure_dir("/run");
    user_ensure_dir("/srv");
    user_ensure_dir("/tmp");
    user_ensure_dir("/usr");
    user_ensure_dir("/var");

    for (int i = 0; i < user_state.count; ++i) {
        user_home_path(user_state.accounts[i].name, path, sizeof(path));
        user_ensure_dir(path);
    }
}

static bool user_require_login(void) {
    if (user_state.current_index >= 0 && user_state.current_index < user_state.count) {
        return true;
    }
    user_print_line("login required");
    return false;
}

static bool user_require_root(void) {
    if (k64_user_is_root()) {
        return true;
    }
    user_print_line("permission denied: root privileges required");
    return false;
}

static void user_print_users(void) {
    char groups_buf[128];

    for (int i = 0; i < user_state.count; ++i) {
        user_collect_groups(i, groups_buf, sizeof(groups_buf));
        k64_term_write(user_state.accounts[i].name);
        k64_term_write("  ");
        k64_term_write(user_state.accounts[i].is_root ? "root" : "user");
        if (user_state.accounts[i].sudoer && !user_state.accounts[i].is_root) {
            k64_term_write(" sudo");
        }
        k64_term_write(" primary=");
        k64_term_write(user_state.accounts[i].primary_group);
        k64_term_write(" groups=");
        k64_term_write(groups_buf[0] ? groups_buf : "-");
        if (i == user_state.current_index) {
            k64_term_write(user_state.sudo_active ? " current(effective root)" : " current");
        }
        k64_term_putc('\n');
    }
}

static void user_print_groups(void) {
    for (int i = 0; i < user_state.group_count; ++i) {
        bool first = true;

        k64_term_write(user_state.groups[i].name);
        k64_term_write(": ");
        for (int j = 0; j < user_state.count; ++j) {
            if (!user_state.groups[i].members[j]) {
                continue;
            }
            if (!first) {
                k64_term_write(", ");
            }
            k64_term_write(user_state.accounts[j].name);
            first = false;
        }
        if (first) {
            k64_term_write("-");
        }
        k64_term_putc('\n');
    }
}

static bool user_command_users(const char* command, const char* args) {
    (void)command;
    (void)args;
    user_print_users();
    return true;
}

static bool user_command_groups(const char* command, const char* args) {
    char name[K64_USER_NAME_MAX];
    int idx;
    char groups_buf[128];

    (void)command;
    args = user_next_token(args, name, sizeof(name));
    if (!name[0]) {
        user_print_groups();
        return true;
    }
    idx = user_find(name);
    if (idx < 0) {
        user_print_line("user not found");
        return true;
    }
    user_collect_groups(idx, groups_buf, sizeof(groups_buf));
    k64_term_write(name);
    k64_term_write(": ");
    k64_term_write(groups_buf[0] ? groups_buf : "-");
    k64_term_putc('\n');
    return true;
}

static bool user_command_whoami(const char* command, const char* args) {
    (void)command;
    (void)args;
    user_print_line(k64_user_effective_name());
    return true;
}

static bool user_command_id(const char* command, const char* args) {
    char path[64];
    char groups_buf[128];
    (void)command;
    (void)args;

    if (!user_require_login()) {
        return true;
    }
    user_home_path(user_state.accounts[user_state.current_index].name, path, sizeof(path));
    user_collect_groups(user_state.current_index, groups_buf, sizeof(groups_buf));
    k64_term_write("real=");
    k64_term_write(user_state.accounts[user_state.current_index].name);
    k64_term_write(" effective=");
    k64_term_write(k64_user_effective_name());
    k64_term_write(" primary=");
    k64_term_write(user_state.accounts[user_state.current_index].primary_group);
    k64_term_write(" groups=");
    k64_term_write(groups_buf[0] ? groups_buf : "-");
    k64_term_write(" home=");
    k64_term_write(path);
    k64_term_putc('\n');
    return true;
}

static bool user_command_login_like(const char* args) {
    char name[K64_USER_NAME_MAX];
    char password[K64_USER_PASS_MAX];
    int idx;

    args = user_next_token(args, name, sizeof(name));
    args = user_next_token(args, password, sizeof(password));
    if (!name[0] || !password[0]) {
        user_print_line("usage: login <user> <password>");
        return true;
    }

    idx = user_find(name);
    if (idx < 0 || !user_password_matches(&user_state.accounts[idx], password)) {
        user_print_line("authentication failed");
        return true;
    }

    user_state.current_index = idx;
    user_state.sudo_active = false;
    k64_term_write("logged in as ");
    k64_term_write(user_state.accounts[idx].name);
    k64_term_putc('\n');
    return true;
}

static bool user_command_login(const char* command, const char* args) {
    (void)command;
    return user_command_login_like(args);
}

static bool user_command_su(const char* command, const char* args) {
    (void)command;
    return user_command_login_like(args);
}

static bool user_command_logout(const char* command, const char* args) {
    (void)command;
    (void)args;
    user_state.current_index = -1;
    user_state.sudo_active = false;
    user_print_line("logged out");
    return true;
}

static bool user_command_sudo(const char* command, const char* args) {
    char token[K64_USER_PASS_MAX];
    (void)command;

    if (!user_require_login()) {
        return true;
    }
    if (k64_user_is_root() && (!args || !args[0])) {
        user_print_line("already root");
        return true;
    }

    args = user_next_token(args, token, sizeof(token));
    if (k64_streq(token, "off")) {
        user_state.sudo_active = false;
        user_print_line("sudo disabled");
        return true;
    }
    if (!user_state.accounts[user_state.current_index].sudoer) {
        user_print_line("permission denied: account is not in sudo");
        return true;
    }
    if (!token[0] || k64_streq(token, "on")) {
        user_state.sudo_active = true;
        user_print_line("sudo enabled");
        return true;
    }
    if (!user_password_matches(&user_state.accounts[user_state.current_index], token)) {
        user_print_line("authentication failed");
        return true;
    }
    user_state.sudo_active = true;
    user_print_line("sudo enabled");
    return true;
}

static bool user_command_passwd(const char* command, const char* args) {
    char name[K64_USER_NAME_MAX];
    char password[K64_USER_PASS_MAX];
    int idx;
    (void)command;

    args = user_next_token(args, name, sizeof(name));
    args = user_next_token(args, password, sizeof(password));
    if (!name[0] || !password[0]) {
        user_print_line("usage: passwd <user> <new-password>");
        return true;
    }
    idx = user_find(name);
    if (idx < 0) {
        user_print_line("user not found");
        return true;
    }
    if (!k64_user_is_root() &&
        (!user_require_login() || !k64_streq(user_state.accounts[user_state.current_index].name, name))) {
        user_print_line("permission denied");
        return true;
    }
    user_store_password(user_state.accounts[idx].password,
                        K64_USER_PASS_MAX,
                        user_state.accounts[idx].name,
                        password,
                        false);
    (void)user_save_db();
    user_print_line("password updated");
    return true;
}

static bool user_command_useradd(const char* command, const char* args) {
    char name[K64_USER_NAME_MAX];
    char password[K64_USER_PASS_MAX];
    char role[8];
    char primary_group[K64_GROUP_NAME_MAX];
    char path[64];
    bool is_root = false;
    bool sudoer = false;
    (void)command;

    if (!user_require_root()) {
        return true;
    }

    args = user_next_token(args, name, sizeof(name));
    args = user_next_token(args, password, sizeof(password));
    args = user_next_token(args, role, sizeof(role));
    args = user_next_token(args, primary_group, sizeof(primary_group));
    if (!name[0] || !password[0]) {
        user_print_line("usage: useradd <user> <password> [user|sudo|root] [primary-group]");
        return true;
    }
    if (k64_streq(role, "root")) {
        is_root = true;
        sudoer = true;
    } else if (k64_streq(role, "sudo")) {
        sudoer = true;
    }
    if (!primary_group[0]) {
        user_copy(primary_group, sizeof(primary_group), name);
    }
    if (!user_add_account(name, password, is_root, sudoer, primary_group, false)) {
        user_print_line("useradd failed");
        return true;
    }
    user_ensure_primary_groups();
    user_sync_role_groups();
    user_home_path(name, path, sizeof(path));
    user_ensure_dir(path);
    (void)user_save_state();
    user_print_line("user added");
    return true;
}

static bool user_command_userdel(const char* command, const char* args) {
    char name[K64_USER_NAME_MAX];
    int idx;
    (void)command;

    if (!user_require_root()) {
        return true;
    }

    args = user_next_token(args, name, sizeof(name));
    if (!name[0]) {
        user_print_line("usage: userdel <user>");
        return true;
    }
    idx = user_find(name);
    if (idx < 0) {
        user_print_line("user not found");
        return true;
    }
    if (user_state.accounts[idx].is_root) {
        user_print_line("cannot delete root account");
        return true;
    }

    for (int i = idx; i + 1 < user_state.count; ++i) {
        user_state.accounts[i] = user_state.accounts[i + 1];
    }
    user_state.count--;
    for (int g = 0; g < user_state.group_count; ++g) {
        for (int i = idx; i < user_state.count; ++i) {
            user_state.groups[g].members[i] = user_state.groups[g].members[i + 1];
        }
        user_state.groups[g].members[user_state.count] = false;
    }
    if (user_state.current_index == idx) {
        user_state.current_index = -1;
        user_state.sudo_active = false;
    } else if (user_state.current_index > idx) {
        user_state.current_index--;
    }
    (void)user_save_state();
    user_print_line("user deleted");
    return true;
}

static bool user_command_groupadd(const char* command, const char* args) {
    char group[K64_GROUP_NAME_MAX];
    (void)command;

    if (!user_require_root()) {
        return true;
    }
    args = user_next_token(args, group, sizeof(group));
    if (!group[0]) {
        user_print_line("usage: groupadd <group>");
        return true;
    }
    if (!group_add_internal(group)) {
        user_print_line("groupadd failed");
        return true;
    }
    (void)user_save_groups_db();
    user_print_line("group added");
    return true;
}

static bool user_command_groupdel(const char* command, const char* args) {
    char group[K64_GROUP_NAME_MAX];
    int idx;
    (void)command;

    if (!user_require_root()) {
        return true;
    }
    args = user_next_token(args, group, sizeof(group));
    if (!group[0]) {
        user_print_line("usage: groupdel <group>");
        return true;
    }
    if (k64_streq(group, "root") || k64_streq(group, "sudo")) {
        user_print_line("cannot delete essential group");
        return true;
    }
    idx = group_find(group);
    if (idx < 0) {
        user_print_line("group not found");
        return true;
    }
    for (int i = 0; i < user_state.count; ++i) {
        if (k64_streq(user_state.accounts[i].primary_group, group)) {
            user_print_line("group is still a primary group");
            return true;
        }
    }
    for (int i = idx; i + 1 < user_state.group_count; ++i) {
        user_state.groups[i] = user_state.groups[i + 1];
    }
    user_state.group_count--;
    (void)user_save_groups_db();
    user_print_line("group deleted");
    return true;
}

static bool user_command_gpasswd(const char* command, const char* args) {
    char action[8];
    char group[K64_GROUP_NAME_MAX];
    char user_name[K64_USER_NAME_MAX];
    bool ok = false;
    (void)command;

    if (!user_require_root()) {
        return true;
    }
    args = user_next_token(args, action, sizeof(action));
    args = user_next_token(args, group, sizeof(group));
    args = user_next_token(args, user_name, sizeof(user_name));
    if (!action[0] || !group[0] || !user_name[0]) {
        user_print_line("usage: gpasswd <add|del> <group> <user>");
        return true;
    }
    if (k64_streq(action, "add")) {
        ok = user_add_to_group(user_name, group);
    } else if (k64_streq(action, "del")) {
        ok = user_remove_from_group(user_name, group);
    }
    if (!ok) {
        user_print_line("gpasswd failed");
        return true;
    }
    (void)user_save_groups_db();
    user_print_line("group membership updated");
    return true;
}

static bool user_command_usermod(const char* command, const char* args) {
    char action[16];
    char name[K64_USER_NAME_MAX];
    char value[K64_GROUP_NAME_MAX];
    int idx;
    (void)command;

    if (!user_require_root()) {
        return true;
    }

    args = user_next_token(args, action, sizeof(action));
    args = user_next_token(args, name, sizeof(name));
    args = user_next_token(args, value, sizeof(value));
    if (!action[0] || !name[0] || !value[0]) {
        user_print_line("usage: usermod <role|primary|groupadd|groupdel> <user> <value>");
        return true;
    }
    idx = user_find(name);
    if (idx < 0) {
        user_print_line("user not found");
        return true;
    }

    if (k64_streq(action, "role")) {
        user_state.accounts[idx].is_root = k64_streq(value, "root");
        user_state.accounts[idx].sudoer = user_state.accounts[idx].is_root || k64_streq(value, "sudo");
        user_sync_role_groups();
        (void)user_save_state();
        user_print_line("user role updated");
        return true;
    }
    if (k64_streq(action, "primary")) {
        if (!group_add_internal(value)) {
            user_print_line("primary group update failed");
            return true;
        }
        user_copy(user_state.accounts[idx].primary_group, K64_GROUP_NAME_MAX, value);
        group_set_membership(group_find(value), idx, true);
        (void)user_save_state();
        user_print_line("primary group updated");
        return true;
    }
    if (k64_streq(action, "groupadd")) {
        if (!user_add_to_group(name, value)) {
            user_print_line("group membership update failed");
            return true;
        }
        (void)user_save_groups_db();
        user_print_line("group membership updated");
        return true;
    }
    if (k64_streq(action, "groupdel")) {
        if (!user_remove_from_group(name, value)) {
            user_print_line("group membership update failed");
            return true;
        }
        (void)user_save_groups_db();
        user_print_line("group membership updated");
        return true;
    }

    user_print_line("usage: usermod <role|primary|groupadd|groupdel> <user> <value>");
    return true;
}

static bool user_command_userctl(const char* command, const char* args) {
    char subcmd[16];
    (void)command;

    args = user_next_token(args, subcmd, sizeof(subcmd));
    if (!subcmd[0]) {
        user_print_line("usage: userctl <users|groups|whoami|id|login|logout|su|sudo|passwd|useradd|userdel|usermod|groupadd|groupdel|gpasswd>");
        return true;
    }
    if (k64_streq(subcmd, "users")) {
        return user_command_users(subcmd, args);
    }
    if (k64_streq(subcmd, "groups")) {
        return user_command_groups(subcmd, args);
    }
    if (k64_streq(subcmd, "whoami")) {
        return user_command_whoami(subcmd, args);
    }
    if (k64_streq(subcmd, "id")) {
        return user_command_id(subcmd, args);
    }
    if (k64_streq(subcmd, "login")) {
        return user_command_login(subcmd, args);
    }
    if (k64_streq(subcmd, "logout")) {
        return user_command_logout(subcmd, args);
    }
    if (k64_streq(subcmd, "su")) {
        return user_command_su(subcmd, args);
    }
    if (k64_streq(subcmd, "sudo")) {
        return user_command_sudo(subcmd, args);
    }
    if (k64_streq(subcmd, "passwd")) {
        return user_command_passwd(subcmd, args);
    }
    if (k64_streq(subcmd, "useradd")) {
        return user_command_useradd(subcmd, args);
    }
    if (k64_streq(subcmd, "userdel")) {
        return user_command_userdel(subcmd, args);
    }
    if (k64_streq(subcmd, "usermod")) {
        return user_command_usermod(subcmd, args);
    }
    if (k64_streq(subcmd, "groupadd")) {
        return user_command_groupadd(subcmd, args);
    }
    if (k64_streq(subcmd, "groupdel")) {
        return user_command_groupdel(subcmd, args);
    }
    if (k64_streq(subcmd, "gpasswd")) {
        return user_command_gpasswd(subcmd, args);
    }
    user_print_line("usage: userctl <users|groups|whoami|id|login|logout|su|sudo|passwd|useradd|userdel|usermod|groupadd|groupdel|gpasswd>");
    return true;
}

bool k64_user_service_start(k64_service_t* service) {
    (void)service;

    if (!k64_fs_driver_running()) {
        user_seed_defaults();
    } else {
        user_ensure_dir("/etc");
        user_ensure_dir("/usr");
        if (!user_load_db()) {
            user_seed_defaults();
            (void)user_save_state();
        } else {
            (void)user_load_groups_db();
        }
        user_ensure_layout();
    }

    if (user_state.current_index < 0) {
        user_state.current_index = user_find("guest");
        if (user_state.current_index < 0 && user_state.count > 0) {
            user_state.current_index = 0;
        }
    }

    user_state.ready = true;
    (void)k64_system_register_command("userctl", "userctl", user_command_userctl);
    (void)k64_system_register_command("userctl", "users", user_command_users);
    (void)k64_system_register_command("userctl", "groups", user_command_groups);
    (void)k64_system_register_command("userctl", "whoami", user_command_whoami);
    (void)k64_system_register_command("userctl", "id", user_command_id);
    (void)k64_system_register_command("userctl", "login", user_command_login);
    (void)k64_system_register_command("userctl", "logout", user_command_logout);
    (void)k64_system_register_command("userctl", "su", user_command_su);
    (void)k64_system_register_command("userctl", "sudo", user_command_sudo);
    (void)k64_system_register_command("userctl", "passwd", user_command_passwd);
    (void)k64_system_register_command("userctl", "useradd", user_command_useradd);
    (void)k64_system_register_command("userctl", "userdel", user_command_userdel);
    (void)k64_system_register_command("userctl", "usermod", user_command_usermod);
    (void)k64_system_register_command("userctl", "groupadd", user_command_groupadd);
    (void)k64_system_register_command("userctl", "groupdel", user_command_groupdel);
    (void)k64_system_register_command("userctl", "gpasswd", user_command_gpasswd);
    user_print_line("[svc] userctl started");
    return true;
}

void k64_user_service_stop(k64_service_t* service) {
    (void)service;
    user_state.ready = false;
    user_print_line("[svc] userctl stopped");
}

bool k64_user_is_root(void) {
    if (!user_state.ready || user_state.current_index < 0 || user_state.current_index >= user_state.count) {
        return false;
    }
    return user_state.accounts[user_state.current_index].is_root || user_state.sudo_active;
}

bool k64_user_can_manage_service(const k64_service_t* service) {
    if (!service) {
        return false;
    }
    if (k64_user_is_root()) {
        return true;
    }
    return service->class_id == K64_SERVICE_CLASS_USER;
}

bool k64_user_can_manage_drivers(void) {
    return k64_user_is_root();
}

const char* k64_user_effective_name(void) {
    if (!user_state.ready || user_state.current_index < 0 || user_state.current_index >= user_state.count) {
        return "guest";
    }
    if (k64_user_is_root()) {
        return "root";
    }
    return user_state.accounts[user_state.current_index].name;
}
