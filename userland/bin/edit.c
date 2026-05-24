#include <k64/libc.h>

#define K64_KEY_CHAR      1
#define K64_KEY_ENTER     2
#define K64_KEY_BACKSPACE 3
#define K64_KEY_DELETE    4
#define K64_KEY_LEFT      5
#define K64_KEY_RIGHT     6
#define K64_KEY_UP        7
#define K64_KEY_DOWN      8
#define K64_KEY_TAB       9
#define K64_KEY_ESCAPE    10

#define EDIT_MAX_LINES 256
#define EDIT_MAX_COLS  192
#define EDIT_STATUS_MAX 96

static char lines[EDIT_MAX_LINES][EDIT_MAX_COLS];
static int line_count = 1;
static int cx = 0;
static int cy = 0;
static int rowoff = 0;
static int coloff = 0;
static int dirty = 0;
static int quit_confirm = 0;
static const char* edit_path = "/tmp/edit.txt";
static char status[EDIT_STATUS_MAX];
static char load_buf[EDIT_MAX_LINES * EDIT_MAX_COLS];

static void set_status(const char* text) {
    k64_strncpy(status, text ? text : "", sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
}

static int line_len(int row) {
    if (row < 0 || row >= line_count) {
        return 0;
    }
    return (int)k64_strlen(lines[row]);
}

static void clamp_cursor(void) {
    int len;
    if (cy < 0) {
        cy = 0;
    }
    if (cy >= line_count) {
        cy = line_count - 1;
    }
    len = line_len(cy);
    if (cx < 0) {
        cx = 0;
    }
    if (cx > len) {
        cx = len;
    }
}

static void scroll_to_cursor(void) {
    int rows = k64_term_rows();
    int cols = k64_term_cols();
    int text_rows = rows - 3;
    int text_cols = cols - 1;

    if (text_rows < 1) {
        text_rows = 1;
    }
    if (text_cols < 20) {
        text_cols = 20;
    }
    if (cy < rowoff) {
        rowoff = cy;
    }
    if (cy >= rowoff + text_rows) {
        rowoff = cy - text_rows + 1;
    }
    if (cx < coloff) {
        coloff = cx;
    }
    if (cx >= coloff + text_cols) {
        coloff = cx - text_cols + 1;
    }
}

static void write_padded(const char* text, int width) {
    int used = 0;
    while (text && *text && used < width) {
        k64_putc(*text++);
        used++;
    }
    while (used++ < width) {
        k64_putc(' ');
    }
}

static void draw_bar(void) {
    char mark = dirty ? '*' : ' ';
    k64_puts(" K64 edit ");
    k64_putc(mark);
    k64_putc(' ');
    k64_puts(edit_path);
    k64_puts("  Ln ");
    k64_put_i64(cy + 1);
    k64_puts(", Col ");
    k64_put_i64(cx + 1);
}

static void redraw(void) {
    int rows = k64_term_rows();
    int cols = k64_term_cols();
    int text_rows = rows - 3;
    int screen_y;
    int screen_x;

    if (text_rows < 1) {
        text_rows = 1;
    }
    clamp_cursor();
    scroll_to_cursor();

    k64_clear_screen();
    draw_bar();
    k64_putc('\n');
    for (int y = 0; y < text_rows; ++y) {
        int file_row = rowoff + y;
        int visible = 0;
        if (file_row < line_count) {
            int len = line_len(file_row);
            for (int x = coloff; x < len && visible < cols; ++x) {
                char ch = lines[file_row][x];
                k64_putc(ch == '\t' ? ' ' : ch);
                visible++;
            }
        } else if (line_count == 1 && file_row == 1) {
            k64_putc('~');
            visible = 1;
        } else {
            k64_putc('~');
            visible = 1;
        }
        while (visible++ < cols) {
            k64_putc(' ');
        }
        if (y + 1 < text_rows) {
            k64_putc('\n');
        }
    }
    k64_putc('\n');
    write_padded("^S Save  ^Q Quit  arrows Move  Del Delete  Enter Newline", cols);
    k64_putc('\n');
    write_padded(status, cols);

    screen_y = 1 + (cy - rowoff);
    screen_x = cx - coloff;
    if (screen_y < 1) {
        screen_y = 1;
    }
    if (screen_y > rows - 3) {
        screen_y = rows - 3;
    }
    if (screen_x < 0) {
        screen_x = 0;
    }
    if (screen_x >= cols) {
        screen_x = cols - 1;
    }
    k64_set_cursor(screen_x, screen_y);
}

static void load_file(void) {
    int64_t fd = k64_open(edit_path);
    int64_t n;
    int r = 0;
    int c = 0;

    line_count = 1;
    lines[0][0] = '\0';
    if (fd < 0) {
        set_status("New file. Ctrl-S saves, Ctrl-Q quits.");
        return;
    }
    n = k64_read((int)fd, load_buf, sizeof(load_buf) - 1);
    (void)k64_close((int)fd);
    if (n <= 0) {
        set_status("Empty file");
        return;
    }
    load_buf[n] = '\0';
    for (int i = 0; i < n && r < EDIT_MAX_LINES; ++i) {
        char ch = load_buf[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            lines[r][c] = '\0';
            if (r + 1 >= EDIT_MAX_LINES) {
                break;
            }
            r++;
            c = 0;
            lines[r][0] = '\0';
            continue;
        }
        if (c + 1 < EDIT_MAX_COLS) {
            lines[r][c++] = ch;
            lines[r][c] = '\0';
        }
    }
    line_count = r + 1;
    if (line_count < 1) {
        line_count = 1;
    }
    set_status("Loaded");
}

static int save_file(void) {
    static char out[EDIT_MAX_LINES * EDIT_MAX_COLS];
    size_t pos = 0;

    for (int y = 0; y < line_count; ++y) {
        for (int x = 0; lines[y][x] && pos + 1 < sizeof(out); ++x) {
            out[pos++] = lines[y][x];
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
    quit_confirm = 0;
    set_status("Saved");
    return 1;
}

static void insert_char(char ch) {
    int len = line_len(cy);
    if (len + 1 >= EDIT_MAX_COLS) {
        set_status("Line too long");
        return;
    }
    for (int i = len; i >= cx; --i) {
        lines[cy][i + 1] = lines[cy][i];
    }
    lines[cy][cx++] = ch;
    dirty = 1;
    quit_confirm = 0;
    set_status("");
}

static void insert_newline(void) {
    int len = line_len(cy);
    if (line_count >= EDIT_MAX_LINES) {
        set_status("File line limit reached");
        return;
    }
    for (int y = line_count; y > cy + 1; --y) {
        k64_strcpy(lines[y], lines[y - 1]);
    }
    k64_strcpy(lines[cy + 1], lines[cy] + cx);
    lines[cy][cx] = '\0';
    (void)len;
    cy++;
    cx = 0;
    line_count++;
    dirty = 1;
    quit_confirm = 0;
    set_status("");
}

static void delete_char(void) {
    int len = line_len(cy);
    if (cx < len) {
        for (int i = cx; i < len; ++i) {
            lines[cy][i] = lines[cy][i + 1];
        }
        dirty = 1;
        quit_confirm = 0;
        return;
    }
    if (cy + 1 < line_count) {
        int next_len = line_len(cy + 1);
        if (len + next_len >= EDIT_MAX_COLS) {
            set_status("Cannot join: line too long");
            return;
        }
        k64_strcat(lines[cy], lines[cy + 1]);
        for (int y = cy + 1; y + 1 < line_count; ++y) {
            k64_strcpy(lines[y], lines[y + 1]);
        }
        line_count--;
        dirty = 1;
        quit_confirm = 0;
    }
}

static void backspace(void) {
    if (cx > 0) {
        cx--;
        delete_char();
        return;
    }
    if (cy > 0) {
        int prev_len = line_len(cy - 1);
        int len = line_len(cy);
        if (prev_len + len >= EDIT_MAX_COLS) {
            set_status("Cannot join: line too long");
            return;
        }
        k64_strcat(lines[cy - 1], lines[cy]);
        for (int y = cy; y + 1 < line_count; ++y) {
            k64_strcpy(lines[y], lines[y + 1]);
        }
        cy--;
        cx = prev_len;
        line_count--;
        dirty = 1;
        quit_confirm = 0;
    }
}

static void move_cursor(int key) {
    if (key == K64_KEY_LEFT) {
        if (cx > 0) {
            cx--;
        } else if (cy > 0) {
            cy--;
            cx = line_len(cy);
        }
    } else if (key == K64_KEY_RIGHT) {
        if (cx < line_len(cy)) {
            cx++;
        } else if (cy + 1 < line_count) {
            cy++;
            cx = 0;
        }
    } else if (key == K64_KEY_UP) {
        cy--;
    } else if (key == K64_KEY_DOWN) {
        cy++;
    }
    clamp_cursor();
    quit_confirm = 0;
}

static int read_key_type(int* ch) {
    int64_t packed = k64_read_key();
    if (ch) {
        *ch = (int)((packed >> 8) & 0xFF);
    }
    return (int)(packed & 0xFF);
}

int main(int argc, char** argv) {
    if (argc > 1 && argv && argv[1]) {
        edit_path = argv[1];
    }
    load_file();

    for (;;) {
        int ch = 0;
        int key;
        int command = 0;
        redraw();
        key = read_key_type(&ch);

        if (key == K64_KEY_CHAR && (ch == 17 || ch == 'q' - 96)) {
            if (dirty && !quit_confirm) {
                quit_confirm = 1;
                set_status("Unsaved changes. Press Ctrl-Q again to quit without saving.");
                continue;
            }
            break;
        }
        if (key == K64_KEY_CHAR && (ch == 19 || ch == 's' - 96)) {
            (void)save_file();
            continue;
        }
        if (key == K64_KEY_ESCAPE) {
            command = 1;
            set_status("Esc command: s save, q quit");
        }
        if ((key == K64_KEY_CHAR && ch == '@') || command) {
            redraw();
            key = read_key_type(&ch);
            if (key == K64_KEY_CHAR && (ch == 's' || ch == 'S')) {
                (void)save_file();
            } else if (key == K64_KEY_CHAR && (ch == 'q' || ch == 'Q')) {
                if (dirty && !quit_confirm) {
                    quit_confirm = 1;
                    set_status("Unsaved changes. @q again quits without saving.");
                } else {
                    break;
                }
            } else {
                set_status("@ command: use @s save or @q quit");
            }
            continue;
        }

        switch (key) {
            case K64_KEY_LEFT:
            case K64_KEY_RIGHT:
            case K64_KEY_UP:
            case K64_KEY_DOWN:
                move_cursor(key);
                break;
            case K64_KEY_DELETE:
                delete_char();
                break;
            case K64_KEY_BACKSPACE:
                backspace();
                break;
            case K64_KEY_ENTER:
                insert_newline();
                break;
            case K64_KEY_TAB:
                insert_char(' ');
                insert_char(' ');
                break;
            case K64_KEY_CHAR:
                if (ch >= 32 && ch < 127) {
                    insert_char((char)ch);
                }
                break;
            default:
                break;
        }
    }

    k64_clear_screen();
    k64_puts("edit: closed ");
    k64_puts(edit_path);
    k64_puts("\n");
    return 0;
}
