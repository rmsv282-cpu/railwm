// raillauncher -- a minimal X11 application launcher for railwm.
//
// Builds its candidate list from XDG desktop entries (~/.local/share/
// applications, $XDG_DATA_DIRS/applications), not raw $PATH binaries
// -- that's what lets it show "Firefox" instead of "firefox-bin", skip
// entries an app doesn't want in a menu, and hand off the actual
// Exec= command line (arguments and all) instead of just a bare name.
//
// Pops up a small centered override-redirect window with a text
// field: typing filters the list (case-insensitive substring match
// against the entry's display name), Up/Down moves the selection,
// Enter runs the selected entry's command and exits, Escape exits
// without doing anything.
//
// No IPC, no config file, no dependency on railwm itself beyond the
// keybind that spawns it (Alt+d, see wm.c) -- this could just as
// easily be run standalone from any WM.

#define _DEFAULT_SOURCE

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_QUERY 256
#define MAX_VISIBLE 8
#define ROW_HEIGHT 26
#define INPUT_HEIGHT 36
#define WIN_WIDTH 520
#define WIN_HEIGHT (INPUT_HEIGHT + MAX_VISIBLE * ROW_HEIGHT)
#define BORDER_WIDTH 2.0

// Same palette as railwm's own defaults (see wm.c), so the
// launcher doesn't look like a foreign popup.
typedef struct {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} Color;

static void color_set_source(Color self, cairo_t *cr) {
    cairo_set_source_rgb(cr,
        (double)self.r / 255.0,
        (double)self.g / 255.0,
        (double)self.b / 255.0);
}

static const Color BG = { 0x0a, 0x14, 0x28 };
static const Color FG = { 0xe8, 0xe8, 0xe0 };
static const Color ACCENT = { 0xff, 0xbf, 0x00 };
static const Color DIM = { 0x4d, 0x38, 0x00 };

/// Turns a fixed/null-terminated C char buffer into a C string. In
/// Zig this helper existed to sidestep std.mem.sliceTo's generic
/// sentinel-type inference -- that inference didn't play nicely with
/// fields pulled out of cImported extern structs (confirmed the hard
/// way against a real Zig 0.16 compiler: it mis-derived the sentinel's
/// element type from dirent's fixed d_name array). An explicit cast to
/// a sentinel pointer sidesteps the inference entirely. In C a char
/// buffer already *is* a NUL-terminated string, so the conversion is a
/// plain pass-through; kept so this file still reads line-for-line
/// against main.zig.
static const char *cStr(const char *buf) {
    return buf;
}

// ---- small helpers standing in for the std.mem/std.fmt calls the
// Zig version got out of its standard library -----------------------

// Byte-exact equality of two (pointer, length) pairs -- std.mem.eql.
static bool mem_eql(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

// std.ascii.eqlIgnoreCase over n bytes, ASCII only.
static bool eql_ascii_ignore_case(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i += 1) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

static bool ends_with(const char *s, size_t s_len, const char *suffix, size_t suffix_len) {
    return s_len >= suffix_len && memcmp(s + s_len - suffix_len, suffix, suffix_len) == 0;
}

static size_t min_sz(size_t a, size_t b) {
    return a < b ? a : b;
}

// arena.dupe equivalent: owned, NUL-terminated copy that stays valid
// for the process lifetime.
static char *dup_bytes(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p == NULL) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

// Growable string list, standing in for std.ArrayList([]const u8)
// over an arena.
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrList;

static bool strlist_push(StrList *list, char *owned) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        char **grown = (char **)realloc(list->items, new_cap * sizeof *grown);
        if (grown == NULL) return false;
        list->items = grown;
        list->cap = new_cap;
    }
    list->items[list->len] = owned;
    list->len += 1;
    return true;
}

static void strlist_free(StrList *list) {
    for (size_t i = 0; i < list->len; i += 1) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

// ---- application entries (XDG .desktop files) -----------------------

typedef struct {
    const char *name;    // Desktop entry's Name=, what's shown/searched
    size_t name_len;     // (the two lengths are what the Zig slices carried)
    const char *exec;    // Its Exec=, field codes already stripped
    size_t exec_len;
} Entry;

// Growable Entry list, standing in for std.ArrayList(Entry) over an
// arena.
typedef struct {
    Entry *items;
    size_t len;
    size_t cap;
} EntryList;

static bool entrylist_push(EntryList *list, Entry entry) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        Entry *grown = (Entry *)realloc(list->items, new_cap * sizeof *grown);
        if (grown == NULL) return false;
        list->items = grown;
        list->cap = new_cap;
    }
    list->items[list->len] = entry;
    list->len += 1;
    return true;
}

static int entryLessThan(const void *pa, const void *pb) {
    const Entry *a = (const Entry *)pa;
    const Entry *b = (const Entry *)pb;
    return strcmp(a->name, b->name);
}

/// %f %F %u %U %d %D %n %N %i %c %k %v %m are all field codes the
/// spec defines (file/URL lists, icon, translated name, etc.) --
/// meaningless without a file or URL actually being opened, so they're
/// dropped outright rather than substituted. %% is the one exception,
/// meaning a literal percent sign.
static char *stripFieldCodes(const char *raw, size_t raw_len, size_t *out_len) {
    // Output is never longer than the input (field codes only shrink
    // it), so one buffer of raw_len + 1 covers every case -- no
    // ArrayList growth needed.
    char *out = (char *)malloc(raw_len + 1);
    if (out == NULL) return NULL;
    size_t olen = 0;
    size_t i = 0;
    for (; i < raw_len; i += 1) {
        if (raw[i] == '%' && i + 1 < raw_len) {
            char next = raw[i + 1];
            if (next == 'f' || next == 'F' || next == 'u' || next == 'U' ||
                next == 'd' || next == 'D' || next == 'n' || next == 'N' ||
                next == 'i' || next == 'c' || next == 'k' || next == 'v' ||
                next == 'm') {
                i += 1; // the loop's own increment makes two, as in Zig
                continue;
            }
            if (next == '%') {
                out[olen] = '%';
                olen += 1;
                i += 1; // the loop's own increment makes two, as in Zig
                continue;
            }
        }
        out[olen] = raw[i];
        olen += 1;
    }
    out[olen] = '\0';
    *out_len = olen;
    return out;
}

/// True if `name` (a bare binary name, no slashes) resolves against
/// $PATH -- used to honor TryExec=, which is how a .desktop file says
/// "don't show me unless this is actually installed".
static bool existsOnPath(const char *name, size_t name_len) {
    if (name_len == 0) return true;
    const char *path_env = getenv("PATH");
    if (path_env == NULL) return false;
    const char *p = path_env;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t dir_len = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (dir_len > 0) {
            char buf[1024];
            int n = snprintf(buf, sizeof buf, "%.*s/%.*s",
                (int)dir_len, p, (int)name_len, name);
            // Same truncation behavior as std.fmt.bufPrintZ: if
            // "dir/name" doesn't fit, skip this directory entry.
            if (n > 0 && (size_t)n < sizeof buf) {
                if (access(buf, X_OK) == 0) return true;
            }
        }
        if (colon == NULL) break;
        p = colon + 1;
    }
    return false;
}

/// Parses one .desktop file's [Desktop Entry] group. Returns false if
/// the file doesn't describe a launchable, visible application --
/// missing Name/Exec, NoDisplay=true, Hidden=true, Type= anything but
/// Application, or a TryExec= that isn't actually installed. On
/// success *out receives malloc'd name/exec buffers that stay valid
/// for the process lifetime.
static bool parseDesktopFile(const char *path, Entry *out) {
    FILE *fh = fopen(path, "r");
    if (fh == NULL) return false;

    bool in_main_group = false;
    bool no_display = false;
    bool hidden = false;
    bool wrong_type = false;

    char name_buf[256];
    size_t name_len = 0;
    char exec_buf[1024];
    size_t exec_len = 0;
    char try_exec_buf[256];
    size_t try_exec_len = 0;

    char linebuf[1024];
    for (;;) {
        if (fgets(linebuf, (int)sizeof linebuf, fh) == NULL) break;
        const char *line = cStr(linebuf);
        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
            line_len -= 1;

        if (line_len == 0 || line[0] == '#') continue;

        if (line[0] == '[') {
            in_main_group = mem_eql(line, line_len, "[Desktop Entry]", sizeof "[Desktop Entry]" - 1);
            continue;
        }
        if (!in_main_group) continue;

        size_t eq = 0;
        bool have_eq = false;
        for (size_t k = 0; k < line_len; k += 1) {
            if (line[k] == '=') {
                eq = k;
                have_eq = true;
                break;
            }
        }
        if (!have_eq) continue;
        const char *key = line;
        size_t key_len = eq;
        const char *val = line + eq + 1;
        size_t val_len = line_len - eq - 1;

        if (mem_eql(key, key_len, "Name", 4)) {
            name_len = min_sz(val_len, sizeof name_buf);
            memcpy(name_buf, val, name_len);
        } else if (mem_eql(key, key_len, "Exec", 4)) {
            exec_len = min_sz(val_len, sizeof exec_buf);
            memcpy(exec_buf, val, exec_len);
        } else if (mem_eql(key, key_len, "TryExec", 7)) {
            try_exec_len = min_sz(val_len, sizeof try_exec_buf);
            memcpy(try_exec_buf, val, try_exec_len);
        } else if (mem_eql(key, key_len, "NoDisplay", 9)) {
            no_display = mem_eql(val, val_len, "true", 4);
        } else if (mem_eql(key, key_len, "Hidden", 6)) {
            hidden = mem_eql(val, val_len, "true", 4);
        } else if (mem_eql(key, key_len, "Type", 4)) {
            wrong_type = !mem_eql(val, val_len, "Application", 11);
        }
    }

    bool visible = !(name_len == 0 || exec_len == 0 || no_display || hidden || wrong_type);
    if (visible && try_exec_len > 0 && !existsOnPath(try_exec_buf, try_exec_len)) visible = false;

    bool ok = false;
    if (visible) {
        char *name = dup_bytes(name_buf, name_len);
        size_t stripped_len = 0;
        char *exec = name != NULL ? stripFieldCodes(exec_buf, exec_len, &stripped_len) : NULL;
        if (name != NULL && exec != NULL) {
            out->name = name;
            out->name_len = name_len;
            out->exec = exec;
            out->exec_len = stripped_len;
            ok = true;
        } else {
            free(name);
            free(exec);
        }
    }

    fclose(fh);
    return ok;
}

static bool appendApplicationsDir(StrList *dirs, const char *data_dir, size_t data_dir_len) {
    // allocPrint("{s}/applications", .{data_dir})
    size_t suffix_len = sizeof "/applications" - 1;
    char *app = (char *)malloc(data_dir_len + suffix_len + 1);
    if (app == NULL) return false;
    memcpy(app, data_dir, data_dir_len);
    memcpy(app + data_dir_len, "/applications", suffix_len);
    app[data_dir_len + suffix_len] = '\0';
    if (!strlist_push(dirs, app)) {
        free(app);
        return false;
    }
    return true;
}

/// Every visible, launchable .desktop entry found across the XDG data
/// dirs, deduplicated by filename (first dir wins, matching spec
/// precedence: $XDG_DATA_HOME before $XDG_DATA_DIRS) and sorted by
/// display name. The returned array is malloc'ed and stays valid for
/// the process lifetime -- exactly one scan happens, at startup. The
/// temporary dir/seen bookkeeping is freed before returning.
static bool scanDesktopEntries(Entry **out_all, size_t *out_len) {
    StrList dirs = { NULL, 0, 0 };

    const char *xdh = getenv("XDG_DATA_HOME");
    if (xdh != NULL) {
        if (!appendApplicationsDir(&dirs, xdh, strlen(xdh))) return false;
    } else {
        const char *home = getenv("HOME");
        if (home != NULL) {
            size_t home_len = strlen(home);
            char *local_share = (char *)malloc(home_len + sizeof "/.local/share");
            if (local_share == NULL) return false;
            memcpy(local_share, home, home_len);
            memcpy(local_share + home_len, "/.local/share", sizeof "/.local/share");
            bool pushed = appendApplicationsDir(&dirs, local_share, strlen(local_share));
            free(local_share);
            if (!pushed) return false;
        }
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (data_dirs == NULL) data_dirs = "/usr/local/share:/usr/share";
    const char *dd = data_dirs;
    for (;;) {
        const char *colon = strchr(dd, ':');
        size_t d_len = colon != NULL ? (size_t)(colon - dd) : strlen(dd);
        if (d_len > 0 && !appendApplicationsDir(&dirs, dd, d_len)) return false;
        if (colon == NULL) break;
        dd = colon + 1;
    }

    EntryList entries = { NULL, 0, 0 };
    StrList seen = { NULL, 0, 0 };

    for (size_t di = 0; di < dirs.len; di += 1) {
        const char *appdir = dirs.items[di];
        size_t appdir_len = strlen(appdir);
        if (appdir_len >= 900) continue;
        char dirbuf[1024];
        memcpy(dirbuf, appdir, appdir_len);
        dirbuf[appdir_len] = 0;

        DIR *dh = opendir(dirbuf);
        if (dh == NULL) continue;

        for (;;) {
            struct dirent *ent = readdir(dh);
            if (ent == NULL) break;
            const char *fname = cStr(ent->d_name);
            size_t fname_len = strlen(fname);
            if (!ends_with(fname, fname_len, ".desktop", sizeof ".desktop" - 1)) continue;

            bool already_seen = false;
            for (size_t s = 0; s < seen.len; s += 1) {
                if (strcmp(seen.items[s], fname) == 0) {
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) continue;
            char *owned_fname = dup_bytes(fname, fname_len);
            if (owned_fname == NULL) continue;
            if (!strlist_push(&seen, owned_fname)) {
                free(owned_fname);
                continue;
            }

            char pathbuf[1024];
            int pn = snprintf(pathbuf, sizeof pathbuf, "%s/%s", appdir, fname);
            // bufPrintZ equivalent: a path too long for the buffer
            // skips this file rather than overflowing.
            if (pn < 0 || (size_t)pn >= sizeof pathbuf) continue;
            Entry entry;
            if (!parseDesktopFile(pathbuf, &entry)) continue;
            if (!entrylist_push(&entries, entry)) continue;
        }
        closedir(dh);
    }

    strlist_free(&dirs);
    strlist_free(&seen);

    // entries.toOwnedSlice() equivalent: hand back the (possibly
    // oversized) allocation along with its exact length; sorting it
    // in place is the std.mem.sort below.
    if (entries.items != NULL)
        qsort(entries.items, entries.len, sizeof (Entry), entryLessThan);
    *out_all = entries.items;
    *out_len = entries.len;
    return true;
}

/// Case-insensitive substring match against the entry's display name,
/// ASCII only (matches the simple ASCII-only text entry below -- this
/// is a launcher, not an input method editor).
static bool matches(const Entry *entry, const char *query, size_t query_len) {
    if (query_len == 0) return true;
    if (query_len > entry->name_len) return false;
    for (size_t i = 0; i + query_len <= entry->name_len; i += 1) {
        if (eql_ascii_ignore_case(entry->name + i, query, query_len)) return true;
    }
    return false;
}

/// Filters `all` by `query`, filling `out` (capped at out_len
/// entries) with matches starting from the `offset`-th match rather
/// than always the first -- this is what makes the result list
/// scrollable instead of only ever showing the first MAX_VISIBLE
/// hits. Returns the total number of matches found across all of
/// `all`, regardless of offset or out_len, so the caller can tell how
/// far scrolling can go.
static size_t filterInto(const Entry *all, size_t all_len, const char *query, size_t query_len,
    size_t offset, Entry *out, size_t out_len)
{
    size_t total = 0;
    size_t filled = 0;
    for (size_t i = 0; i < all_len; i += 1) {
        if (!matches(&all[i], query, query_len)) continue;
        if (total >= offset && filled < out_len) {
            out[filled] = all[i];
            filled += 1;
        }
        total += 1;
    }
    return total;
}

/// Given the absolute (into the full filtered list) selected index
/// and the total match count, picks which match should be the first
/// one shown -- the smallest window of MAX_VISIBLE rows that still
/// contains `selected`, clamped so the window never scrolls past the
/// end of the list. Recomputed fresh every time rather than stored in
/// State, so it can never drift out of sync with `selected`.
static size_t computeScrollOffset(size_t selected, size_t total) {
    if (total <= MAX_VISIBLE) return 0;
    size_t offset = selected < MAX_VISIBLE ? 0 : selected - MAX_VISIBLE + 1;
    size_t max_offset = total - MAX_VISIBLE;
    if (offset > max_offset) offset = max_offset;
    return offset;
}

// ---- launching --------------------------------------------------

/// Runs a desktop entry's (field-code-stripped) Exec= line via
/// `/bin/sh -c`, in a detached grandchild, then returns immediately --
/// the launcher's own process exits right after, it never waits
/// around. A shell is what actually understands Exec='s quoting and
/// multi-word command lines; a bare execvp() of the first token
/// wouldn't.
static void launch(const char *exec_cmd) {
    pid_t pid = fork();
    if (pid < 0) return; // nothing we can do; just exit below
    if (pid == 0) {
        (void)setsid();
        char *argv[] = { "/bin/sh", "-c", (char *)exec_cmd, NULL };
        (void)execv("/bin/sh", argv);
        _exit(127); // only reached if execv failed
    }
}

// ---- UI state ---------------------------------------------------

typedef struct {
    char query[MAX_QUERY];
    size_t query_len;
    size_t selected; // index into the currently-filtered list
} State;

static void pushChar(State *self, char ch) {
    if (self->query_len >= MAX_QUERY) return;
    self->query[self->query_len] = ch;
    self->query_len += 1;
    self->selected = 0;
}

static void backspace(State *self) {
    if (self->query_len == 0) return;
    self->query_len -= 1;
    self->selected = 0;
}

static void draw(cairo_t *cr, const Entry *all, size_t all_len, const State *state) {
    color_set_source(BG, cr);
    cairo_paint(cr);

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 16);

    // Input line, with a simple block cursor at the end -- no
    // mid-string editing, so the cursor is always at query_len.
    color_set_source(FG, cr);
    cairo_move_to(cr, 12, INPUT_HEIGHT / 2 + 6);
    char qbuf[MAX_QUERY + 1];
    const char *q = state->query;
    size_t q_len = state->query_len;
    memcpy(qbuf, q, q_len);
    qbuf[q_len] = 0;
    cairo_show_text(cr, qbuf);

    color_set_source(ACCENT, cr);
    double cursor_x = 12 + (double)q_len * 9.6;
    cairo_rectangle(cr, cursor_x, 8, 2, INPUT_HEIGHT - 16);
    cairo_fill(cr);

    color_set_source(DIM, cr);
    cairo_rectangle(cr, 0, INPUT_HEIGHT - 1, WIN_WIDTH, 1);
    cairo_fill(cr);

    // Result rows. `selected` is an absolute index into the full
    // filtered list (it can go past MAX_VISIBLE), so figure out which
    // window of the list is actually on screen before filling
    // matched_buf, instead of always showing the first MAX_VISIBLE
    // matches like before.
    Entry count_buf[MAX_VISIBLE];
    size_t total = filterInto(all, all_len, q, q_len, 0, count_buf, MAX_VISIBLE);
    size_t selected = total == 0 ? 0 : min_sz(state->selected, total - 1);
    size_t scroll = computeScrollOffset(selected, total);

    Entry matched_buf[MAX_VISIBLE];
    (void)filterInto(all, all_len, q, q_len, scroll, matched_buf, MAX_VISIBLE);
    size_t shown = min_sz(total - scroll, MAX_VISIBLE);

    for (size_t i = 0; i < shown; i += 1) {
        size_t y = INPUT_HEIGHT + i * ROW_HEIGHT;
        if (i == selected - scroll) {
            color_set_source(DIM, cr);
            cairo_rectangle(cr, 0, (double)y, WIN_WIDTH, ROW_HEIGHT);
            cairo_fill(cr);
            color_set_source(ACCENT, cr);
        } else {
            color_set_source(FG, cr);
        }
        cairo_move_to(cr, 12, (double)y + ROW_HEIGHT / 2 + 5);
        char nbuf[256];
        size_t n = min_sz(matched_buf[i].name_len, sizeof nbuf - 1);
        memcpy(nbuf, matched_buf[i].name, n);
        nbuf[n] = 0;
        cairo_show_text(cr, nbuf);
    }

    // Small "more above"/"more below" ticks on the right edge of the
    // list, so scrolling has a visible cue instead of results just
    // silently changing underneath the highlighted row.
    color_set_source(ACCENT, cr);
    if (scroll > 0) {
        cairo_move_to(cr, WIN_WIDTH - 14, INPUT_HEIGHT + 4);
        cairo_line_to(cr, WIN_WIDTH - 8, INPUT_HEIGHT + 4);
        cairo_line_to(cr, WIN_WIDTH - 11, INPUT_HEIGHT - 2);
        cairo_fill(cr);
    }
    if (scroll + shown < total) {
        size_t by = INPUT_HEIGHT + shown * ROW_HEIGHT;
        cairo_move_to(cr, WIN_WIDTH - 14, (double)by - 4);
        cairo_line_to(cr, WIN_WIDTH - 8, (double)by - 4);
        cairo_line_to(cr, WIN_WIDTH - 11, (double)by + 2);
        cairo_fill(cr);
    }

    // Amber border around the whole popup, drawn last so it isn't
    // painted over by the background or row fills. Inset by half the
    // line width so the stroke lands fully inside the window instead
    // of being clipped at the edge.
    color_set_source(ACCENT, cr);
    cairo_set_line_width(cr, BORDER_WIDTH);
    cairo_rectangle(cr, BORDER_WIDTH / 2, BORDER_WIDTH / 2,
        WIN_WIDTH - BORDER_WIDTH, WIN_HEIGHT - BORDER_WIDTH);
    cairo_stroke(cr);

    cairo_surface_flush(cairo_get_target(cr));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Entry *all = NULL;
    size_t all_len = 0;
    // Zig's `try scanDesktopEntries(arena)` -- the only failure that
    // could escape main; there it exits with an error return code,
    // here with 1.
    if (!scanDesktopEntries(&all, &all_len)) return 1;

    Display *disp = XOpenDisplay(NULL);
    if (disp == NULL) {
        fprintf(stderr, "raillauncher: cannot open X display\n");
        return 0;
    }

    int screen = XDefaultScreen(disp);
    Window root = XRootWindow(disp, screen);
    int screen_w = XDisplayWidth(disp, screen);
    int screen_h = XDisplayHeight(disp, screen);

    int x = (screen_w - WIN_WIDTH) / 2;
    int y = (screen_h - WIN_HEIGHT) / 2; // dead center of the screen

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof attrs);
    attrs.override_redirect = True;
    attrs.background_pixel = 0;
    attrs.event_mask = KeyPressMask | ExposureMask;

    Window win = XCreateWindow(disp, root, x, y, WIN_WIDTH, WIN_HEIGHT, 0,
        CopyFromParent, InputOutput, NULL,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    (void)XMapRaised(disp, win);
    (void)XSetInputFocus(disp, win, RevertToParent, CurrentTime);

    // Paint the real UI immediately on map, before even attempting the
    // keyboard grab below. Previously the window only ever got its
    // first draw() call after a successful grab, so for however long
    // the grab retry loop took, the window showed nothing but its raw
    // background_pixel -- a plain black square -- which is exactly
    // what made a slow-to-succeed (or genuinely failing) grab look
    // broken rather than just "still starting up".
    cairo_surface_t *surface = cairo_xlib_surface_create(disp, win,
        XDefaultVisual(disp, screen), WIN_WIDTH, WIN_HEIGHT);
    cairo_t *cr = cairo_create(surface);

    State state = { .query_len = 0, .selected = 0 };
    draw(cr, all, all_len, &state);

    // override_redirect windows are never handed input by a WM, so
    // grab the keyboard outright for as long as we're up -- this is a
    // modal popup, not a regular managed window.
    //
    // A grab attempt right after spawning routinely fails at first,
    // for a completely ordinary reason: raillauncher is normally
    // launched by a hotkey (Alt+d), and railwm's own XGrabKey for that
    // binding holds an *implicit* active grab on the keyboard for as
    // long as the user is still physically holding those keys down --
    // which can easily be 100ms+. Until that releases, this process's
    // own XGrabKeyboard comes back AlreadyGrabbed even though nothing
    // is actually wrong. (The same call can also race a *previous*
    // raillauncher instance's grab still tearing down, e.g. right
    // after Escape -- same fix applies.)
    //
    // The old code discarded the return value outright, so a failed
    // grab still left a fully drawn window on screen that silently
    // never received a single key event. Retrying briefly and then
    // giving up (previous version of this fix) was closer, but still
    // undershot how long a normal key-hold can last, which showed up
    // as the window flashing on screen for an instant and then
    // vanishing. Give it a much more generous budget -- two seconds,
    // well past any real hotkey-hold or previous-instance teardown --
    // before actually concluding something is wrong.
    int grab_result = AlreadyGrabbed;
    for (size_t grab_attempt = 0; grab_attempt < 400; grab_attempt += 1) {
        grab_result = XGrabKeyboard(disp, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (grab_result == GrabSuccess) break;
        (void)usleep(5000); // 5ms
    }
    if (grab_result != GrabSuccess) {
        fprintf(stderr, "raillauncher: could not grab keyboard after retrying, exiting\n");
        (void)XDestroyWindow(disp, win);
        return 0;
    }

    XEvent ev;
    for (;;) {
        (void)XNextEvent(disp, &ev);
        switch (ev.type) {
        case Expose:
            draw(cr, all, all_len, &state);
            break;
        case KeyPress: {
            KeySym keysym = 0;
            char buf[32];
            int n = XLookupString(&ev.xkey, buf, (int)sizeof buf, &keysym, NULL);

            switch (keysym) {
            case XK_Escape:
                return 0;
            case XK_Return:
            case XK_KP_Enter: {
                Entry count_buf[MAX_VISIBLE];
                size_t total = filterInto(all, all_len, state.query, state.query_len,
                    0, count_buf, MAX_VISIBLE);
                if (total == 0) return 0;
                size_t sel = min_sz(state.selected, total - 1);
                size_t scroll = computeScrollOffset(sel, total);
                Entry matched_buf[MAX_VISIBLE];
                (void)filterInto(all, all_len, state.query, state.query_len,
                    scroll, matched_buf, MAX_VISIBLE);
                Entry pick = matched_buf[sel - scroll];
                char execbuf[1024];
                size_t elen = min_sz(pick.exec_len, sizeof execbuf - 1);
                memcpy(execbuf, pick.exec, elen);
                execbuf[elen] = 0;
                launch(execbuf);
                return 0;
            }
            case XK_Up:
                if (state.selected > 0) state.selected -= 1;
                draw(cr, all, all_len, &state);
                break;
            case XK_Down: {
                // `selected` is an absolute index into the full
                // filtered list now, not just the visible
                // window, so Down can keep advancing selection
                // past MAX_VISIBLE -- draw()'s scroll math is
                // what brings the newly-selected row on screen.
                Entry count_buf[MAX_VISIBLE];
                size_t total = filterInto(all, all_len, state.query, state.query_len,
                    0, count_buf, MAX_VISIBLE);
                if (total > 0 && state.selected + 1 < total) state.selected += 1;
                draw(cr, all, all_len, &state);
                break;
            }
            case XK_BackSpace:
                backspace(&state);
                draw(cr, all, all_len, &state);
                break;
            default:
                if (n == 1 && (unsigned char)buf[0] >= 0x20 && (unsigned char)buf[0] < 0x7f) {
                    pushChar(&state, buf[0]);
                    draw(cr, all, all_len, &state);
                }
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}
