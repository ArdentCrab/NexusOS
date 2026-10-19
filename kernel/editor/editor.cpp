// kernel/editor/editor.cpp — Refactor Session 11a
#include "kernel/editor/editor.h"
#include "kernel/fs/fs.h"
#include "kernel/dev/kbd.h"

namespace editor {

static constexpr int EMX_ROWS = 256;
static constexpr int EMX_COLS = 96;
static constexpr int VIS      = 20;   // sichtbare Textzeilen

static char g_lines[EMX_ROWS][EMX_COLS];
static int  g_lens[EMX_ROWS];
static int  g_nrows = 1;
static int  g_cx = 0, g_cy = 0, g_off = 0;
static char g_fname[32];
static bool g_dirty = false;

static void wr_u(u64 v) {
    char b[24]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n--) hal::pc(b[n]);
}

static void cur(int row, int col) {
    hal::ps("\x1b["); wr_u((u64)row); hal::pc(';');
    wr_u((u64)col);   hal::pc('H');
}

static void draw_line(int srow, int ty) {
    cur(srow, 1);
    if (ty < g_nrows) {
        int n = g_lens[ty]; if (n > EMX_COLS - 1) n = EMX_COLS - 1;
        for (int i = 0; i < n; i++) hal::pc(g_lines[ty][i]);
    }
    hal::ps("\x1b[K");
}

static void draw_header() {
    cur(1, 1);
    hal::ps("EDIT: "); hal::ps(g_fname);
    if (g_dirty) hal::ps("  [MODIFIED]");
    hal::ps("\x1b[K");
}

static void draw_footer() {
    cur(VIS + 4, 1);
    hal::ps("^S Save   ^X Save+Quit   ^Q Discard   Arrows Move");
    hal::ps("\x1b[K");
    cur(VIS + 5, 1);
    hal::ps("Row "); wr_u((u64)(g_cy + 1));
    hal::ps("  Col "); wr_u((u64)(g_cx + 1));
    hal::ps("  /  "); wr_u((u64)g_nrows); hal::ps(" lines");
    hal::ps("\x1b[K");
}

static void draw_all() {
    hal::ps("\x1b[?25l");    // Cursor aus
    hal::ps("\x1b[2J\x1b[H"); // Clear + Home
    draw_header();
    cur(2, 1);
    for (int i = 0; i < 78; i++) hal::pc('-');
    hal::ps("\x1b[K");
    for (int i = 0; i < VIS; i++) draw_line(3 + i, g_off + i);
    cur(VIS + 3, 1);
    for (int i = 0; i < 78; i++) hal::pc('-');
    hal::ps("\x1b[K");
    draw_footer();
    hal::ps("\x1b[?25h");
    cur((g_cy - g_off) + 3, g_cx + 1);
}

static void update_cursor() {
    cur((g_cy - g_off) + 3, g_cx + 1);
}

static void mark_dirty() { g_dirty = true; draw_header(); }

// ---- Operationen ----
static void ins_char(char c) {
    if (g_lens[g_cy] >= EMX_COLS - 1) return;
    int n = g_lens[g_cy];
    for (int i = n; i > g_cx; i--) g_lines[g_cy][i] = g_lines[g_cy][i-1];
    g_lines[g_cy][g_cx] = c;
    g_lens[g_cy]++;
    g_cx++;
    mark_dirty();
    draw_line((g_cy - g_off) + 3, g_cy);
    draw_footer();
    update_cursor();
}

static void ins_nl() {
    if (g_nrows >= EMX_ROWS - 1) return;
    for (int i = g_nrows; i > g_cy + 1; i--) {
        for (int j = 0; j < g_lens[i-1]; j++) g_lines[i][j] = g_lines[i-1][j];
        g_lens[i] = g_lens[i-1];
    }
    int rest = g_lens[g_cy] - g_cx;
    for (int i = 0; i < rest; i++) g_lines[g_cy + 1][i] = g_lines[g_cy][g_cx + i];
    g_lens[g_cy + 1] = rest;
    g_lens[g_cy]     = g_cx;
    g_nrows++;
    g_cy++;
    g_cx = 0;
    mark_dirty();
    if (g_cy - g_off >= VIS) g_off = g_cy - VIS + 1;
    for (int i = 0; i < VIS; i++) draw_line(3 + i, g_off + i);
    draw_footer();
    update_cursor();
}

static void backspace() {
    if (g_cx > 0) {
        for (int i = g_cx - 1; i < g_lens[g_cy] - 1; i++)
            g_lines[g_cy][i] = g_lines[g_cy][i+1];
        g_lens[g_cy]--;
        g_cx--;
        mark_dirty();
        draw_line((g_cy - g_off) + 3, g_cy);
        draw_footer();
        update_cursor();
    } else if (g_cy > 0) {
        int pl = g_lens[g_cy - 1];
        int cl = g_lens[g_cy];
        if (pl + cl < EMX_COLS - 1) {
            for (int i = 0; i < cl; i++) g_lines[g_cy - 1][pl + i] = g_lines[g_cy][i];
            g_lens[g_cy - 1] = pl + cl;
            for (int i = g_cy; i < g_nrows - 1; i++) {
                for (int j = 0; j < g_lens[i+1]; j++) g_lines[i][j] = g_lines[i+1][j];
                g_lens[i] = g_lens[i+1];
            }
            g_nrows--;
            g_cy--;
            g_cx = pl;
            mark_dirty();
            if (g_cy < g_off) g_off = g_cy;
            for (int i = 0; i < VIS; i++) draw_line(3 + i, g_off + i);
            draw_footer();
            update_cursor();
        }
    }
}

static void move_up() {
    if (g_cy > 0) {
        g_cy--;
        if (g_cx > g_lens[g_cy]) g_cx = g_lens[g_cy];
        if (g_cy < g_off) { g_off = g_cy; draw_all(); return; }
        draw_footer(); update_cursor();
    }
}
static void move_down() {
    if (g_cy < g_nrows - 1) {
        g_cy++;
        if (g_cx > g_lens[g_cy]) g_cx = g_lens[g_cy];
        if (g_cy - g_off >= VIS) { g_off = g_cy - VIS + 1; draw_all(); return; }
        draw_footer(); update_cursor();
    }
}
static void move_left() {
    if (g_cx > 0) g_cx--;
    else if (g_cy > 0) { g_cy--; g_cx = g_lens[g_cy]; }
    else return;
    if (g_cy < g_off) { g_off = g_cy; draw_all(); return; }
    draw_footer(); update_cursor();
}
static void move_right() {
    if (g_cx < g_lens[g_cy]) g_cx++;
    else if (g_cy < g_nrows - 1) { g_cy++; g_cx = 0; }
    else return;
    if (g_cy - g_off >= VIS) { g_off = g_cy - VIS + 1; draw_all(); return; }
    draw_footer(); update_cursor();
}

// ---- File-I/O ----
static int load_file() {
    static char buf[8192];
    int n = fs::cat_file(g_fname, buf, sizeof(buf));
    g_nrows = 1; g_lens[0] = 0;
    if (n <= 0) return 0;
    for (int i = 0; i < n && g_nrows < EMX_ROWS; i++) {
        char c = buf[i];
        if (c == '\n') {
            g_nrows++;
            g_lens[g_nrows - 1] = 0;
        } else if (g_lens[g_nrows - 1] < EMX_COLS - 1) {
            g_lines[g_nrows - 1][g_lens[g_nrows - 1]++] = c;
        }
    }
    return n;
}

static int save_file() {
    static char buf[8192];
    int pos = 0;
    for (int i = 0; i < g_nrows && pos < (int)sizeof(buf) - 1; i++) {
        for (int j = 0; j < g_lens[i] && pos < (int)sizeof(buf) - 1; j++)
            buf[pos++] = g_lines[i][j];
        if (i < g_nrows - 1 && pos < (int)sizeof(buf) - 1) buf[pos++] = '\n';
    }
    buf[pos] = 0;
    int r = fs::write_file(g_fname, buf, pos);
    if (r >= 0) { g_dirty = false; draw_header(); }
    return r;
}

// ---- Main ----
int run(const char* name) {
    int i = 0;
    for (; name[i] && i < 30; i++) g_fname[i] = name[i];
    g_fname[i] = 0;
    g_cx = g_cy = g_off = 0;
    g_dirty = false;
    load_file();
    draw_all();

    for (;;) {
        int ch = kbd::getchar_nb();
        if (ch < 0) { for (int i = 0; i < 2000; i++) __asm__ volatile("" ::: "memory"); continue; }
        char c = (char)ch;

        if (c == 0x13) { save_file(); continue; }   // Ctrl+S
        if (c == 0x18) { save_file(); break; }      // Ctrl+X (Save+Quit)
        if (c == 0x11) { break; }                    // Ctrl+Q (Discard)
        if (c == 0x04) { save_file(); break; }      // Ctrl+D Fallback

        if (c == 0x1B) {                             // ESC-Sequenz
            int c2 = kbd::getchar_nb_wait();
            if (c2 == '[') {
                int c3 = kbd::getchar_nb_wait();
                if      (c3 == 'A') move_up();
                else if (c3 == 'B') move_down();
                else if (c3 == 'C') move_right();
                else if (c3 == 'D') move_left();
                else if (c3 == 'H') { g_cx = 0; update_cursor(); }
                else if (c3 == 'F') { g_cx = g_lens[g_cy]; update_cursor(); }
            }
            continue;
        }
        if (c == '\b' || c == 127) { backspace(); continue; }
        if (c == '\n' || c == '\r') { ins_nl(); continue; }
        if (c >= 32 && c < 127) { ins_char(c); continue; }
    }

    hal::ps("\x1b[2J\x1b[H");
    return 0;
}

} // namespace editor
