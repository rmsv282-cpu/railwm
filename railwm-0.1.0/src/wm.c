// railwm -- a minimal scrolling-tiling X11 window manager, written in
// C. Same event-handling shape and same animation technique as the
// original wm.c (which went wm.c -> Python scrollwm -> C -> Zig and is
// now back in C): a single scrolling strip of equal-width columns, one
// window each, plus floating windows managed by EWMH heuristics. The
// three bugs fixed on the Python side are handled correctly here from
// the start:
//
//   1. Setting a background color alone doesn't repaint anything; you
//      need an explicit clear (XClearArea) to force it.
//   2. A hardcoded terminal binary silently does nothing if it isn't
//      installed -- resolve a list of candidates via a PATH search.
//   3. Floating windows got input focus explicitly; tiled windows
//      never did. Focus is synced for whichever window is actually
//      focused, every time layout changes.

#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ewmh.h"
#include "layout.h"

#define FLOAT_DEFAULT_W 640
#define FLOAT_DEFAULT_H 480
#define MAX_WINDOWS 256

// Same divisor-based step/snap easing as the layout's own scroll
// animation: move a fraction of the remaining distance each tick, snap
// once within a couple pixels so it actually settles instead of
// crawling forever. ANIM_TICK_MS also doubles as the poll() timeout in
// run() -- see there for why that only applies while something's
// actually moving.
#define ANIM_STEP_DIVISOR 6
#define ANIM_SNAP_THRESHOLD_PX 2
#define ANIM_TICK_MS 16 // ~60fps while animating

// ---- built-in defaults ---------------------------------------------
// No config file, no runtime overrides -- these are the only values
// that exist until load_config() decides otherwise. Colors are X color
// names (anything XAllocNamedColor accepts), string literals so they
// can go straight to the X11 call without any copying.
static const char *const BACKGROUND_COLOR = "#0a1428";
static const char *const FOCUSED_BORDER_COLOR = "#ffbf00";
static const char *const UNFOCUSED_BORDER_COLOR = "#4d3800";
#define BORDER_WIDTH 3

// Candidates tried in order by resolve_terminal() (PATH search, see
// there). Fix #2 from the Python port: a single hardcoded binary
// silently does nothing if it isn't installed.
// foot deliberately excluded: it's Wayland-only and refuses to run
// under X11 at all, so on this (X11-only) WM it would just be a
// guaranteed-dead entry if resolution ever reached it.
static const char *const DEFAULT_TERMINALS[] = {
    "term", "xterm", "urxvt", "rxvt", "st", "alacritty", "kitty",
};
#define DEFAULT_TERMINALS_COUNT ((int)(sizeof(DEFAULT_TERMINALS) / sizeof(DEFAULT_TERMINALS[0])))

// Binary name the launcher keybind execs. Unlike DEFAULT_TERMINALS,
// this isn't pre-resolved against $PATH at startup -- see
// launch_launcher() for why.
static const char *const LAUNCHER_BIN = "raillauncher";

// Screenshot binary the Print keybind execs, via a shell so the
// timestamped output filename can be built with `date` at the moment
// the key is pressed rather than baked into a fixed argv.
static const char *const SCREENSHOT_BIN = "maim";

typedef enum {
    ACTION_FOCUS_NEXT,
    ACTION_FOCUS_PREV,
    ACTION_MOVE_RIGHT,
    ACTION_MOVE_LEFT,
    ACTION_TERMINAL,
    ACTION_LAUNCHER,
    ACTION_MAXIMIZE,
    ACTION_FULLSCREEN,
    ACTION_SHRINK,
    ACTION_GROW,
    ACTION_KILL,
    ACTION_QUIT,
    ACTION_SCREENSHOT,
    ACTION_SCREENSHOT_REGION,
    ACTION_LAYOUT_SWITCH,
    /* arg = 0-based workspace index. */
    ACTION_WORKSPACE,
    /* arg = 0-based workspace index. */
    ACTION_MOVE_TO_WORKSPACE,
} KeyAction;

typedef struct {
    unsigned int mod;
    KeySym keysym;
    KeyAction action;
    int32_t arg;
} KeyBinding;

// Upper bound for wm.key_bindings -- DEFAULT_KEY_BINDINGS count plus
// generous headroom for a config file that adds workspace/
// move_to_workspace binds beyond the default 9, or just rebinds
// everything under a different modifier. load_config() silently stops
// reading further `bind =` lines once this is hit (see there).
#define MAX_KEY_BINDINGS 64

// Default bindings, used as-is unless the config file has at least
// one `bind =` line (see load_config()), in which case those lines
// replace this whole list rather than merging with it. Built directly
// out of X11 modifier masks and keysyms -- no string parsing needed
// for the compiled-in defaults, only for a config override. Mod4 =
// Super key. Focus/move use Alt (Mod1); everything else that isn't a
// workspace switch stays on Mod4.
static const KeyBinding DEFAULT_KEY_BINDINGS[] = {
    { Mod1Mask, XK_Right, ACTION_FOCUS_NEXT, 0 },
    { Mod1Mask, XK_Left, ACTION_FOCUS_PREV, 0 },
    { Mod1Mask | ShiftMask, XK_Right, ACTION_MOVE_RIGHT, 0 },
    { Mod1Mask | ShiftMask, XK_Left, ACTION_MOVE_LEFT, 0 },
    { Mod1Mask, XK_t, ACTION_TERMINAL, 0 },
    { Mod1Mask, XK_d, ACTION_LAUNCHER, 0 },
    { Mod1Mask, XK_f, ACTION_MAXIMIZE, 0 },
    { Mod1Mask | ShiftMask, XK_f, ACTION_FULLSCREEN, 0 },
    { Mod4Mask, XK_minus, ACTION_SHRINK, 0 },
    { Mod4Mask, XK_equal, ACTION_GROW, 0 },
    { Mod1Mask, XK_q, ACTION_KILL, 0 },
    { Mod1Mask, XK_space, ACTION_LAYOUT_SWITCH, 0 },
    { Mod1Mask | ShiftMask, XK_e, ACTION_QUIT, 0 },
    { 0, XK_Print, ACTION_SCREENSHOT, 0 },
    { ShiftMask, XK_Print, ACTION_SCREENSHOT_REGION, 0 },
    { Mod4Mask, XK_1, ACTION_WORKSPACE, 0 },
    { Mod4Mask, XK_2, ACTION_WORKSPACE, 1 },
    { Mod4Mask, XK_3, ACTION_WORKSPACE, 2 },
    { Mod4Mask, XK_4, ACTION_WORKSPACE, 3 },
    { Mod4Mask, XK_5, ACTION_WORKSPACE, 4 },
    { Mod4Mask, XK_6, ACTION_WORKSPACE, 5 },
    { Mod4Mask, XK_7, ACTION_WORKSPACE, 6 },
    { Mod4Mask, XK_8, ACTION_WORKSPACE, 7 },
    { Mod4Mask, XK_9, ACTION_WORKSPACE, 8 },
    { Mod4Mask | ShiftMask, XK_1, ACTION_MOVE_TO_WORKSPACE, 0 },
    { Mod4Mask | ShiftMask, XK_2, ACTION_MOVE_TO_WORKSPACE, 1 },
    { Mod4Mask | ShiftMask, XK_3, ACTION_MOVE_TO_WORKSPACE, 2 },
    { Mod4Mask | ShiftMask, XK_4, ACTION_MOVE_TO_WORKSPACE, 3 },
    { Mod4Mask | ShiftMask, XK_5, ACTION_MOVE_TO_WORKSPACE, 4 },
    { Mod4Mask | ShiftMask, XK_6, ACTION_MOVE_TO_WORKSPACE, 5 },
    { Mod4Mask | ShiftMask, XK_7, ACTION_MOVE_TO_WORKSPACE, 6 },
    { Mod4Mask | ShiftMask, XK_8, ACTION_MOVE_TO_WORKSPACE, 7 },
    { Mod4Mask | ShiftMask, XK_9, ACTION_MOVE_TO_WORKSPACE, 8 },
};
#define DEFAULT_KEY_BINDINGS_COUNT ((int)(sizeof(DEFAULT_KEY_BINDINGS) / sizeof(DEFAULT_KEY_BINDINGS[0])))

// Modifier bits that actually matter for chord matching -- grab_keys()
// below already grabs every combination of Lock/Mod2 along with each
// real binding, so a held Caps/Num Lock has already been "absorbed" by
// the time an event arrives here and shouldn't stop it matching a
// plain binding.
#define RELEVANT_MOD_MASK \
    (ShiftMask | ControlMask | Mod1Mask | Mod3Mask | Mod4Mask | Mod5Mask)

static const char *const FALLBACK_DIRS[] = {
    "/usr/bin", "/bin", "/usr/local/bin", "/usr/X11R6/bin",
};
#define FALLBACK_DIRS_COUNT ((int)(sizeof(FALLBACK_DIRS) / sizeof(FALLBACK_DIRS[0])))

typedef struct {
    unsigned long id;
    Window xwin;
    bool floating;

    // i3's technique (src/handlers.c), not a counter: record the X
    // protocol SEQUENCE NUMBER of each XUnmapWindow request we issue
    // ourselves, and match incoming UnmapNotify events against that
    // exact number. A serial match identifies "this is the one I just
    // caused" with no ambiguity, regardless of how many other events
    // happen in between. has_ignore_unmap distinguishes "no pending
    // self-unmap" from a legitimate serial value of 0.
    bool has_ignore_unmap;
    unsigned long ignore_unmap_serial;

    // Whether this window is CURRENTLY mapped, so apply_layout only
    // calls XUnmapWindow on an actual mapped->unmapped transition --
    // X does not generate a new UnmapNotify for an already-unmapped
    // window, so calling XUnmapWindow again would just be a silent
    // no-op that never gets matched against anything.
    bool mapped;

    // Animation state. has_rect is false until this window has been
    // placed at least once -- the first placement snaps directly.
    // After that, apply_layout() only updates target_*, and
    // step_animations() moves cur_* toward it a bit each tick until
    // they match.
    bool has_rect;
    int32_t cur_x, cur_y, cur_w, cur_h;
    int32_t target_x, target_y, target_w, target_h;

    // Set only during a workspace-switch slide: this window belongs to
    // the workspace being left and is animating off-screen rather than
    // unmapping immediately. step_animations() unmaps it for real once
    // it settles at its target.
    bool pending_unmap;

    // Fullscreen: covers the entire screen including the bar, with no
    // border, raised above everything. Handled at the WM level rather
    // than layout.c's tiling, since it applies the same way to a
    // floating window or a tiled column.
    bool fullscreen;
    int32_t saved_x, saved_y, saved_w, saved_h;
} ManagedWindow;

typedef struct {
    Display *disp;
    int screen_num;
    Window root;
    int screen_width;
    int screen_height;

    ScrollLayout layout;
    EWMH ewmh;

    /* Reserved top-edge pixels from the tallest _NET_WM_STRUT_PARTIAL
     * advertisement among the dock/bar windows. Zero when none. */
    int strut_top;

    ManagedWindow windows[MAX_WINDOWS];
    int nwindows;
    unsigned long focused_floating; // 0 = none

    char *terminal_argv[8];
    char runtime_log_path[512];

    Colormap colormap;
    unsigned long focused_border;
    unsigned long unfocused_border;
    unsigned long bg_pixel;

    // Runtime config (see load_config()) -- every field here defaults
    // to the compile-time constants above and is only overwritten if
    // ~/.config/railwm/config exists and actually sets that key. Fixed
    // buffers rather than pointers into somewhere else, since these
    // need to survive for the WM's entire lifetime (colors are
    // re-read every time a window's border needs (re)coloring)
    // without any ongoing allocator dependency.
    int32_t border_width;
    char bg_color_buf[64];
    char focused_border_buf[64];
    char unfocused_border_buf[64];
    // Comma-separated extra terminal candidates from config, tried
    // BEFORE the built-in DEFAULT_TERMINALS list. Kept as one raw
    // buffer (split at resolve_terminal() time) rather than a
    // pre-split array, since parsing happens once at startup either
    // way and this avoids needing per-entry storage here too.
    char extra_terminal_buf[256];

    /* Number of comma-separated layouts from `setxkbmap -query` at
     * startup (e.g. "us,de" -> 2). XKB groups wrap at 4 regardless,
     * so this is clamped to that. Defaults to 1 (nothing to cycle) if
     * the query fails or a layout keybind never actually needs it. */
    unsigned char layout_count;

    // Runtime config, applied to wm.layout right after layout_init()
    // (see wm_init) -- defaults match layout.c's own DEFAULT_*
    // constants, only overwritten if the config file sets these keys.
    int32_t gap;
    int32_t default_column_width;
    int32_t min_column_width;

    // Configurable binary names, same override pattern as `terminal`
    // above but simpler -- exactly one name each, no PATH-search
    // fallback list to build (launch_launcher/launchScreenshot already
    // exec lazily via execvp's own PATH search).
    char launcher_bin_buf[128];
    char screenshot_bin_buf[128];

    // Runtime keybindings (see load_config() and parse_binding()).
    // Defaults to DEFAULT_KEY_BINDINGS below; a config `bind =` line
    // replaces the whole set the first time one is seen (not merged),
    // so a user who wants only three bindings just writes three lines
    // rather than fighting the compiled-in defaults for the rest.
    KeyBinding key_bindings[MAX_KEY_BINDINGS];
    int nkey_bindings;

    // Config hot-reload (see setup_config_watch() / reload_config()
    // below). -1 means "not watching" -- either inotify_init1/add_watch
    // failed, or ~/.config/railwm doesn't exist yet, in either case
    // the WM just runs exactly as it always has, no reload
    // capability, no error.
    int inotify_fd;
    int inotify_wd;
} WM;

// ---- logging -----------------------------------------------------

static struct timespec monotonic_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    return ts;
}

// Millisecond timestamps: the gap between two log lines' timestamps is
// the actual answer to "where did the delay go", not another guess.
static void wm_log(WM *wm, const char *fmt, ...) {
    // Zig's std.fmt.bufPrint(&buf, fmt, args) catch return: a message
    // that doesn't fit the 512-byte scratch buffer is dropped
    // entirely -- and before the log file is even opened -- not
    // written truncated.
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof buf)
        return;

    FILE *file = fopen(wm->runtime_log_path, "a");
    if (file == NULL)
        return;

    struct timespec ts = monotonic_now();
    fprintf(file, "[%ld.%03ld] ", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000));
    fwrite(buf, 1, (size_t)n, file);
    fputc('\n', file);
    fclose(file);
}

// ---- managed window bookkeeping ----------------------------------

static ManagedWindow *find_window(WM *wm, unsigned long id) {
    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].id == id)
            return &wm->windows[i];
    }
    return NULL;
}

static ManagedWindow *add_managed(WM *wm, unsigned long id, Window xwin, bool floating) {
    if (wm->nwindows >= MAX_WINDOWS)
        return NULL;
    ManagedWindow *m = &wm->windows[wm->nwindows];
    wm->nwindows += 1;
    // Full memset (not field-by-field), so every other field resets --
    // this slot may be reused from a previously-removed window that
    // didn't clear the vacated tail slot, and a leftover
    // has_ignore_unmap/pending_unmap here would make this window's
    // first real close silently ignored.
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->xwin = xwin;
    m->floating = floating;
    return m;
}

static void remove_managed(WM *wm, unsigned long id) {
    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].id == id) {
            for (int j = i; j + 1 < wm->nwindows; j++)
                wm->windows[j] = wm->windows[j + 1];
            wm->nwindows -= 1;
            return;
        }
    }
}

// ---- terminal resolution -------------------------------------------

static bool is_executable(const char *path) {
    return access(path, X_OK) == 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash != NULL)
        return slash + 1;
    return path;
}

static char *dup_z(const char *s) {
    return strdup(s);
}

// xterm, urxvt, and rxvt all default to Xft/fontconfig font rendering,
// which does two things worth killing outright: (1) the classic
// white-background flash and (2) a slow fontconfig directory scan.
// "-fn fixed" selects a built-in core X font instead, bypassing
// fontconfig entirely, and -bg/-fg force the dark colors from the
// start. All three share the same X-toolkit-style flag names.
static void build_terminal_argv(WM *wm, const char *path) {
    int i = 0;
    wm->terminal_argv[i] = dup_z(path);
    i += 1;

    const char *base = base_name(path);
    if (strcmp(base, "xterm") == 0 || strcmp(base, "urxvt") == 0 ||
        strcmp(base, "rxvt") == 0) {
        wm->terminal_argv[i] = dup_z("-bg");
        i += 1;
        wm->terminal_argv[i] = dup_z("black");
        i += 1;
        wm->terminal_argv[i] = dup_z("-fg");
        i += 1;
        wm->terminal_argv[i] = dup_z("white");
        i += 1;
        wm->terminal_argv[i] = dup_z("-fn");
        i += 1;
        wm->terminal_argv[i] = dup_z("fixed");
        i += 1;
    }
    if (i < (int)(sizeof(wm->terminal_argv) / sizeof(wm->terminal_argv[0])))
        wm->terminal_argv[i] = NULL;
}

// ---- config file -----------------------------------------------
// Optional ~/.config/railwm/config: simple `key=value` lines, `#`
// comments and blank lines ignored -- same shape as raillauncher's
// own .desktop-file parser, for consistency across this project's two
// config-ish readers. Every setting here defaults to the compile-time
// constants above; a missing file, or a key the file doesn't mention,
// silently keeps that default -- there's no partial/invalid state,
// just "as much of the file as parsed, defaults for the rest."
//
// Supported keys: border_width (integer), background_color,
// focused_border_color, unfocused_border_color (X color specs, same
// syntax XAllocNamedColor accepts -- "#rrggbb" or a named color like
// "black"), and terminal (exactly one binary name or absolute/~ path
// -- not a list. If set, it's the only terminal railwm will ever try;
// it does not get combined with, or fall back to, DEFAULT_TERMINALS.
// A comma-separated value is rejected outright rather than silently
// picking one of them for you.
//
// Also: gap, column_width, min_column_width (integers, pixels --
// applied to wm.layout right after it's constructed), launcher_bin,
// screenshot_bin (single binary names, same override shape as
// terminal but no PATH-search fallback list), and bind (repeatable --
// see parse_binding() below for the "mods, key, action[:arg]" syntax
// and note that the first bind= line replaces every compiled-in
// default rather than adding to it).

// Truncating copy, matching the original's `@min(val.len, buf.len - 1)`
// + memcpy shape exactly -- overlong config values are cut, not
// rejected, and the buffer stays NUL-terminated either way. Written as
// memcpy rather than snprintf("%s") purely so the compiler can see
// the truncation is deliberate.
static void copy_truncated(char *buf, size_t bufsize, const char *val) {
    size_t n = strlen(val);
    if (n > bufsize - 1)
        n = bufsize - 1;
    memcpy(buf, val, n);
    buf[n] = 0;
}

static void set_color_field(char *buf, size_t bufsize, const char *val) {
    copy_truncated(buf, bufsize, val);
}

static void set_str_field(char *buf, size_t bufsize, const char *val) {
    copy_truncated(buf, bufsize, val);
}

// std.mem.trim(u8, ..., " \t") equivalent -- spaces and tabs only.
// (\r and \n are stripped separately where the Zig original strips
// them, e.g. the config line reader below.)
static const char *trim(const char *s, const char **end_out) {
    while (*s == ' ' || *s == '\t')
        s++;
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end_out = end;
    return s;
}

// std.fmt.parseInt(i32, s, 10) equivalent: optional single +/- sign,
// then one or more base-10 digits and nothing else -- no whitespace,
// no 0x prefix, no underscores -- with the value inside i32 range
// (for a negative sign the magnitude may reach 2147483648, since
// -2147483648 is a valid i32). Returns false on anything Zig's
// parseInt would turn into an error.
static bool parse_i32(const char *s, int32_t *out) {
    if (*s == '\0')
        return false;
    bool neg = false;
    if (*s == '+')
        s++;
    else if (*s == '-') {
        neg = true;
        s++;
    }
    if (*s == '\0')
        return false;
    const int64_t cap = neg ? 2147483648LL : 2147483647LL;
    int64_t acc = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
        acc = acc * 10 + (*p - '0');
        if (acc > cap)
            return false;
    }
    *out = (int32_t)(neg ? -acc : acc);
    return true;
}

// One name per modifier bit, the common aliases included ("alt" for
// Mod1, "super"/"win" for Mod4 -- what most other WMs' configs call
// them) since nothing here has to match X11's own naming.
static bool parse_mod_name(const char *name, unsigned int *out) {
    if (strcmp(name, "shift") == 0) { *out = ShiftMask; return true; }
    if (strcmp(name, "ctrl") == 0 || strcmp(name, "control") == 0) { *out = ControlMask; return true; }
    if (strcmp(name, "alt") == 0 || strcmp(name, "mod1") == 0) { *out = Mod1Mask; return true; }
    if (strcmp(name, "mod2") == 0) { *out = Mod2Mask; return true; }
    if (strcmp(name, "mod3") == 0) { *out = Mod3Mask; return true; }
    if (strcmp(name, "super") == 0 || strcmp(name, "win") == 0 || strcmp(name, "mod4") == 0) { *out = Mod4Mask; return true; }
    if (strcmp(name, "mod5") == 0) { *out = Mod5Mask; return true; }
    return false;
}

static const struct {
    const char *name;
    KeyAction action;
} ACTION_NAMES[] = {
    { "focus_next", ACTION_FOCUS_NEXT },
    { "focus_prev", ACTION_FOCUS_PREV },
    { "move_right", ACTION_MOVE_RIGHT },
    { "move_left", ACTION_MOVE_LEFT },
    { "terminal", ACTION_TERMINAL },
    { "launcher", ACTION_LAUNCHER },
    { "maximize", ACTION_MAXIMIZE },
    { "fullscreen", ACTION_FULLSCREEN },
    { "shrink", ACTION_SHRINK },
    { "grow", ACTION_GROW },
    { "kill", ACTION_KILL },
    { "quit", ACTION_QUIT },
    { "screenshot", ACTION_SCREENSHOT },
    { "screenshot_region", ACTION_SCREENSHOT_REGION },
    { "layout_switch", ACTION_LAYOUT_SWITCH },
    { "workspace", ACTION_WORKSPACE },
    { "move_to_workspace", ACTION_MOVE_TO_WORKSPACE },
};
#define ACTION_NAMES_COUNT ((int)(sizeof(ACTION_NAMES) / sizeof(ACTION_NAMES[0])))

// Parses one `bind = <mods>, <key>, <action>[:<arg>]` value (the part
// after the `=`) into a KeyBinding. Returns false on any malformed
// piece -- caller logs and skips the whole line rather than guessing
// at a partial match, same "no partial/invalid state" policy as the
// rest of this parser.
static bool parse_binding(const char *val, KeyBinding *out) {
    // Split into exactly three comma-separated fields.
    const char *p = val;
    const char *fields[4];
    size_t flen[4];
    int nfields = 0;
    while (nfields < 4) {
        const char *comma = strchr(p, ',');
        if (comma == NULL) {
            fields[nfields] = p;
            flen[nfields] = strlen(p);
            nfields++;
            break;
        }
        fields[nfields] = p;
        flen[nfields] = (size_t)(comma - p);
        nfields++;
        p = comma + 1;
    }
    if (nfields != 3)
        return false; // extra comma-separated field -- malformed

    char mods_str[512], key_str[512], action_str[512];
    char (*outs[3])[512] = { &mods_str, &key_str, &action_str };
    for (int i = 0; i < 3; i++) {
        char tmp[512];
        size_t n = flen[i] < sizeof(tmp) - 1 ? flen[i] : sizeof(tmp) - 1;
        memcpy(tmp, fields[i], n);
        tmp[n] = 0;
        const char *end;
        const char *start = trim(tmp, &end);
        size_t len = (size_t)(end - start);
        if (len >= sizeof(*outs[i]))
            return false;
        memcpy(*outs[i], start, len);
        (*outs[i])[len] = 0;
    }

    unsigned int mod = 0;
    if (mods_str[0] != '\0' && strcmp(mods_str, "none") != 0) {
        // Split on '+' WITHOUT collapsing empty runs -- Zig's
        // splitScalar yields "" for "shift++alt" / a trailing '+',
        // and parseModName("") then rejects the whole line.
        const char *p = mods_str;
        for (;;) {
            const char *plus = strchr(p, '+');
            const char *tok_end = plus != NULL ? plus : p + strlen(p);
            const char *inner_start = p;
            while (inner_start < tok_end &&
                   (*inner_start == ' ' || *inner_start == '\t'))
                inner_start++;
            const char *inner_end = tok_end;
            while (inner_end > inner_start &&
                   (inner_end[-1] == ' ' || inner_end[-1] == '\t'))
                inner_end--;
            char one[64];
            size_t len = (size_t)(inner_end - inner_start);
            if (len >= sizeof(one))
                return false;
            memcpy(one, inner_start, len);
            one[len] = 0;
            unsigned int m;
            if (!parse_mod_name(one, &m))
                return false;
            mod |= m;
            if (plus == NULL)
                break;
            p = plus + 1;
        }
    }

    if (key_str[0] == '\0' || strlen(key_str) >= 64)
        return false;
    KeySym keysym = XStringToKeysym(key_str);
    if (keysym == NoSymbol)
        return false;

    // action[:arg] -- arg only means something for workspace /
    // move_to_workspace, but harmless to parse generically and let it
    // sit unused for every other action.
    int32_t arg = 0;
    char *action_name = action_str;
    char *colon = strchr(action_str, ':');
    if (colon != NULL) {
        *colon = 0;
        if (!parse_i32(colon + 1, &arg))
            return false;
        action_name = action_str;
    }

    KeyAction action = ACTION_FOCUS_NEXT;
    bool found = false;
    for (int i = 0; i < ACTION_NAMES_COUNT; i++) {
        if (strcmp(ACTION_NAMES[i].name, action_name) == 0) {
            action = ACTION_NAMES[i].action;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    out->mod = mod;
    out->keysym = keysym;
    out->action = action;
    out->arg = arg;
    return true;
}

static void load_config(WM *wm) {
    set_color_field(wm->bg_color_buf, sizeof(wm->bg_color_buf), BACKGROUND_COLOR);
    set_color_field(wm->focused_border_buf, sizeof(wm->focused_border_buf), FOCUSED_BORDER_COLOR);
    set_color_field(wm->unfocused_border_buf, sizeof(wm->unfocused_border_buf), UNFOCUSED_BORDER_COLOR);
    wm->border_width = BORDER_WIDTH;
    wm->extra_terminal_buf[0] = 0;
    wm->gap = DEFAULT_GAP;
    wm->default_column_width = DEFAULT_COLUMN_WIDTH;
    wm->min_column_width = DEFAULT_MIN_COLUMN_WIDTH;
    set_str_field(wm->launcher_bin_buf, sizeof(wm->launcher_bin_buf), LAUNCHER_BIN);
    set_str_field(wm->screenshot_bin_buf, sizeof(wm->screenshot_bin_buf), SCREENSHOT_BIN);

    // Defaults stand until (if) the first `bind =` line is seen below,
    // at which point that first line clears this back to zero and
    // starts the user's own list from scratch -- see the `bind` case.
    wm->nkey_bindings = DEFAULT_KEY_BINDINGS_COUNT;
    memcpy(wm->key_bindings, DEFAULT_KEY_BINDINGS, sizeof(DEFAULT_KEY_BINDINGS));
    bool seen_bind_line = false;

    const char *home = getenv("HOME");
    if (home == NULL)
        return; // no HOME -- nothing to look for, defaults stand
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/.config/railwm/config", home) >= (int)sizeof(path))
        return;

    FILE *fh = fopen(path, "r");
    if (fh == NULL)
        return; // no config file -- defaults stand

    char linebuf[512];
    while (fgets(linebuf, sizeof(linebuf), fh) != NULL) {
        // Zig strips trailing \n/\r first, then trims " \t" -- a line
        // ending in "\r" followed by blanks therefore keeps its \r
        // (and later fails to parse, exactly as in the Zig original).
        size_t rawlen = strlen(linebuf);
        while (rawlen > 0 &&
               (linebuf[rawlen - 1] == '\n' || linebuf[rawlen - 1] == '\r'))
            linebuf[--rawlen] = '\0';
        const char *end;
        const char *line = trim(linebuf, &end);
        if (line == end || *line == '#')
            continue;

        const char *eq = memchr(line, '=', (size_t)(end - line));
        if (eq == NULL)
            continue;
        const char *key_end;
        const char *key = trim(line, &key_end);
        // key spans line..eq, trimmed
        char keybuf[128];
        {
            const char *ks = line;
            while (ks < eq && (*ks == ' ' || *ks == '\t'))
                ks++;
            const char *ke = eq;
            while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t'))
                ke--;
            size_t klen = (size_t)(ke - ks);
            if (klen == 0 || klen >= sizeof(keybuf))
                continue;
            memcpy(keybuf, ks, klen);
            keybuf[klen] = 0;
        }
        (void)key;
        (void)key_end;

        const char *val_end;
        const char *val_start = trim(eq + 1, &val_end);
        if (val_start == val_end)
            continue;
        char valbuf[512];
        size_t vlen = (size_t)(val_end - val_start);
        if (vlen >= sizeof(valbuf))
            vlen = sizeof(valbuf) - 1;
        memcpy(valbuf, val_start, vlen);
        valbuf[vlen] = 0;
        const char *val = valbuf;

        if (strcmp(keybuf, "border_width") == 0) {
            int32_t v;
            if (parse_i32(val, &v))
                wm->border_width = v;
        } else if (strcmp(keybuf, "background_color") == 0) {
            set_color_field(wm->bg_color_buf, sizeof(wm->bg_color_buf), val);
        } else if (strcmp(keybuf, "focused_border_color") == 0) {
            set_color_field(wm->focused_border_buf, sizeof(wm->focused_border_buf), val);
        } else if (strcmp(keybuf, "unfocused_border_color") == 0) {
            set_color_field(wm->unfocused_border_buf, sizeof(wm->unfocused_border_buf), val);
        } else if (strcmp(keybuf, "gap") == 0) {
            int32_t v;
            if (parse_i32(val, &v))
                wm->gap = v;
        } else if (strcmp(keybuf, "column_width") == 0) {
            int32_t v;
            if (parse_i32(val, &v))
                wm->default_column_width = v;
        } else if (strcmp(keybuf, "min_column_width") == 0) {
            int32_t v;
            if (parse_i32(val, &v))
                wm->min_column_width = v;
        } else if (strcmp(keybuf, "launcher_bin") == 0) {
            set_str_field(wm->launcher_bin_buf, sizeof(wm->launcher_bin_buf), val);
        } else if (strcmp(keybuf, "screenshot_bin") == 0) {
            set_str_field(wm->screenshot_bin_buf, sizeof(wm->screenshot_bin_buf), val);
        } else if (strcmp(keybuf, "bind") == 0) {
            // First bind= line seen replaces the compiled-in defaults
            // wholesale rather than adding to them -- a config that
            // wants to rebind should start from a blank slate, not
            // fight 30 defaults it never asked for.
            if (!seen_bind_line) {
                wm->nkey_bindings = 0;
                seen_bind_line = true;
            }
            if (wm->nkey_bindings >= MAX_KEY_BINDINGS) {
                fprintf(stderr, "railwm: MAX_KEY_BINDINGS reached, ignoring further bind= lines\n");
                continue;
            }
            KeyBinding kb;
            if (!parse_binding(val, &kb)) {
                fprintf(stderr, "railwm: malformed bind= line, ignoring: '%s'\n", val);
                continue;
            }
            wm->key_bindings[wm->nkey_bindings] = kb;
            wm->nkey_bindings += 1;
        } else if (strcmp(keybuf, "terminal") == 0) {
            // Single value only -- a comma here used to mean "try
            // these in order", which just meant the config could name
            // a terminal and still not deterministically pick one.
            // Reject it outright instead of guessing which candidate
            // was meant; wm_log isn't safe to call yet this early (the
            // runtime log path isn't set up until after load_config()
            // returns), so this goes straight to stderr like the other
            // pre-log-setup messages in wm_init.
            if (strchr(val, ',') != NULL) {
                fprintf(stderr,
                    "railwm: config 'terminal' takes exactly one binary, not a comma list -- ignoring '%s'\n",
                    val);
                continue;
            }
            copy_truncated(wm->extra_terminal_buf, sizeof(wm->extra_terminal_buf), val);
        }
    }
    fclose(fh);
}

// Watches the *directory* (~/.config/railwm), not the config file
// itself. Most editors (vim, and anything using the common
// write-to-tempfile-then-rename save pattern) replace the file on
// save rather than writing into the existing inode in place -- a watch
// on the file directly would silently stop firing after the very first
// save, since the inode it was attached to no longer has that path.
// Watching the directory for CLOSE_WRITE (plain in-place writes, e.g.
// a shell redirect or an editor that does write() in place) and
// MOVED_TO/CREATE (the rename-on-save pattern) catches both, and the
// actual filename is filtered in drain_inotify_and_maybe_reload()
// below so unrelated files in that directory don't trigger a reload.
//
// Failure anywhere here (no HOME, inotify_init1 fails, the directory
// doesn't exist yet because no config file was ever created) just
// leaves inotify_fd at -1 -- run()'s poll() then simply never has a
// second fd to watch, same as if this function was never called.
// Hot reload is a nice-to-have layered on top of load_config(), not a
// requirement for the WM to start.
static void setup_config_watch(WM *wm) {
    const char *home = getenv("HOME");
    if (home == NULL)
        return;
    char dir_path[1024];
    if (snprintf(dir_path, sizeof(dir_path), "%s/.config/railwm", home) >= (int)sizeof(dir_path))
        return;

    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0)
        return;

    int wd = inotify_add_watch(fd, dir_path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        close(fd);
        return;
    }
    wm->inotify_fd = fd;
    wm->inotify_wd = wd;
}

static void resolve_terminal(WM *wm);
static void update_border_colors(WM *wm);
static void apply_layout(WM *wm);
static void grab_keys(WM *wm);

// Re-parses the config file and applies every setting that isn't
// picked up automatically just by load_config() overwriting wm's
// fields -- namely anything already baked into an X pixel value
// (colors are resolved once via XAllocNamedColor, not re-read from the
// string buffer on every use) or already-grabbed X state (key
// bindings). Mirrors the exact sequence wm_init() runs once at
// startup, just re-entered instead of run for the first time.
static void reload_config(WM *wm) {
    load_config(wm);

    XColor color, exact;
    XAllocNamedColor(wm->disp, wm->colormap, wm->bg_color_buf, &color, &exact);
    wm->bg_pixel = color.pixel;
    XSetWindowBackground(wm->disp, wm->root, wm->bg_pixel);
    XClearArea(wm->disp, wm->root, 0, 0, 0, 0, True);

    XColor fcolor, funused;
    XAllocNamedColor(wm->disp, wm->colormap, wm->focused_border_buf, &fcolor, &funused);
    wm->focused_border = fcolor.pixel;
    XColor ucolor, uunused;
    XAllocNamedColor(wm->disp, wm->colormap, wm->unfocused_border_buf, &ucolor, &uunused);
    wm->unfocused_border = ucolor.pixel;

    wm->layout.gap = wm->gap;
    wm->layout.default_column_width = wm->default_column_width;
    wm->layout.min_column_width = wm->min_column_width;

    // A configured `terminal` value only lives in extra_terminal_buf
    // after load_config() above -- resolve_terminal() is what actually
    // turns that into wm->terminal_argv (the exec() argv used every
    // time a terminal is launched), so it has to be re-run here too,
    // not just at startup, or a changed `terminal` line would parse
    // fine but never take effect until a full restart.
    resolve_terminal(wm);

    update_border_colors(wm);
    apply_layout(wm);

    // Drop every previous grab before re-grabbing -- a new bind= set
    // can be smaller than the old one, and stale grabs on keys the new
    // config no longer claims would otherwise linger until the next
    // full restart.
    XUngrabKey(wm->disp, AnyKey, AnyModifier, wm->root);
    grab_keys(wm);

    XFlush(wm->disp);
    wm_log(wm, "config reloaded");
}

static void resolve_terminal(WM *wm) {
    // A configured "terminal" (see load_config) is now exactly one
    // value, and when it's set, it is the *only* thing tried below --
    // no blending with DEFAULT_TERMINALS, no auto-detection guessing
    // on top of an explicit choice. DEFAULT_TERMINALS auto-detection
    // only runs when nothing was configured at all.
    const char *configured[1];
    const char *const *terms = DEFAULT_TERMINALS;
    int nterms = DEFAULT_TERMINALS_COUNT;
    char configured_buf[128];
    if (wm->extra_terminal_buf[0] != 0) {
        const char *end;
        const char *trimmed = trim(wm->extra_terminal_buf, &end);
        size_t n = (size_t)(end - trimmed);
        if (n >= sizeof(configured_buf))
            n = sizeof(configured_buf) - 1;
        memcpy(configured_buf, trimmed, n);
        configured_buf[n] = 0;
        configured[0] = configured_buf;
        terms = configured;
        nterms = 1;
    }

    // Pass 1: explicit absolute / ~ paths -- check as-is rather than
    // treating them as names to search $PATH for (matters on NixOS,
    // where a binary can be installed but not on this process's PATH).
    for (int i = 0; i < nterms; i++) {
        const char *entry = terms[i];
        char expanded[1024];
        const char *resolved = NULL;
        if (entry[0] == '/') {
            resolved = entry;
        } else if (entry[0] == '~' && (entry[1] == 0 || entry[1] == '/')) {
            const char *home = getenv("HOME");
            if (home != NULL) {
                int w = snprintf(expanded, sizeof(expanded), "%s%s",
                    home, entry + 1);
                if (w >= 0 && w < (int)sizeof(expanded))
                    resolved = expanded;
            }
        }
        if (resolved != NULL) {
            if (is_executable(resolved)) {
                build_terminal_argv(wm, resolved);
                wm_log(wm, "terminal resolved to absolute path: %s", resolved);
                return;
            }
            wm_log(wm, "candidate terminal path not executable, skipping: %s", resolved);
        }
    }

    const char *path_env = getenv("PATH");

    // Pass 2: $PATH name search. Split on ':' WITHOUT collapsing empty
    // runs -- Zig's splitScalar yields "" for "a::b", a leading ':' or
    // a trailing ':', producing a candidate of "/name" for that slot.
    if (path_env != NULL) {
        const char *p = path_env;
        for (;;) {
            const char *colon = strchr(p, ':');
            const char *dir_end = colon != NULL ? colon : p + strlen(p);
            for (int i = 0; i < nterms; i++) {
                const char *name = terms[i];
                if (name[0] == '/' || name[0] == '~')
                    continue; // already tried above
                char cand[1024];
                if (snprintf(cand, sizeof(cand), "%.*s/%s",
                        (int)(dir_end - p), p, name) >= (int)sizeof(cand))
                    continue;
                if (is_executable(cand)) {
                    build_terminal_argv(wm, cand);
                    wm_log(wm, "terminal resolved to: %s", cand);
                    return;
                }
            }
            if (colon == NULL)
                break;
            p = colon + 1;
        }
    }

    // Pass 3: absolute fallback dirs, in case the session launcher
    // started this WM with a stripped PATH.
    for (int i = 0; i < nterms; i++) {
        const char *name = terms[i];
        if (name[0] == '/' || name[0] == '~')
            continue;
        for (int d = 0; d < FALLBACK_DIRS_COUNT; d++) {
            char cand[1024];
            if (snprintf(cand, sizeof(cand), "%s/%s", FALLBACK_DIRS[d], name) >= (int)sizeof(cand))
                continue;
            if (is_executable(cand)) {
                build_terminal_argv(wm, cand);
                wm_log(wm, "found %s via absolute-path fallback (not on PATH=%s)",
                    cand, path_env ? path_env : "(unset)");
                return;
            }
        }
    }

    wm->terminal_argv[0] = NULL;
    wm_log(wm, "no terminal found on PATH or in fallback dirs; PATH was %s",
        path_env ? path_env : "(unset)");
}

static void launch_terminal(WM *wm) {
    const char *a0 = wm->terminal_argv[0];
    if (a0 == NULL) {
        wm_log(wm, "Alt+t pressed but no terminal is available");
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        wm_log(wm, "fork failed launching terminal");
        return;
    }
    if (pid == 0) {
        // child: detach from the WM's own session so terminal exit
        // doesn't affect us, then exec
        setsid();
        execv(a0, wm->terminal_argv);
        _exit(127); // only reached if execv failed
    }
    wm_log(wm, "launched terminal %s, pid=%d", a0, (int)pid);
}

// The launcher binary isn't pre-resolved against $PATH at startup the
// way resolve_terminal() resolves a terminal -- there's only one name,
// no argv variations to build per-candidate, so there's nothing to gain
// from doing that walk ahead of time. Instead this execs lazily, right
// when the keybind fires, and lets execvp() do its own PATH search
// (the same lookup a shell would do). A pipe with the write end marked
// close-on-exec is what makes that synchronous: if exec succeeds, the
// kernel closes pipefd[1] for us and the parent's read() returns
// immediately with 0 bytes; if exec fails, the child writes a single
// byte first so the parent's read() comes back with 1 instead. No
// polling, no guessing based on whether the process is still around a
// moment later.
static void launch_launcher(WM *wm) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        wm_log(wm, "launcher: pipe() failed, not launching");
        return;
    }
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        wm_log(wm, "launcher: fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    const char *bin = wm->launcher_bin_buf;
    if (pid == 0) {
        close(pipefd[0]);
        setsid();
        char *argv[2] = { wm->launcher_bin_buf, NULL };
        execvp(bin, argv);
        // execvp() only returns on failure. One byte down the pipe is
        // enough to tell the parent that, then this process is done.
        unsigned char fail_marker = 1;
        (void)!write(pipefd[1], &fail_marker, 1);
        _exit(127);
    }

    close(pipefd[1]);
    unsigned char marker = 0;
    ssize_t n = read(pipefd[0], &marker, 1);
    close(pipefd[0]);

    if (n == 0) {
        wm_log(wm, "launched launcher (%s), pid=%d", bin, (int)pid);
    } else {
        wm_log(wm, "launcher (%s) failed to start -- is it installed and on $PATH?", bin);
    }
}

// ---- keyboard layout switching ---------------------------------------
//
// railbar already reads the *active* layout every tick via XkbGetState;
// this is the write side. Cycling the group is one XkbLockGroup call --
// instant, no shelling out on the hot path -- but it needs to know how
// many layouts are actually configured (XKB always reports up to 4
// group slots, most of them meaningless if the user only set up one or
// two). That count is queried once at startup the same way railbar
// builds its layout list: `setxkbmap -query`'s "layout:" line, comma
// count + 1.

static void query_layout_count(WM *wm) {
    wm->layout_count = 1;
    FILE *fh = popen("setxkbmap -query 2>/dev/null", "r");
    if (fh == NULL)
        return;

    char buf[512];
    size_t total = 0;
    while (total < sizeof(buf)) {
        size_t n = fread(&buf[total], 1, sizeof(buf) - total, fh);
        if (n == 0)
            break;
        total += n;
    }
    pclose(fh);

    if (total >= sizeof(buf))
        total = sizeof(buf) - 1;
    buf[total] = 0;
    char *text = buf;

    const char *marker = "layout:";
    const char *start = strstr(text, marker);
    if (start == NULL)
        return;
    const char *rest = start + strlen(marker);
    const char *eol = strchr(rest, '\n');
    size_t rest_len = eol != NULL ? (size_t)(eol - rest) : strlen(rest);

    // Trim " \t\r" from both ends of the line.
    const char *ls = rest;
    const char *le = rest + rest_len;
    while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r'))
        ls++;
    while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r'))
        le--;
    if (ls == le)
        return;

    int count = 1;
    for (const char *p = ls; p < le; p++) {
        if (*p == ',')
            count += 1;
    }
    if (count > 4)
        count = 4; // XKB only ever has 4 group slots
    wm->layout_count = (unsigned char)count;
}

static void switch_layout(WM *wm) {
    if (wm->layout_count <= 1) {
        wm_log(wm, "layout switch pressed but only one layout is configured");
        return;
    }
    XkbStateRec state;
    if (XkbGetState(wm->disp, XkbUseCoreKbd, &state) != 0) { // non-zero = error
        wm_log(wm, "layout switch: XkbGetState failed");
        return;
    }
    unsigned int next = ((unsigned int)state.group + 1) % (unsigned int)wm->layout_count;
    XkbLockGroup(wm->disp, XkbUseCoreKbd, next);
    wm_log(wm, "layout switched to group %u/%u", next, (unsigned int)wm->layout_count);
}

// Print key -> maim, full-screen capture saved to ~/Pictures with a
// timestamped filename. Shift+Print does the same but with `-s`,
// handing off to slop for an interactive region select first (slop is
// the whole reason maim needs it installed alongside it). Goes through
// /bin/sh -c (like raillauncher's own launch()) since the filename
// embeds `date` output computed at press-time, not something a bare
// execvp() argv could build.
static void launch_screenshot(WM *wm, bool region) {
    const char *home = getenv("HOME");
    if (home == NULL)
        home = "/tmp";
    const char *bin = wm->screenshot_bin_buf;
    char cmd[1024];
    const char *flag = region ? "-s " : "";
    if (snprintf(cmd, sizeof(cmd),
            "mkdir -p '%s/Pictures' && %s %s\"%s/Pictures/screenshot-$(date +%%Y%%m%%d-%%H%%M%%S).png\"",
            home, bin, flag, home) >= (int)sizeof(cmd)) {
        wm_log(wm, "screenshot: command too long, not launching");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        wm_log(wm, "screenshot: fork failed");
        return;
    }
    if (pid == 0) {
        setsid();
        char *argv[4] = { "/bin/sh", "-c", cmd, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    wm_log(wm, "launched screenshot (%s%s), pid=%d", bin, flag, (int)pid);
}

static void reap_children(WM *wm) {
    int status = 0;
    for (;;) {
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        // WIFEXITED / WIFSIGNALED / WEXITSTATUS / WTERMSIG -- the Zig
        // original had to hand-roll this decode (wait.h's macros aren't
        // callable from @cImport); in C the macros are used directly.
        if (WIFSIGNALED(status))
            wm_log(wm, "child pid=%d killed by signal %d", (int)pid, WTERMSIG(status));
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            wm_log(wm, "child pid=%d exited with status %d", (int)pid, WEXITSTATUS(status));
    }
}

// ---- layout application -------------------------------------------

static void update_border_colors(WM *wm) {
    unsigned long tiled_focus_id = 0;
    if (wm->focused_floating == 0) {
        Column *col = layout_focused(&wm->layout);
        if (col != NULL)
            tiled_focus_id = col->window_id;
    }
    unsigned int bw = (unsigned int)wm->border_width;
    for (int i = 0; i < wm->nwindows; i++) {
        ManagedWindow *m = &wm->windows[i];
        // Fullscreen windows are borderless by design (set_fullscreen
        // sets width 0 on entry) -- apply_layout() runs on every
        // relayout while a window stays fullscreen (new window mapped,
        // workspace switch, focus change...), and without this check
        // this loop would silently reassert BORDER_WIDTH on it every
        // single time, undoing set_fullscreen's effect.
        XSetWindowBorderWidth(wm->disp, m->xwin, m->fullscreen ? 0 : bw);
        bool is_focused = (m->id == tiled_focus_id) || (m->id == wm->focused_floating);
        XSetWindowBorder(wm->disp, m->xwin, is_focused ? wm->focused_border : wm->unfocused_border);
    }
}

static void sync_input_focus(WM *wm) {
    unsigned long target_id = wm->focused_floating;
    if (target_id == 0) {
        Column *col = layout_focused(&wm->layout);
        if (col != NULL)
            target_id = col->window_id;
    }
    if (target_id == 0)
        return;
    ManagedWindow *m = find_window(wm, target_id);
    if (m == NULL)
        return;
    XSetInputFocus(wm->disp, m->xwin, RevertToParent, CurrentTime);
}

static void sync_ewmh_state(WM *wm) {
    unsigned long ids[MAX_WINDOWS];
    for (int i = 0; i < wm->nwindows; i++)
        ids[i] = wm->windows[i].id;
    ewmh_set_client_list(&wm->ewmh, ids, wm->nwindows);

    unsigned long active = wm->focused_floating;
    if (active == 0) {
        Column *col = layout_focused(&wm->layout);
        if (col != NULL)
            active = col->window_id;
    }
    ewmh_set_active_window(&wm->ewmh, active);
}

static void apply_layout(WM *wm) {
    GeomEntry geo[MAX_COLUMNS];
    size_t n = layout_geometry(&wm->layout, geo, MAX_COLUMNS);
    unsigned long visible_ids[MAX_COLUMNS];

    for (size_t i = 0; i < n; i++) {
        visible_ids[i] = geo[i].window_id;
        ManagedWindow *m = find_window(wm, geo[i].window_id);
        if (m == NULL)
            continue;
        if (m->fullscreen) {
            // leave geometry alone -- set_fullscreen owns it while
            // this flag is set
            XMapWindow(wm->disp, m->xwin);
            m->mapped = true;
            continue;
        }

        m->target_x = geo[i].x;
        m->target_y = geo[i].y;
        m->target_w = geo[i].w;
        m->target_h = geo[i].h;

        if (!m->has_rect) {
            // First time this window has ever been placed -- no
            // previous on-screen position to animate from, so snap
            // directly instead of growing/sliding in from an
            // arbitrary invented starting point.
            m->cur_x = m->target_x;
            m->cur_y = m->target_y;
            m->cur_w = m->target_w;
            m->cur_h = m->target_h;
            m->has_rect = true;
            // cur_w/cur_h are the full on-screen footprint the layout
            // intends (border included) -- X11 draws a window's border
            // OUTSIDE the width/height passed to XMoveResizeWindow, so
            // the interior must be shrunk by 2*border_width on each
            // axis for the actual footprint to match what the layout
            // computed.
            int bw = wm->border_width;
            int w = m->cur_w - 2 * bw;
            if (w < 1)
                w = 1;
            int h = m->cur_h - 2 * bw;
            if (h < 1)
                h = 1;
            XMoveResizeWindow(wm->disp, m->xwin, m->cur_x, m->cur_y, (unsigned)w, (unsigned)h);
        }
        // Otherwise: leave cur_* alone. step_animations() (called from
        // the main loop) carries it toward target_* over the next
        // several ticks.

        XMapWindow(wm->disp, m->xwin);
        m->mapped = true;
    }

    for (int i = 0; i < wm->nwindows; i++) {
        ManagedWindow *m = &wm->windows[i];
        if (m->floating)
            continue;
        bool visible = false;
        for (size_t j = 0; j < n; j++) {
            if (visible_ids[j] == m->id) {
                visible = true;
                break;
            }
        }
        if (!visible && m->mapped) {
            // NextRequest() returns the sequence number the X server
            // will assign to the NEXT request we send -- i.e. exactly
            // the number XUnmapWindow's request (and therefore its
            // UnmapNotify) will carry.
            m->ignore_unmap_serial = NextRequest(wm->disp);
            m->has_ignore_unmap = true;
            XUnmapWindow(wm->disp, m->xwin);
            m->mapped = false;
            // Forget its rect so that if it becomes visible again
            // later it's treated as a fresh placement (snap, not
            // animate in from a stale position).
            m->has_rect = false;
        }
    }

    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].floating)
            XRaiseWindow(wm->disp, wm->windows[i].xwin);
    }
    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].fullscreen)
            XRaiseWindow(wm->disp, wm->windows[i].xwin);
    }

    update_border_colors(wm);
    sync_input_focus(wm);
    sync_ewmh_state(wm);
    XFlush(wm->disp);
}

// Toggles fullscreen on a specific window: covers the whole screen
// (including the bar), border removed, raised above everything. Works
// identically for a floating or tiled window. Called both from the
// fullscreen keybind (on the focused window) and from an EWMH
// _NET_WM_STATE_FULLSCREEN request (on whatever window the client
// names, which may not be focused).
static void set_fullscreen(WM *wm, ManagedWindow *m, bool enable) {
    if (m->fullscreen == enable) {
        // Our flag already matches, but don't trust that alone -- if
        // it ever drifted from what the window's actual _NET_WM_STATE
        // property says (e.g. a client re-requesting fullscreen after
        // some intermediate unmap/remap), a client relying on that
        // property to confirm the transition would see no write and
        // could get stuck treating clicks as no-ops. Re-sync the
        // property (itself a no-op if it's already correct) and stop
        // there -- geometry/border are already right if the flag was.
        ewmh_set_fullscreen_state(&wm->ewmh, m->xwin, enable);
        return;
    }

    if (enable) {
        m->saved_x = m->cur_x;
        m->saved_y = m->cur_y;
        m->saved_w = m->cur_w;
        m->saved_h = m->cur_h;
        m->fullscreen = true;

        XSetWindowBorderWidth(wm->disp, m->xwin, 0);
        m->cur_x = 0;
        m->target_x = 0;
        m->cur_y = 0;
        m->target_y = 0;
        m->cur_w = wm->screen_width;
        m->target_w = wm->screen_width;
        m->cur_h = wm->screen_height;
        m->target_h = wm->screen_height;
        m->has_rect = true;
        XMoveResizeWindow(wm->disp, m->xwin, 0, 0,
            (unsigned)wm->screen_width, (unsigned)wm->screen_height);
        XRaiseWindow(wm->disp, m->xwin);
        ewmh_set_fullscreen_state(&wm->ewmh, m->xwin, true);
    } else {
        m->fullscreen = false;
        ewmh_set_fullscreen_state(&wm->ewmh, m->xwin, false);
        XSetWindowBorderWidth(wm->disp, m->xwin, (unsigned)wm->border_width);
        if (m->floating) {
            m->cur_x = m->saved_x;
            m->target_x = m->saved_x;
            m->cur_y = m->saved_y;
            m->target_y = m->saved_y;
            m->cur_w = m->saved_w;
            m->target_w = m->saved_w;
            m->cur_h = m->saved_h;
            m->target_h = m->saved_h;
            int bw = wm->border_width;
            int w = m->cur_w - 2 * bw;
            if (w < 1)
                w = 1;
            int h = m->cur_h - 2 * bw;
            if (h < 1)
                h = 1;
            XMoveResizeWindow(wm->disp, m->xwin, m->cur_x, m->cur_y, (unsigned)w, (unsigned)h);
        }
        // tiled: apply_layout() below recomputes real tiled geometry
        // fresh, so saved_* is never even consulted for that case.
    }
    apply_layout(wm);
}

// Same focus resolution kill_focused uses: floating focus wins if set,
// otherwise whatever column the layout has focused.
static unsigned long resolve_focus_id(WM *wm) {
    if (wm->focused_floating != 0)
        return wm->focused_floating;
    Column *col = layout_focused(&wm->layout);
    if (col != NULL)
        return col->window_id;
    return 0;
}

static void toggle_fullscreen_focused(WM *wm) {
    unsigned long id = resolve_focus_id(wm);
    if (id == 0)
        return;
    ManagedWindow *m = find_window(wm, id);
    if (m == NULL)
        return;
    set_fullscreen(wm, m, !m->fullscreen);
}

// Workspace switch, but sliding instead of the instant cut plain
// apply_layout() would produce: the outgoing workspace's windows slide
// off one edge and the incoming workspace's windows slide in from the
// other, like scrolling vertically between two strips.
//
// Direction: switching to a HIGHER-numbered workspace moves things
// UPWARD (outgoing slides off the top, incoming enters from the
// bottom). Switching to a LOWER-numbered workspace is the mirror. Only
// y moves -- x/w/h go straight to their final values.
static void switch_workspace_animated(WM *wm, int32_t ws_in) {
    int32_t new_ws = ws_in;
    int32_t old_ws = wm->layout.active_workspace;
    if (new_ws < 0)
        new_ws = 0;
    if (new_ws >= MAX_WORKSPACES)
        new_ws = MAX_WORKSPACES - 1;

    if (new_ws == old_ws) {
        // layout_switch_workspace() always re-runs even for a
        // same-workspace "switch" (see its own comment on why) --
        // mirror that here too, just without anything to slide.
        layout_switch_workspace(&wm->layout, new_ws);
        apply_layout(wm);
        return;
    }

    int sh = wm->screen_height;
    bool to_higher = new_ws > old_ws;

    // Slide the OLD workspace's currently-visible windows off-screen
    // instead of letting apply_layout() unmap them instantly. Must
    // happen before layout_switch_workspace() below changes which
    // workspace layout_geometry() reports.
    GeomEntry old_geo[MAX_COLUMNS];
    size_t old_n = layout_geometry(&wm->layout, old_geo, MAX_COLUMNS);
    for (size_t i = 0; i < old_n; i++) {
        ManagedWindow *m = find_window(wm, old_geo[i].window_id);
        if (m == NULL)
            continue;
        if (!m->has_rect)
            continue;
        // Fullscreen windows slide out exactly like any other window
        // -- only target_y changes here, so their existing
        // full-screen x/w/h (and zero border) are left completely
        // alone.
        m->target_y = m->cur_y + (to_higher ? -sh : sh);
        m->pending_unmap = true;
    }

    layout_switch_workspace(&wm->layout, new_ws);

    // Place the NEW workspace's windows starting off-screen on the
    // opposite edge, with their real target already set to where they
    // belong -- step_animations() then carries them in.
    GeomEntry new_geo[MAX_COLUMNS];
    size_t new_n = layout_geometry(&wm->layout, new_geo, MAX_COLUMNS);

    for (size_t i = 0; i < new_n; i++) {
        ManagedWindow *m = find_window(wm, new_geo[i].window_id);
        if (m == NULL)
            continue;

        // Fullscreen windows slide in too, same as everything else --
        // just using full-screen coordinates instead of the layout
        // engine's tiled geometry (g.x/y/w/h), since the layout engine
        // doesn't know about the fullscreen override.
        int32_t final_x = m->fullscreen ? 0 : new_geo[i].x;
        int32_t final_y = m->fullscreen ? 0 : new_geo[i].y;
        int32_t final_w = m->fullscreen ? wm->screen_width : new_geo[i].w;
        int32_t final_h = m->fullscreen ? wm->screen_height : new_geo[i].h;

        m->cur_x = final_x;
        m->cur_y = final_y + (to_higher ? sh : -sh);
        m->cur_w = final_w;
        m->cur_h = final_h;
        m->has_rect = true;
        m->pending_unmap = false;

        // Fullscreen windows have their border width set to 0 already
        // (set_fullscreen did that) -- subtracting wm->border_width
        // here like a normal tiled/floating window would leave a gap
        // around the edges for no reason, so skip the shrink for them.
        int bw = m->fullscreen ? 0 : wm->border_width;
        int w = m->cur_w - 2 * bw;
        if (w < 1)
            w = 1;
        int h = m->cur_h - 2 * bw;
        if (h < 1)
            h = 1;
        XMoveResizeWindow(wm->disp, m->xwin, m->cur_x, m->cur_y, (unsigned)w, (unsigned)h);
        XMapWindow(wm->disp, m->xwin);
        m->mapped = true;

        m->target_x = final_x;
        m->target_y = final_y;
        m->target_w = final_w;
        m->target_h = final_h;
    }

    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].floating)
            XRaiseWindow(wm->disp, wm->windows[i].xwin);
    }
    for (int i = 0; i < wm->nwindows; i++) {
        if (wm->windows[i].fullscreen)
            XRaiseWindow(wm->disp, wm->windows[i].xwin);
    }

    update_border_colors(wm);
    sync_input_focus(wm);
    sync_ewmh_state(wm);
    XFlush(wm->disp);
}

static bool step_one(int32_t *cur, int32_t target) {
    int32_t delta = target - *cur;
    if (delta == 0)
        return false;
    int32_t step = delta / ANIM_STEP_DIVISOR;
    if (step == 0)
        step = delta > 0 ? 1 : -1;
    *cur += step;
    int32_t rest = target - *cur;
    if (rest < 0)
        rest = -rest;
    if (rest <= ANIM_SNAP_THRESHOLD_PX)
        *cur = target;
    return true;
}

// Moves every window's cur_* rect one step closer to its target_*.
// Returns true if anything is still moving, so run()'s poll() knows
// whether to keep ticking on a short timeout or go back to blocking
// indefinitely.
static bool step_animations(WM *wm) {
    bool any = false;
    for (int i = 0; i < wm->nwindows; i++) {
        ManagedWindow *m = &wm->windows[i];
        if (!m->has_rect)
            continue;
        bool moved_x = step_one(&m->cur_x, m->target_x);
        bool moved_y = step_one(&m->cur_y, m->target_y);
        bool moved_w = step_one(&m->cur_w, m->target_w);
        bool moved_h = step_one(&m->cur_h, m->target_h);
        if (moved_x || moved_y || moved_w || moved_h) {
            // Width/height must stay positive -- X errors (BadValue) on
            // 0 or negative, which a mid-animation width could briefly
            // compute given how step_one rounds. Also shrink by
            // 2*border_width same as apply_layout's initial snap --
            // except fullscreen windows, whose border is 0, so
            // shrinking them the same way would leave a false gap.
            int bw = m->fullscreen ? 0 : wm->border_width;
            int w = m->cur_w - 2 * bw;
            if (w < 1)
                w = 1;
            int h = m->cur_h - 2 * bw;
            if (h < 1)
                h = 1;
            XMoveResizeWindow(wm->disp, m->xwin, m->cur_x, m->cur_y, (unsigned)w, (unsigned)h);
            any = true;
        } else if (m->pending_unmap && m->mapped) {
            // Slide-out finished (reached the off-screen target) -- now
            // actually unmap it, same serial-tracking dance
            // apply_layout uses so the resulting UnmapNotify doesn't
            // get mistaken for the client unmapping itself.
            m->ignore_unmap_serial = NextRequest(wm->disp);
            m->has_ignore_unmap = true;
            XUnmapWindow(wm->disp, m->xwin);
            m->mapped = false;
            m->has_rect = false;
            m->pending_unmap = false;
        }
    }
    if (any)
        XFlush(wm->disp);
    return any;
}

static void place_floating(WM *wm, Window xwin) {
    XSizeHints hints;
    long supplied = 0;
    int w = FLOAT_DEFAULT_W;
    int h = FLOAT_DEFAULT_H;

    if (XGetWMNormalHints(wm->disp, xwin, &hints, &supplied) != 0) {
        if ((hints.flags & PMaxSize) != 0 && hints.max_width > 0)
            w = hints.max_width;
        else if ((hints.flags & PBaseSize) != 0 && hints.base_width > 0)
            w = hints.base_width;
        if ((hints.flags & PMaxSize) != 0 && hints.max_height > 0)
            h = hints.max_height;
        else if ((hints.flags & PBaseSize) != 0 && hints.base_height > 0)
            h = hints.base_height;
    }

    if (w > wm->screen_width - 40)
        w = wm->screen_width - 40;
    if (h > wm->screen_height - 40)
        h = wm->screen_height - 40;
    if (w <= 0)
        w = FLOAT_DEFAULT_W;
    if (h <= 0)
        h = FLOAT_DEFAULT_H;

    int x = (wm->screen_width - w) / 2;
    int y = (wm->screen_height - h) / 2;
    XMoveResizeWindow(wm->disp, xwin, x, y, (unsigned)w, (unsigned)h);
    XSetWindowBorderWidth(wm->disp, xwin, (unsigned)wm->border_width);
}

// ---- event handlers ------------------------------------------------

static void on_map_request(WM *wm, XMapRequestEvent *ev) {
    Window win = ev->window;
    unsigned long win_id = (unsigned long)win;

    // X can legitimately redeliver a MapRequest for a window we already
    // manage (a client re-mapping itself). Without this check, that
    // produced a second duplicate entry for the same window_id in both
    // the managed list and the layout's column list -- cycling focus
    // then looked like "nothing ever changes" even though many
    // terminals were genuinely launched.
    ManagedWindow *existing = find_window(wm, win_id);
    if (existing != NULL) {
        wm_log(wm, "MapRequest for already-managed window %lu, ignoring duplicate", win_id);
        // A remap of an already-managed floating window (e.g. a GTK
        // file chooser that hides and re-shows the same window) needs
        // focus re-claimed on every remap, not just the first map.
        if (existing->floating) {
            place_floating(wm, win);
            wm->focused_floating = win_id;
            XRaiseWindow(wm->disp, win);
            XSetInputFocus(wm->disp, win, RevertToParent, CurrentTime);
        }
        XMapWindow(wm->disp, win);
        apply_layout(wm);
        return;
    }

    XSelectInput(wm->disp, win, EnterWindowMask | PropertyChangeMask);

    bool floats = ewmh_should_float(&wm->ewmh, win);
    wm_log(wm, "MapRequest for window %lu, floating=%s", win_id, floats ? "true" : "false");

    if (add_managed(wm, win_id, win, floats) == NULL) {
        wm_log(wm, "window limit reached, not managing %lu", win_id);
        return;
    }

    if (floats) {
        place_floating(wm, win);
        wm->focused_floating = win_id;
        XMapWindow(wm->disp, win);
    } else {
        layout_add_window(&wm->layout, win_id, true);
    }

    apply_layout(wm);
}

static void forget_window(WM *wm, unsigned long id) {
    ManagedWindow *m = find_window(wm, id);
    if (m == NULL)
        return;
    bool was_floating = m->floating;
    remove_managed(wm, id);
    if (was_floating && wm->focused_floating == id)
        wm->focused_floating = 0;
    layout_remove_window(&wm->layout, id); // no-op if it was never tiled
    apply_layout(wm);
}

static void kill_focused(WM *wm) {
    unsigned long id = resolve_focus_id(wm);
    if (id == 0)
        return;
    ManagedWindow *m = find_window(wm, id);
    if (m == NULL)
        return;

    // Try WM_DELETE_WINDOW first (graceful); fall back to XKillClient
    // if the window doesn't participate in WM_PROTOCOLS.
    Atom *protocols = NULL;
    int nprotocols = 0;
    bool has_delete = false;
    Atom delete_atom = wm->ewmh.atoms[ATOM_WM_DELETE_WINDOW];
    if (XGetWMProtocols(wm->disp, m->xwin, &protocols, &nprotocols) != 0) {
        for (int i = 0; i < nprotocols; i++) {
            if (protocols[i] == delete_atom)
                has_delete = true;
        }
        XFree(protocols);
    }

    if (has_delete) {
        XEvent msg;
        memset(&msg, 0, sizeof(msg));
        msg.xclient.type = ClientMessage;
        msg.xclient.window = m->xwin;
        msg.xclient.message_type = wm->ewmh.atoms[ATOM_WM_PROTOCOLS];
        msg.xclient.format = 32;
        msg.xclient.data.l[0] = (long)delete_atom;
        msg.xclient.data.l[1] = CurrentTime;
        XSendEvent(wm->disp, m->xwin, False, NoEventMask, &msg);
    } else {
        XKillClient(wm->disp, m->xwin);
    }
    XFlush(wm->disp);
}

// Every reparenting/tiling WM requests SubstructureRedirectMask on the
// root window, which means X routes every client-initiated move/resize
// through us as a ConfigureRequest instead of applying it directly --
// nothing happens to the real window unless we explicitly grant (or
// answer) the request. Missing this entirely (as opposed to granting it
// wrong) is what breaks apps like a GTK file chooser that resizes
// itself to fit an image preview pane: the resize is silently dropped
// by the X server, but GTK's own widget layout often proceeds as if it
// succeeded, so the dialog ends up internally inconsistent with its
// actual on-screen size -- exactly the "half-working, partially
// clipped" look. i3 hits this same case (src/handlers.c) and always
// answers one way or another rather than ignoring it outright.
static void on_configure_request(WM *wm, XConfigureRequestEvent *ev) {
    unsigned long id = (unsigned long)ev->window;
    ManagedWindow *m = find_window(wm, id);

    if (m == NULL) {
        // Not managed (yet) -- nothing of ours constrains it, so grant
        // exactly what was asked for, same as every other WM does for
        // an unmanaged/not-yet-mapped window.
        XWindowChanges changes;
        memset(&changes, 0, sizeof(changes));
        changes.x = ev->x;
        changes.y = ev->y;
        changes.width = ev->width;
        changes.height = ev->height;
        changes.border_width = ev->border_width;
        changes.sibling = ev->above;
        changes.stack_mode = ev->detail;
        XConfigureWindow(wm->disp, ev->window, (unsigned)ev->value_mask, &changes);
        return;
    }

    if (m->floating) {
        // Floating windows own their geometry -- honor whichever
        // fields the client actually asked to change (value_mask), not
        // all of x/y/width/height unconditionally.
        XWindowChanges changes;
        memset(&changes, 0, sizeof(changes));
        changes.x = ev->x;
        changes.y = ev->y;
        changes.width = ev->width;
        changes.height = ev->height;
        changes.border_width = ev->border_width;
        changes.sibling = ev->above;
        changes.stack_mode = ev->detail;
        XConfigureWindow(wm->disp, ev->window, (unsigned)ev->value_mask, &changes);

        // Keep our own bookkeeping in sync with whatever we just
        // granted, so a later unrelated apply_layout()/animation step
        // doesn't fight the size the client (and the server) now
        // actually have by animating it back toward a stale remembered
        // target.
        if (ev->value_mask & CWX) {
            m->cur_x = ev->x;
            m->target_x = ev->x;
        }
        if (ev->value_mask & CWY) {
            m->cur_y = ev->y;
            m->target_y = ev->y;
        }
        if (ev->value_mask & CWWidth) {
            m->cur_w = ev->width;
            m->target_w = ev->width;
        }
        if (ev->value_mask & CWHeight) {
            m->cur_h = ev->height;
            m->target_h = ev->height;
        }
        return;
    }

    // Tiled: the layout engine owns this window's geometry, not the
    // client, so the requested x/y/width/height are ignored -- but we
    // still answer with a synthetic ConfigureNotify carrying its real,
    // current geometry (matches i3's approach) instead of staying
    // silent, so a client that treats "no response" as "it worked"
    // doesn't end up internally inconsistent with what's actually on
    // screen.
    XConfigureEvent note;
    memset(&note, 0, sizeof(note));
    note.type = ConfigureNotify;
    note.event = m->xwin;
    note.window = m->xwin;
    note.x = m->cur_x;
    note.y = m->cur_y;
    note.width = m->cur_w;
    note.height = m->cur_h;
    note.border_width = BORDER_WIDTH;
    note.above = None;
    note.override_redirect = False;
    XSendEvent(wm->disp, m->xwin, False, StructureNotifyMask, (XEvent *)&note);
}

static void on_key_press(WM *wm, XKeyEvent *ev) {
    KeySym sym = XLookupKeysym(ev, 0);
    unsigned int state = (unsigned int)(ev->state & RELEVANT_MOD_MASK);

    const KeyBinding *b = NULL;
    for (int i = 0; i < wm->nkey_bindings; i++) {
        if (wm->key_bindings[i].keysym == sym && wm->key_bindings[i].mod == state) {
            b = &wm->key_bindings[i];
            break;
        }
    }
    if (b == NULL)
        return;

    switch (b->action) {
    case ACTION_FOCUS_NEXT:
        layout_focus_next(&wm->layout);
        break;
    case ACTION_FOCUS_PREV:
        layout_focus_prev(&wm->layout);
        break;
    case ACTION_MOVE_RIGHT:
        layout_move_focused_right(&wm->layout);
        break;
    case ACTION_MOVE_LEFT:
        layout_move_focused_left(&wm->layout);
        break;
    case ACTION_MAXIMIZE:
        layout_toggle_maximize_focused(&wm->layout);
        break;
    case ACTION_SHRINK:
        layout_resize_focused(&wm->layout, -80);
        break;
    case ACTION_GROW:
        layout_resize_focused(&wm->layout, 80);
        break;
    case ACTION_MOVE_TO_WORKSPACE:
        layout_move_focused_to_workspace(&wm->layout, b->arg);
        break;
    case ACTION_TERMINAL:
        wm_log(wm, "terminal keybind pressed");
        launch_terminal(wm);
        return; // nothing to relayout yet, MapRequest will trigger it
    case ACTION_LAUNCHER:
        wm_log(wm, "launcher keybind pressed");
        launch_launcher(wm);
        return;
    case ACTION_FULLSCREEN:
        toggle_fullscreen_focused(wm);
        return;
    case ACTION_KILL:
        kill_focused(wm);
        return;
    case ACTION_SCREENSHOT:
        wm_log(wm, "screenshot keybind pressed");
        launch_screenshot(wm, false);
        return;
    case ACTION_SCREENSHOT_REGION:
        wm_log(wm, "region screenshot keybind pressed");
        launch_screenshot(wm, true);
        return;
    case ACTION_LAYOUT_SWITCH:
        wm_log(wm, "layout switch keybind pressed");
        switch_layout(wm);
        return;
    case ACTION_QUIT:
        printf("Exiting.\n");
        exit(0);
    case ACTION_WORKSPACE:
        wm_log(wm, "workspace switch: %d -> %d requested",
            wm->layout.active_workspace, b->arg);
        switch_workspace_animated(wm, b->arg);
        wm_log(wm, "workspace now active=%d, columns on it=%d",
            wm->layout.active_workspace,
            wm->layout.workspaces[wm->layout.active_workspace].ncolumns);
        return; // already applied everything apply_layout() would
    }
    apply_layout(wm);
}

// ---- setup ----------------------------------------------------------

static int xerror_handler(Display *d, XErrorEvent *e) {
    char buf[256];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "[railwm] X error (non-fatal): %s\n", buf);
    return 0;
}

static bool g_became_wm_error = false;

// Returns false (and the caller errors) iff another WM is already
// running -- SubstructureRedirectMask can only be selected by one
// client at a time, and X reports that as a BadAccess error rather
// than a normal return value, so this has to go through the error
// handler.
static int became_wm_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadAccess)
        g_became_wm_error = true;
    return 0;
}

static bool become_wm(WM *wm) {
    XSetErrorHandler(became_wm_error_handler);
    XSelectInput(wm->disp, wm->root,
        SubstructureRedirectMask | SubstructureNotifyMask | KeyPressMask);
    XSync(wm->disp, False);
    XSetErrorHandler(xerror_handler);

    if (g_became_wm_error) {
        fprintf(stderr, "Another window manager is already running on this display.\n");
        return false;
    }
    return true;
}

static void grab_keys(WM *wm) {
    // Each binding carries its own full modifier mask rather than
    // everything implicitly assuming one MOD. Grab every combination
    // of Lock/Mod2 along with each real binding so Caps/Num Lock stay
    // transparent.
    const unsigned int lock_masks[4] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

    for (int i = 0; i < wm->nkey_bindings; i++) {
        KeyBinding *b = &wm->key_bindings[i];
        KeyCode code = XKeysymToKeycode(wm->disp, b->keysym);
        if (code == 0) {
            wm_log(wm, "binding %d: keysym has no keycode on this keyboard, skipping", i);
            continue;
        }
        for (int j = 0; j < 4; j++)
            XGrabKey(wm->disp, code, b->mod | lock_masks[j], wm->root, True,
                GrabModeAsync, GrabModeAsync);
    }
}

// Re-scans every override-redirect root child for the tallest
// _NET_WM_STRUT_PARTIAL "top" value and applies it as the layout's
// top_inset. Returns true if the reserved space changed (and layout was
// reapplied). Also arm PropertyChangeMask on each dock so later strut
// updates / destruction reach us.
static bool recompute_struts(WM *wm) {
    Window root_ret = 0;
    Window parent_ret = 0;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(wm->disp, wm->root, &root_ret, &parent_ret, &children, &nchildren) == 0)
        return false;

    int top = 0;
    Atom strut_atom = wm->ewmh.atoms[ATOM_NET_WM_STRUT_PARTIAL];
    // XA_CARDINAL is only a macro in Xatom.h in some setups, so intern
    // the predefined "CARDINAL" type by name -- it is the same atom.
    Atom cardinal_atom = XInternAtom(wm->disp, "CARDINAL", False);
    for (unsigned int i = 0; i < nchildren; i++) {
        Window win = children[i];
        if (win == wm->root)
            continue;
        XWindowAttributes attrs;
        if (XGetWindowAttributes(wm->disp, win, &attrs) == 0)
            continue;
        if (attrs.override_redirect == 0)
            continue;

        // So a strut that the bar changes later (or the bar's death)
        // triggers a property/destroy event on us.
        XSelectInput(wm->disp, win, PropertyChangeMask);

        Atom type_ret = 0;
        int format_ret = 0;
        unsigned long items_ret = 0;
        unsigned long after_ret = 0;
        unsigned char *prop = NULL;
        int got = XGetWindowProperty(wm->disp, win, strut_atom, 0, 12, False,
            cardinal_atom, &type_ret, &format_ret, &items_ret, &after_ret, &prop);
        if (got != Success || prop == NULL) {
            if (prop != NULL)
                XFree(prop);
            continue;
        }
        if (type_ret != cardinal_atom || format_ret != 32 || items_ret < 4) {
            XFree(prop);
            continue;
        }
        // Read as 4-byte words, not long-sized ones: Xlib returns
        // format-32 data in long units, but the original casts the
        // buffer to [*]const u32 and reads vals[2] from *that*. This
        // is not an accident to "fix" -- railbar writes its strut
        // through the same Xlib call with a u32[12] array, whose
        // pairs Xlib reads as longs, so the value intended for index
        // 2 ends up in long-slot 1 -- and this u32-slot-2 read picks
        // it back up. Reading longs here instead would silently drop
        // the strut.
        const uint32_t *vals = (const uint32_t *)prop;
        int t = (int)vals[2]; // strut layout: [l, r, top, b, ...]
        if (t > top)
            top = t;
        XFree(prop);
    }
    if (children != NULL)
        XFree(children);

    if (top == wm->strut_top)
        return false;
    wm->strut_top = top;
    wm->layout.top_inset = top;
    wm_log(wm, "dock strut: reserving top %dpx for tiled windows", top);
    apply_layout(wm);
    return true;
}

static void scan_existing_windows(WM *wm) {
    Window root_ret = 0;
    Window parent_ret = 0;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(wm->disp, wm->root, &root_ret, &parent_ret, &children, &nchildren) == 0)
        return;

    int adopted = 0;
    for (unsigned int i = 0; i < nchildren; i++) {
        XWindowAttributes attrs;
        if (XGetWindowAttributes(wm->disp, children[i], &attrs) == 0)
            continue;
        if (attrs.map_state != IsViewable)
            continue;
        if (attrs.override_redirect != 0)
            continue;

        unsigned long win_id = (unsigned long)children[i];
        bool floats = ewmh_should_float(&wm->ewmh, children[i]);
        add_managed(wm, win_id, children[i], floats);

        // The layout engine only ever looks at what's been added there
        // too (layout_add_window), so an adopted window must be hooked
        // in or the very next apply_layout would unmap it as "not in
        // the visible set".
        if (!floats) {
            layout_add_window(&wm->layout, win_id, false);
        } else if (wm->focused_floating == 0) {
            wm->focused_floating = win_id;
        }

        adopted += 1;
        printf("[railwm] Adopted existing window %lu (floating=%d)\n",
            win_id, floats ? 1 : 0);
    }
    if (children != NULL)
        XFree(children);
    if (adopted > 0) {
        printf("[railwm] Adopted %d existing window(s)\n", adopted);
        apply_layout(wm);
    }
}

static bool wm_init(WM *wm) {
    memset(wm, 0, sizeof(*wm));
    wm->inotify_fd = -1;
    wm->inotify_wd = -1;
    wm->layout_count = 1;

    wm->disp = XOpenDisplay(NULL);
    if (wm->disp == NULL) {
        fprintf(stderr, "railwm: cannot open display\n");
        return false;
    }

    wm->screen_num = DefaultScreen(wm->disp);
    wm->root = RootWindow(wm->disp, wm->screen_num);
    wm->screen_width = DisplayWidth(wm->disp, wm->screen_num);
    wm->screen_height = DisplayHeight(wm->disp, wm->screen_num);
    wm->colormap = DefaultColormap(wm->disp, wm->screen_num);

    load_config(wm);
    setup_config_watch(wm);

    {
        const char *home = getenv("HOME");
        if (home == NULL)
            home = "/tmp";
        if (snprintf(wm->runtime_log_path, sizeof(wm->runtime_log_path),
                "%s/.local/share/railwm-runtime.log", home) >= (int)sizeof(wm->runtime_log_path))
            return false;
        char dirbuf[512];
        if (snprintf(dirbuf, sizeof(dirbuf), "%s/.local/share", home) >= (int)sizeof(dirbuf))
            return false;
        mkdir(dirbuf, 0755);
    }

    if (!become_wm(wm))
        return false;

    // Without this, the root window keeps the X server's literal
    // default cursor (X_cursor) anywhere the pointer is over root
    // background. XcursorLibraryLoadCursor makes this theme-aware
    // (resolves "left_ptr" against XCURSOR_THEME); falls back to the
    // plain core-font arrow if no themed cursor can be found at all.
    Cursor root_cursor = XcursorLibraryLoadCursor(wm->disp, "left_ptr");
    if (root_cursor == 0)
        root_cursor = XCreateFontCursor(wm->disp, XC_left_ptr);
    XDefineCursor(wm->disp, wm->root, root_cursor);

    layout_init(&wm->layout, wm->screen_width, wm->screen_height);
    wm->layout.gap = wm->gap;
    wm->layout.default_column_width = wm->default_column_width;
    wm->layout.min_column_width = wm->min_column_width;

    // Dark blue background, actually forced to repaint -- this is fix
    // #1 from the Python version applied from the start: setting the
    // background attribute alone does not repaint anything, you need
    // XClearArea to force it, and exposures=True is what triggers it.
    XColor color, exact;
    XAllocNamedColor(wm->disp, wm->colormap, wm->bg_color_buf, &color, &exact);
    wm->bg_pixel = color.pixel;
    XSetWindowBackground(wm->disp, wm->root, wm->bg_pixel);
    XClearArea(wm->disp, wm->root, 0, 0, 0, 0, True);

    XColor fcolor, funused;
    XAllocNamedColor(wm->disp, wm->colormap, wm->focused_border_buf, &fcolor, &funused);
    wm->focused_border = fcolor.pixel;
    XColor ucolor, uunused;
    XAllocNamedColor(wm->disp, wm->colormap, wm->unfocused_border_buf, &ucolor, &uunused);
    wm->unfocused_border = ucolor.pixel;

    XFlush(wm->disp);

    ewmh_init(&wm->ewmh, wm->disp, wm->root);
    ewmh_announce_support(&wm->ewmh);

    // XKB must be requested before XkbGetState/XkbLockGroup are used
    // (layout switch keybind, and railbar's own active-layout read).
    int xkb_opcode = 0;
    int xkb_event = 0;
    int xkb_error = 0;
    int xkb_major = XkbMajorVersion;
    int xkb_minor = XkbMinorVersion;
    XkbQueryExtension(wm->disp, &xkb_opcode, &xkb_event, &xkb_error, &xkb_major, &xkb_minor);
    query_layout_count(wm);

    scan_existing_windows(wm);
    (void)recompute_struts(wm); // pick up a dock that was already running
    grab_keys(wm);

    resolve_terminal(wm);
    return true;
}

static void handle_event(WM *wm, XEvent *ev) {
    switch (ev->type) {
    case MapRequest:
        on_map_request(wm, &ev->xmaprequest);
        break;
    case ConfigureRequest:
        on_configure_request(wm, &ev->xconfigurerequest);
        break;
    case CreateNotify: {
        // A dock/bar may appear at any time (it's override-redirect,
        // so no MapRequest): recompute the reserved strip.
        (void)recompute_struts(wm);
        break;
    }
    case MapNotify: {
        // Same for the moment an override-redirect window actually
        // maps (its strut is normally set before the map).
        (void)recompute_struts(wm);
        break;
    }
    case DestroyNotify: {
        unsigned long id = (unsigned long)ev->xdestroywindow.window;
        wm_log(wm, "DestroyNotify for window %lu", id);
        forget_window(wm, id);
        // If it was the bar, its strut reservation dies with it.
        (void)recompute_struts(wm);
        break;
    }
    case UnmapNotify: {
        unsigned long id = (unsigned long)ev->xunmap.window;
        ManagedWindow *m = find_window(wm, id);
        if (m != NULL) {
            // Matches i3's approach: compare the event's serial (the
            // sequence number of the request that caused it) against
            // the exact serial we recorded when WE issued the unmap.
            // An exact match is unambiguous regardless of how many
            // other events happened in between -- the actual cause of
            // two symptoms that looked unrelated (windows "killed" by
            // opening more, and silent deletion across workspace
            // switches), both a self-caused unmap being misread as a
            // close.
            if (m->has_ignore_unmap && ev->xunmap.serial == m->ignore_unmap_serial) {
                m->has_ignore_unmap = false;
                wm_log(wm, "UnmapNotify for window %lu (self-inflicted, serial %lu matched)",
                    id, ev->xunmap.serial);
            } else {
                wm_log(wm, "UnmapNotify for window %lu (client-initiated, serial %lu)",
                    id, ev->xunmap.serial);
                forget_window(wm, id);
            }
        }
        break;
    }
    case KeyPress:
        on_key_press(wm, &ev->xkey);
        break;
    case ClientMessage:
        if (ev->xclient.message_type == wm->ewmh.atoms[ATOM_NET_ACTIVE_WINDOW]) {
            unsigned long target = (unsigned long)ev->xclient.window;
            ManagedWindow *m = find_window(wm, target);
            wm_log(wm, "_NET_ACTIVE_WINDOW request for window %lu%s", target,
                m != NULL ? "" : " (not managed, ignoring)");
            if (m != NULL) {
                if (m->floating) {
                    wm->focused_floating = target;
                    XRaiseWindow(wm->disp, ev->xclient.window);
                    XSetInputFocus(wm->disp, ev->xclient.window, RevertToParent, CurrentTime);
                }
                apply_layout(wm);
            }
        } else if (ev->xclient.message_type == wm->ewmh.atoms[ATOM_NET_WM_STATE]) {
            long action_v = ev->xclient.data.l[0]; // 0=remove 1=add 2=toggle
            Atom prop1 = (Atom)ev->xclient.data.l[1];
            Atom prop2 = (Atom)ev->xclient.data.l[2];
            if (prop1 == wm->ewmh.atoms[ATOM_NET_WM_STATE_FULLSCREEN] ||
                prop2 == wm->ewmh.atoms[ATOM_NET_WM_STATE_FULLSCREEN]) {
                unsigned long target = (unsigned long)ev->xclient.window;
                ManagedWindow *m = find_window(wm, target);
                wm_log(wm, "_NET_WM_STATE_FULLSCREEN request for window %lu, action=%ld%s",
                    target, action_v, m != NULL ? "" : " (not managed, ignoring)");
                if (m != NULL) {
                    bool want = action_v == 1 ? true : (action_v == 0 ? false : !m->fullscreen);
                    set_fullscreen(wm, m, want);
                }
            }
        }
        break;
    case PropertyNotify: {
        unsigned long id = (unsigned long)ev->xproperty.window;
        ManagedWindow *m = find_window(wm, id);
        // A dock that changes its strut while running renegotiates the
        // reserved strip. (Excludes managed windows -- the only
        // PropertyChangeMask we arm on strangers is for docks.)
        if (m == NULL && ev->xproperty.atom == wm->ewmh.atoms[ATOM_NET_WM_STRUT_PARTIAL]) {
            (void)recompute_struts(wm);
            return;
        }
        // Only tiled windows need rechecking -- a window already
        // floating has nothing to promote. This catches windows the
        // WM classified as tiled too early, before the app finished
        // setting its EWMH properties (e.g. a GTK file chooser when
        // MapRequest fired).
        if (m != NULL) {
            if (!m->floating && ewmh_should_float(&wm->ewmh, m->xwin)) {
                wm_log(wm, "window %lu now floating (property arrived after map -- promoting out of the tiled strip)", id);
                layout_remove_window(&wm->layout, id);
                m->floating = true;
                place_floating(wm, m->xwin);
                wm->focused_floating = id;
                XRaiseWindow(wm->disp, m->xwin);
                XSetInputFocus(wm->disp, m->xwin, RevertToParent, CurrentTime);
                apply_layout(wm);
            }
        }
        break;
    }
    default:
        break;
    }
}

// Read-only check: is anything still short of its target? Unlike
// step_animations(), this never calls XMoveResizeWindow, so it's safe
// to call every loop iteration to decide whether a deadline is needed,
// without that check itself causing the early-wakeup problem described
// in run() below.
static bool animation_pending(WM *wm) {
    for (int i = 0; i < wm->nwindows; i++) {
        ManagedWindow *m = &wm->windows[i];
        if (!m->has_rect)
            continue;
        if (m->cur_x != m->target_x || m->cur_y != m->target_y ||
            m->cur_w != m->target_w || m->cur_h != m->target_h)
            return true;
    }
    return false;
}

// Drains every pending inotify event in one pass (non-blocking fd, so
// a single read() can return several coalesced events) and reloads at
// most once even if multiple events fired for the same save -- an
// editor's write-then-rename pattern typically produces more than one
// event per save. Only a change to a file literally named "config"
// triggers a reload; anything else dropped into ~/.config/railwm (a
// stray swap file, a backup, an unrelated dotfile) is ignored.
static void drain_inotify_and_maybe_reload(WM *wm) {
    if (wm->inotify_fd < 0)
        return;

    char buf[4096];
    bool should_reload = false;

    for (;;) {
        ssize_t n = read(wm->inotify_fd, buf, sizeof(buf));
        if (n <= 0)
            break; // EAGAIN (nothing left, non-blocking fd) or error

        // Fixed struct inotify_event header is 16 bytes: wd, mask,
        // cookie, len (each 4 bytes), followed by `len` bytes of
        // NUL-padded filename. The Zig original parsed this by hand
        // rather than via cImport's translation of the flexible array
        // member; in C the system struct is used directly.
        for (ssize_t offset = 0; offset < (ssize_t)n;) {
            struct inotify_event *iev = (struct inotify_event *)&buf[offset];
            if (iev->len > 0 && strcmp(iev->name, "config") == 0)
                should_reload = true;
            offset += (ssize_t)sizeof(struct inotify_event) + (ssize_t)iev->len;
        }
    }

    if (should_reload)
        reload_config(wm);
}

static void run(WM *wm) {
    printf("railwm running. Screen: %dx%d\n", wm->screen_width, wm->screen_height);
    printf("Managed windows: %d\n", wm->nwindows);
    printf("Alt+t for a terminal, Alt+f to maximize, Mod4+1-9 for workspaces, Alt+Shift+e to quit.\n");

    // X has no frame-callback signal the way a Wayland compositor does
    // -- XNextEvent() blocks forever until something happens. To
    // animate, this waits for either a real X event OR a short timeout,
    // whichever comes first, using poll() on the X connection's own fd.
    //
    // BUG FIXED (same as the C version): the original stepped
    // animations once per loop iteration, trusting a short timeout to
    // pace it at ~60fps. But every XMoveResizeWindow generates a
    // ConfigureNotify -- caught by our own SubstructureNotifyMask --
    // which makes the connection readable again almost immediately,
    // waking the wait long before the timeout expired, so the whole
    // motion completed in a handful of milliseconds regardless of
    // ANIM_STEP_DIVISOR. Fix: track an explicit wall-clock deadline
    // (CLOCK_MONOTONIC) and only actually step when that deadline has
    // passed. Spurious early wakeups still happen, but they just
    // re-enter poll() with whatever time is actually left instead of
    // stepping again immediately. Idle, nothing animating, this still
    // blocks indefinitely.
    int xfd = XConnectionNumber(wm->disp);
    bool have_deadline = false;
    struct timespec next_tick;

    // wm->inotify_fd is -1 when setup_config_watch() couldn't set up a
    // watch (no HOME, no ~/.config/railwm yet, or inotify_init1 itself
    // failed) -- poll(2) treats a negative fd as "ignore this entry,
    // always report revents=0", so this slot is safe to leave in the
    // array unconditionally rather than branching the whole array size
    // on whether the watch exists.

    for (;;) {
        while (XPending(wm->disp) != 0) {
            XEvent ev;
            XNextEvent(wm->disp, &ev);
            handle_event(wm, &ev);
        }

        struct timespec now = monotonic_now();

        if (!have_deadline && animation_pending(wm)) {
            // Something just started animating -- step it right away on
            // this pass rather than waiting a full tick for the first
            // frame.
            next_tick = now;
            have_deadline = true;
        }

        int timeout_ms = -1; // -1 = block indefinitely (idle)
        if (have_deadline) {
            long long remaining_ms = (next_tick.tv_sec - now.tv_sec) * 1000 +
                (next_tick.tv_nsec - now.tv_nsec) / 1000000;
            if (remaining_ms <= 0) {
                (void)step_animations(wm);
                if (animation_pending(wm)) {
                    next_tick = now;
                    next_tick.tv_nsec += (long)ANIM_TICK_MS * 1000000L;
                    if (next_tick.tv_nsec >= 1000000000L) {
                        next_tick.tv_sec += 1;
                        next_tick.tv_nsec -= 1000000000L;
                    }
                    timeout_ms = ANIM_TICK_MS;
                } else {
                    have_deadline = false;
                    timeout_ms = -1;
                }
            } else {
                timeout_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
            }
        }

        struct pollfd pollfds[2];
        pollfds[0].fd = xfd;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;
        pollfds[1].fd = wm->inotify_fd;
        pollfds[1].events = POLLIN;
        pollfds[1].revents = 0;
        poll(pollfds, 2, timeout_ms);

        if (pollfds[1].revents & POLLIN)
            drain_inotify_and_maybe_reload(wm);

        reap_children(wm);
    }
}

int main(void) {
    WM wm;
    if (!wm_init(&wm))
        return 1;
    run(&wm);
    return 0;
}
