#include <k64/libc.h>

#define EDIT_MAX_LINES 64
#define EDIT_MAX_COLS 96

static char lines[EDIT_MAX_LINES][EDIT_MAX_COLS];
static int line_count = 1;
static int row = 0;
static int col = 0;
static int dirty = 0;
static const char* edit_path = "/tmp/edit.txt";
static char status[80];

static void set_status(const char* text) {
    k64_strncpy(status, text ? text : "", sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
}

static void redraw(void) {
    k64_clear_screen();
    k64_puts("K64 edit - ");
    k64_puts(edit_path);
    k64_puts(dirty ? " *\n" : "\n");
    k64_puts("Esc+s/@s save  Esc+q/@q quit  Enter newline  Backspace delete\n");
    k64_puts("------------------------------------------------------------\n");

    for (int i = 0; i < EDIT_MAX_LINES && i < line_count; ++i) {
        k64_puts(i == row ? "> " : "  ");
        for (int j = 0; lines[i][j]; ++j) {
            if (i == row && j == col) {
                k64_putc('|');
            }
            k64_putc(lines[i][j]);
        }
        if (i == row && col == (int)k64_strlen(lines[i])) {
            k64_putc('|');
        }
        k64_putc('\n');
    }

    k64_puts("------------------------------------------------------------\n");
    k64_puts(status);
    k64_putc('\n');
}

static void load_file(void) {
    char buf[EDIT_MAX_LINES * EDIT_MAX_COLS];
    int64_t fd = k64_open(edit_path);
    int64_t n;
    int r = 0;
    int c = 0;

    line_count = 1;
    lines[0][0] = '\0';
    if (fd < 0) {
        set_status("New file");
        return;
    }
    n = k64_read((int)fd, buf, sizeof(buf) - 1);
    (void)k64_close((int)fd);
    if (n <= 0) {
        set_status("Empty file");
        return;
    }
    buf[n] = '\0';

    for (int i = 0; i < n && r < EDIT_MAX_LINES; ++i) {
        char ch = buf[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            lines[r][c] = '\0';
            r++;
            c = 0;
            if (r < EDIT_MAX_LINES) {
                lines[r][0] = '\0';
            }
            continue;
        }
        if (c + 1 < EDIT_MAX_COLS) {
            lines[r][c++] = ch;
            lines[r][c] = '\0';
        }
    }
    line_count = r + 1;
    if (line_count > EDIT_MAX_LINES) {
        line_count = EDIT_MAX_LINES;
    }
    set_status("Loaded");
}

static int save_file(void) {
    char out[EDIT_MAX_LINES * EDIT_MAX_COLS];
    size_t pos = 0;

    for (int i = 0; i < line_count; ++i) {
        for (int j = 0; lines[i][j] && pos + 1 < sizeof(out); ++j) {
            out[pos++] = lines[i][j];
        }
        if (pos + 1 < sizeof(out)) {
            out[pos++] = '\n';
        }
    }
    if (k64_write_file(edit_path, out, pos) != 0) {
        set_status("Save failed");
        return 0;
    }
    dirty = 0;
    set_status("Saved");
    return 1;
}

static void insert_char(char ch) {
    int len = (int)k64_strlen(lines[row]);

    if (len + 1 >= EDIT_MAX_COLS) {
        set_status("Line is full");
        return;
    }
    for (int i = len; i >= col; --i) {
        lines[row][i + 1] = lines[row][i];
    }
    lines[row][col++] = ch;
    dirty = 1;
    set_status("");
}

static void newline(void) {
    int len = (int)k64_strlen(lines[row]);

    if (line_count >= EDIT_MAX_LINES) {
        set_status("File line limit reached");
        return;
    }
    for (int i = line_count; i > row + 1; --i) {
        k64_strcpy(lines[i], lines[i - 1]);
    }
    k64_strcpy(lines[row + 1], lines[row] + col);
    lines[row][col] = '\0';
    row++;
    line_count++;
    col = 0;
    (void)len;
    dirty = 1;
}

static void backspace(void) {
    int len = (int)k64_strlen(lines[row]);

    if (col > 0) {
        for (int i = col - 1; i < len; ++i) {
            lines[row][i] = lines[row][i + 1];
        }
        col--;
        dirty = 1;
        return;
    }
    if (row > 0) {
        int prev_len = (int)k64_strlen(lines[row - 1]);
        if (prev_len + len >= EDIT_MAX_COLS) {
            set_status("Cannot join: line too long");
            return;
        }
        k64_strcat(lines[row - 1], lines[row]);
        for (int i = row; i + 1 < line_count; ++i) {
            k64_strcpy(lines[i], lines[i + 1]);
        }
        row--;
        col = prev_len;
        line_count--;
        dirty = 1;
    }
}

static int read_key(void) {
    char ch = 0;
    if (k64_read_stdin(&ch, 1) != 1) {
        return -1;
    }
    return (unsigned char)ch;
}

int main(int argc, char** argv) {
    int escape = 0;
    int command = 0;
    int confirm_quit = 0;

    if (argc > 1 && argv && argv[1]) {
        edit_path = argv[1];
    }
    load_file();

    for (;;) {
        int key;
        redraw();
        key = read_key();
        if (key < 0) {
            continue;
        }

        if (escape || command) {
            int was_command = command;
            escape = 0;
            command = 0;
            if (key == 's' || key == 'S') {
                (void)save_file();
                confirm_quit = 0;
                continue;
            }
            if (key == 'q' || key == 'Q') {
                if (dirty && !confirm_quit) {
                    set_status("Unsaved changes. Esc+q again quits without saving.");
                    confirm_quit = 1;
                    continue;
                }
                break;
            }
            set_status(was_command ? "Unknown @ command" : "Unknown Esc command");
            continue;
        }

        if (key == 27) {
            escape = 1;
            set_status("Esc command: s save, q quit");
        } else if (key == '@') {
            command = 1;
            set_status("@ command: s save, q quit");
        } else if (key == 17) {
            if (confirm_quit) {
                break;
            }
            if (dirty) {
                set_status("Unsaved changes. Press Ctrl+Q again to quit.");
                confirm_quit = 1;
            } else {
                break;
            }
        } else if (key == 19) {
            (void)save_file();
            confirm_quit = 0;
        } else if (key == '\n') {
            newline();
            confirm_quit = 0;
        } else if (key == '\b' || key == 127) {
            backspace();
            confirm_quit = 0;
        } else if (key >= 32 && key < 127) {
            insert_char((char)key);
            confirm_quit = 0;
        }
    }

    k64_clear_screen();
    k64_puts("edit: closed ");
    k64_puts(edit_path);
    k64_puts("\n");
    return 0;
}
