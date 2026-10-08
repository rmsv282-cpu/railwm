// railbar -- a minimal status bar for railwm.
//
// A full-width override-redirect strip pinned to the top of the screen.
// Without arguments it shows the current date and time on the left;
// pass a shell command as ARGV[1] and it runs that command once a
// second and shows its output instead.
//
// On the right, four always-on modules are drawn in this order
// (left to right): ethernet status, keyboard layout, volume, then a
// system tray that embeds any app's tray icon via the freedesktop
// XEMBED tray protocol.
//
// No IPC, no config file, no dependency on railwm itself beyond sharing
// its color palette -- run it standalone or from any WM's startup file.

// Opt back into the POSIX/GNU interfaces this file uses (dirent's
// d_type, popen/pclose, usleep) that a strict -std=c11 compile would
// otherwise hide behind __STRICT_ANSI__. The Zig original picked them
// up implicitly through its @cImport block.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define BAR_HEIGHT 26
#define MAX_STATUS 2048
#define REFRESH_MS 1000 // status is re-queried every second

// Real, visible margin between the bar and the screen's left/right
// edges -- the bar's own X window is inset by this much and made
// narrower, the same way tiled windows leave empty screen behind
// their edges in layout.c, rather than just changing where text
// starts drawing inside a window that still spans full-width. Matches
// layout.c's window `gap` so it reads as the same visual gap.
#define WINDOW_GAP 16

// Padding from the bar's own left/right edges to where its text/tray
// actually start, on top of WINDOW_GAP above. Zero -- WINDOW_GAP
// alone already provides the visible margin; adding both would just
// double the gap.
#define EDGE_GAP 0

// Width of the vertical accent-color stripe drawn down both of the
// bar's edges. The status text (left) and module cluster/tray (right)
// are offset past this by RIGHT_MARGIN/left_text_x so neither paints
// under a stripe. Left as a plain integer constant (like BAR_HEIGHT)
// so it converts straight into both cairo's double params and the int
// layout arithmetic below without an explicit cast either way.
#define ACCENT_STRIPE_W 2

// Clearance from the bar's real right edge to where the right-side
// module cluster (tray/volume/kbd/eth) starts, mirroring left_text_x
// on the other side.
#define RIGHT_MARGIN (ACCENT_STRIPE_W + 4)

#define STRUT_PARTIAL_LEN 12

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Color;

static void color_set_source(Color self, cairo_t *cr) {
    cairo_set_source_rgb(cr,
        (double)self.r / 255.0,
        (double)self.g / 255.0,
        (double)self.b / 255.0);
}

// Same palette as railwm's own defaults (see wm.c).
static const Color BG = { .r = 0x0a, .g = 0x14, .b = 0x28 };
static const Color FG = { .r = 0xe8, .g = 0xe8, .b = 0xe0 };
static const Color ACCENT = { .r = 0xff, .g = 0xbf, .b = 0x00 };
static const Color DIM = { .r = 0x4d, .g = 0x38, .b = 0x00 };
static const Color RED = { .r = 0xc0, .g = 0x40, .b = 0x40 };

// The C stand-in for a Zig `[]const u8` slice -- a borrowed pointer
// plus a length -- so buffers that aren't NUL-terminated (status_buf,
// layout names, sysfs reads) can still be passed around as data. The
// Zig original kept a cStr() helper here to turn a fixed/null-
// terminated C char buffer into a slice without going through
// std.mem.sliceTo's generic sentinel-type inference (same trick as
// raillauncher); a C char buffer already is a NUL-terminated string,
// so slices are assembled directly with mk_slice() instead.
typedef struct {
    const char *ptr;
    size_t len;
} Slice;

static Slice mk_slice(const char *ptr, size_t len) {
    Slice s = { ptr, len };
    return s;
}

// std.mem.indexOf equivalent: stores the offset of the first
// occurrence of `needle` inside `hay` and returns false when it isn't
// there -- the shape Zig's `indexOf(...) orelse return st` maps onto.
static bool mem_index_of(const char *hay, size_t hay_len, const char *needle, size_t *out) {
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        *out = 0;
        return true;
    }
    if (nlen > hay_len)
        return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool char_in_set(const char *set, char c) {
    for (const char *p = set; *p != '\0'; p++) {
        if (*p == c)
            return true;
    }
    return false;
}

// std.mem.trimStart equivalent: drop every character of `set` off the
// front of the buffer.
static Slice mem_trim_start(Slice s, const char *set) {
    size_t i = 0;
    while (i < s.len && char_in_set(set, s.ptr[i]))
        i++;
    return mk_slice(s.ptr + i, s.len - i);
}

// std.mem.trim equivalent: drop every character of `set` off both
// ends of the buffer.
static Slice mem_trim(Slice s, const char *set) {
    s = mem_trim_start(s, set);
    size_t end = s.len;
    while (end > 0 && char_in_set(set, s.ptr[end - 1]))
        end--;
    return mk_slice(s.ptr, end);
}

// std.mem.startsWith equivalent against a NUL-terminated prefix.
static bool slice_starts_with(Slice s, const char *prefix) {
    size_t plen = strlen(prefix);
    return s.len >= plen && memcmp(s.ptr, prefix, plen) == 0;
}

// std.mem.eql equivalent over two slices.
static bool slice_eql(Slice a, Slice b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

// std.mem.eql equivalent against a NUL-terminated literal.
static bool slice_eql_str(Slice a, const char *b) {
    size_t blen = strlen(b);
    return a.len == blen && memcmp(a.ptr, b, blen) == 0;
}

// std.fmt.parseFloat equivalent: `out` gets the parsed value only if
// `strtod` consumed the whole buffer (Zig's parseFloat rejects a bare
// "." or trailing junk the same way).
static bool parse_float(const char *s, size_t len, double *out) {
    if (len == 0 || len >= 64)
        return false;
    char buf[64];
    memcpy(buf, s, len);
    buf[len] = '\0';
    // Zig's parseFloat rejects a trailing '.' outright ("0.45." is an
    // error there) while strtod happily consumes it -- match Zig.
    if (buf[len - 1] == '.')
        return false;
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp != buf + len)
        return false;
    *out = v;
    return true;
}

// Runs `cmd` through `/bin/sh -c`, writes its first `out_len - 1`
// bytes into `out`, and returns how many bytes were written (trailing
// newline stripped). A command whose popen() fails yields an empty
// string.
static size_t run_command(char *out, size_t out_len, const char *cmd) {
    FILE *fh = popen(cmd, "r");
    if (fh == NULL)
        return 0;

    size_t total = 0;
    while (total < out_len) {
        size_t n = fread(&out[total], 1, out_len - total, fh);
        if (n == 0)
            break;
        total += n;
    }
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
        total -= 1;
    (void)pclose(fh);
    return total;
}

// Default status when no command was given: the local date and time.
static size_t get_clock(char *out, size_t out_len) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm == NULL)
        return 0;
    return strftime(out, out_len, "%a %b %d  %H:%M:%S", tm);
}

// ---- module: volume --------------------------------------------------
// Shells out to `wpctl` (WirePlumber's CLI, ships with WirePlumber
// itself -- no separate package) once a second. `wpctl get-volume
// @DEFAULT_AUDIO_SINK@` prints a line like "Volume: 0.45" or
// "Volume: 0.45 [MUTED]"; we parse the fraction and scale to a
// percent. Volume can run above 1.0 if the sink allows boosted gain,
// so this isn't clamped to 100 the way a raw ALSA percent would be.

typedef struct {
    uint16_t percent;
    bool muted;
    bool ok;
} VolState;

static VolState query_volume(char *scratch, size_t scratch_len) {
    VolState st = { 0, false, false };
    size_t n = run_command(scratch, scratch_len,
        "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    if (n == 0)
        return st;
    const char *text = scratch;
    size_t text_len = n;

    const char *marker = "Volume:";
    size_t start;
    if (!mem_index_of(text, text_len, marker, &start))
        return st;
    Slice rest = mem_trim_start(
        mk_slice(text + start + strlen(marker), text_len - start - strlen(marker)), " \t");

    // Grab the leading "0.45"-shaped run of digits/dot.
    size_t end = 0;
    while (end < rest.len && ((rest.ptr[end] >= '0' && rest.ptr[end] <= '9') || rest.ptr[end] == '.'))
        end += 1;
    if (end == 0)
        return st;
    double frac;
    if (!parse_float(rest.ptr, end, &frac))
        return st;

    st.ok = true;
    size_t muted_at;
    st.muted = mem_index_of(text, text_len, "[MUTED]", &muted_at);
    st.percent = (uint16_t)(long)round(fmax(frac, 0.0) * 100.0);
    return st;
}

// Nudges the default sink's volume up or down 2% via wpctl (same tool
// query_volume reads back from). The trailing "&" backgrounds the real
// wpctl call inside the shell popen() spawns, so pclose() returns as
// soon as the shell has forked it off instead of blocking until wpctl
// itself exits -- that wait (plus the second one query_volume used to
// do right after) was the actual cause of scroll feeling laggy, not
// wpctl's own speed. Output is discarded either way.
static void adjust_volume(bool up) {
    const char *cmd = up
        ? "wpctl set-volume @DEFAULT_AUDIO_SINK@ 2%+ >/dev/null 2>&1 &"
        : "wpctl set-volume @DEFAULT_AUDIO_SINK@ 2%- >/dev/null 2>&1 &";
    char discard[64];
    (void)run_command(discard, sizeof(discard), cmd);
}

// Optimistic local update of the displayed percent, used right after
// a scroll event instead of shelling back out to `wpctl get-volume`
// to confirm it -- that round trip was the other half of the scroll
// lag. The periodic once-a-second query_volume() call in the main loop
// still re-syncs against the real value shortly after, so this can
// only ever be visibly wrong for under a second, and only if wpctl's
// actual step size ever disagrees with the 2% assumed here.
static void nudge_volume_locally(VolState *st, bool up) {
    if (!st->ok)
        return;
    int delta = up ? 2 : -2;
    int new_val = (int)st->percent + delta;
    int clamped = new_val < 0 ? 0 : (new_val > 999 ? 999 : new_val);
    st->percent = (uint16_t)clamped;
    if (up)
        st->muted = false;
}

// A tiny 4-bar "speaker" glyph drawn with rectangles instead of a font
// icon (keeps this dependency-free -- no icon font, no emoji fallback
// guesswork through fontconfig). Bars fill left-to-right by level;
// muted draws all bars dim and adds a diagonal strike.
static void draw_volume_icon(cairo_t *cr, double x, double y_mid, VolState st) {
    const double bar_w = 3.0;
    const double gap = 2.0;
    const double heights[4] = { 4, 7, 10, 13 };
    unsigned filled_buckets = (unsigned)st.percent / 25 + (st.percent > 0 ? 1u : 0u);
    if (filled_buckets > 4)
        filled_buckets = 4;
    const uint8_t filled = st.muted ? 0 : (uint8_t)filled_buckets;
    // (percent can exceed 100 with boosted gain; the /25 bucket count is
    // clamped to 4 above regardless, so the icon just reads "maxed".)

    double bx = x;
    for (int idx = 0; idx < 4; idx++) {
        double h = heights[idx];
        bool on = idx < (int)filled;
        if (on)
            color_set_source(ACCENT, cr);
        else
            color_set_source(DIM, cr);
        cairo_rectangle(cr, bx, y_mid - h / 2, bar_w, h);
        cairo_fill(cr);
        bx += bar_w + gap;
    }
    if (st.muted) {
        color_set_source(RED, cr);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x, y_mid - 7);
        cairo_line_to(cr, bx - gap, y_mid + 7);
        cairo_stroke(cr);
    }
}

// ---- module: keyboard layout ------------------------------------------
// The list of configured layouts ("us,de,...") is queried once at
// startup via `setxkbmap -query` since that list is effectively static
// for a session. Which one is *active* changes at runtime, so that's
// read every tick straight from the XKB extension (XkbGetState) --
// no shelling out on the hot path.

#define MAX_LAYOUTS 8

typedef struct {
    char names[MAX_LAYOUTS][8];
    uint8_t lens[MAX_LAYOUTS];
    uint8_t count;
} LayoutList;

static LayoutList query_layout_list(char *scratch, size_t scratch_len) {
    LayoutList ll;
    memset(&ll, 0, sizeof(ll)); // Zig's `LayoutList{}` -- the `undefined` rows are only read up to `lens[i]`
    size_t n = run_command(scratch, scratch_len,
        "setxkbmap -query 2>/dev/null | awk -F': ' '/^layout/ {print $2}'");
    if (n == 0) {
        memset(ll.names[0], 0, 8);
        memcpy(ll.names[0], "??", 2);
        ll.lens[0] = 2;
        ll.count = 1;
        return ll;
    }
    size_t pos = 0;
    for (;;) {
        if (ll.count >= MAX_LAYOUTS)
            break;
        size_t comma = pos;
        while (comma < n && scratch[comma] != ',')
            comma += 1;
        Slice part = mem_trim(mk_slice(scratch + pos, comma - pos), " \t");
        size_t len = part.len < 8 ? part.len : 8;
        memcpy(ll.names[ll.count], part.ptr, len);
        ll.lens[ll.count] = (uint8_t)len;
        ll.count += 1;
        if (comma >= n)
            break;
        pos = comma + 1;
    }
    if (ll.count == 0) {
        memcpy(ll.names[0], "??", 2);
        ll.lens[0] = 2;
        ll.count = 1;
    }
    return ll;
}

static Slice current_layout_name(Display *disp, const LayoutList *ll) {
    XkbStateRec state;
    if (XkbGetState(disp, XkbUseCoreKbd, &state) != 0)
        return mk_slice("??", 2);
    // count -| 1 in Zig (saturating) -- so 0 layouts still lands on
    // group 0, which the ll.count == 0 check just below turns into "??"
    unsigned sub = ll->count > 0 ? (unsigned)ll->count - 1u : 0u;
    unsigned group = state.group;
    if (group > sub)
        group = sub;
    if (ll->count == 0)
        return mk_slice("??", 2);
    return mk_slice(ll->names[group], ll->lens[group]);
}

// ---- module: ethernet ---------------------------------------------
// No netlink socket, just /sys/class/net -- read once a second: pick
// the first interface that (a) isn't loopback, (b) isn't wifi (name
// starting with "wl", the systemd predictable-naming convention, or
// the older "wlan"/"wwan"), and (c) reports ARPHRD_ETHER (type "1")
// in .../type. Report its operstate.

typedef enum {
    ETH_CONNECTED,
    ETH_DISCONNECTED,
    ETH_NONE,
} EthState;

// std.Io.Dir.cwd().readFileAlloc(..., .limited(16)) equivalent, minus
// the arena: read the whole file into `buf` and fail the way Zig's
// `.limited(16)` did when the file is longer than 16 bytes.
static bool read_sysfs_file(const char *path, char *buf, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    size_t n = fread(buf, 1, 16, f);
    bool too_long = (n == 16 && fgetc(f) != EOF);
    (void)fclose(f);
    if (too_long)
        return false;
    *len_out = n;
    return true;
}

static EthState query_ethernet(void) {
    DIR *dir = opendir("/sys/class/net");
    if (dir == NULL)
        return ETH_NONE;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_LNK && entry->d_type != DT_DIR)
            continue;
        const char *name = entry->d_name;
        if (strcmp(name, "lo") == 0)
            continue;
        if (strncmp(name, "wl", 2) == 0)
            continue;
        if (strncmp(name, "ww", 2) == 0)
            continue;
        if (strncmp(name, "docker", 6) == 0)
            continue;
        if (strncmp(name, "veth", 4) == 0)
            continue;
        if (strncmp(name, "br", 2) == 0)
            continue;
        if (strncmp(name, "tun", 3) == 0)
            continue;
        if (strncmp(name, "vir", 3) == 0)
            continue;

        char type_path[256];
        if (snprintf(type_path, sizeof(type_path), "/sys/class/net/%s/type", name)
                >= (int)sizeof(type_path))
            continue;
        char type_str[16];
        size_t type_len;
        if (!read_sysfs_file(type_path, type_str, &type_len))
            continue;
        bool is_ether = slice_starts_with(mem_trim(mk_slice(type_str, type_len), " \n"), "1");
        if (!is_ether)
            continue;

        char op_path[256];
        if (snprintf(op_path, sizeof(op_path), "/sys/class/net/%s/operstate", name)
                >= (int)sizeof(op_path))
            continue;
        char op_str[16];
        size_t op_len;
        if (!read_sysfs_file(op_path, op_str, &op_len))
            continue;
        Slice state = mem_trim(mk_slice(op_str, op_len), " \n");
        EthState result = slice_eql_str(state, "up") ? ETH_CONNECTED : ETH_DISCONNECTED;
        (void)closedir(dir);
        return result;
    }
    (void)closedir(dir);
    return ETH_NONE;
}

static void draw_eth_icon(cairo_t *cr, double x, double y_mid, EthState st) {
    // A little RJ45-ish plug: a body rectangle plus two prongs. Color
    // carries the actual meaning; the shape just says "this is wired
    // networking" at a glance.
    Color col;
    switch (st) {
    case ETH_CONNECTED:
        col = ACCENT;
        break;
    case ETH_DISCONNECTED:
        col = RED;
        break;
    case ETH_NONE:
    default:
        col = DIM;
        break;
    }
    color_set_source(col, cr);
    cairo_rectangle(cr, x, y_mid - 5, 10, 8);
    cairo_fill(cr);
    cairo_rectangle(cr, x + 2, y_mid + 3, 2, 3);
    cairo_fill(cr);
    cairo_rectangle(cr, x + 6, y_mid + 3, 2, 3);
    cairo_fill(cr);
}

// ---- module: system tray (XEMBED) --------------------------------
// Implements the freedesktop.org "System Tray Protocol": we take
// ownership of the _NET_SYSTEM_TRAY_S<screen> selection and announce
// it via a MANAGER ClientMessage on the root window. Tray apps then
// send us a SYSTEM_TRAY_REQUEST_DOCK ClientMessage containing their
// icon window; we reparent that window into our bar, resize it to a
// fixed square, tell it (via _XEMBED) that it's embedded, and map it.
//
// Icons are laid out right-to-left starting from the bar's right
// edge, newest dock request appended at the current rightmost slot.

#define MAX_TRAY_ICONS 16
#define TRAY_ICON_SIZE 20
#define TRAY_ICON_GAP 4

typedef struct {
    Window win;
} TrayIcon;

// Zig's Tray struct; its methods become the tray_*() functions below
// with the struct pointer as their first argument.
typedef struct {
    TrayIcon icons[MAX_TRAY_ICONS];
    size_t count;
    Atom atom_selection;
    Atom atom_opcode;
    Atom atom_xembed;
    Atom atom_manager;
    bool have_selection;
} Tray;

static void tray_init(Tray *self, Display *disp, int screen, Window bar_win, Window root) {
    char name_buf[32];
    const char *sel_name;
    if (snprintf(name_buf, sizeof(name_buf), "_NET_SYSTEM_TRAY_S%d", screen) >= (int)sizeof(name_buf))
        sel_name = "_NET_SYSTEM_TRAY_S0";
    else
        sel_name = name_buf;
    self->atom_selection = XInternAtom(disp, sel_name, False);
    self->atom_opcode = XInternAtom(disp, "_NET_SYSTEM_TRAY_OPCODE", False);
    self->atom_xembed = XInternAtom(disp, "_XEMBED", False);
    self->atom_manager = XInternAtom(disp, "MANAGER", False);

    Window owner = XGetSelectionOwner(disp, self->atom_selection);
    if (owner != None) {
        // Someone else (another bar, a DE panel) already owns the
        // tray. Don't fight over it -- just skip tray duty.
        self->have_selection = false;
        return;
    }

    (void)XSetSelectionOwner(disp, self->atom_selection, bar_win, CurrentTime);
    if (XGetSelectionOwner(disp, self->atom_selection) != bar_win) {
        self->have_selection = false;
        return;
    }
    self->have_selection = true;

    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = root;
    ev.message_type = self->atom_manager;
    ev.format = 32;
    ev.data.l[0] = (long)CurrentTime;
    ev.data.l[1] = (long)self->atom_selection;
    ev.data.l[2] = (long)bar_win;
    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.xclient = ev;
    (void)XSendEvent(disp, root, False, StructureNotifyMask, &xev);
}

static void tray_handle_dock_request(Tray *self, Display *disp, Window bar_win, Window icon_win) {
    if (!self->have_selection)
        return;
    if (self->count >= MAX_TRAY_ICONS)
        return;
    if (icon_win == None)
        return;

    (void)XReparentWindow(disp, icon_win, bar_win, 0, 0);
    (void)XResizeWindow(disp, icon_win, TRAY_ICON_SIZE, TRAY_ICON_SIZE);
    (void)XSelectInput(disp, icon_win, StructureNotifyMask | PropertyChangeMask);

    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = icon_win;
    ev.message_type = self->atom_xembed;
    ev.format = 32;
    ev.data.l[0] = (long)CurrentTime;
    ev.data.l[1] = 0; // XEMBED_EMBEDDED_NOTIFY
    ev.data.l[2] = 0;
    ev.data.l[3] = (long)bar_win;
    ev.data.l[4] = 0;
    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.xclient = ev;
    (void)XSendEvent(disp, icon_win, False, NoEventMask, &xev);

    (void)XMapWindow(disp, icon_win);
    self->icons[self->count].win = icon_win;
    self->count += 1;
}

static bool tray_remove_if_present(Tray *self, Window win) {
    size_t i = 0;
    while (i < self->count) {
        if (self->icons[i].win == win) {
            size_t j = i;
            while (j + 1 < self->count) {
                self->icons[j] = self->icons[j + 1];
                j += 1;
            }
            self->count -= 1;
            return true;
        }
        i += 1;
    }
    return false;
}

// Right edge of the tray cluster (icons occupy the space to the
// right of this point up to the bar's right margin) and left edge
// (where the next module to the left should stop).
static int tray_layout(const Tray *self, Display *disp, int right_margin) {
    int x = right_margin;
    size_t i = self->count;
    while (i > 0) {
        i -= 1;
        x -= TRAY_ICON_SIZE;
        (void)XMoveWindow(disp, self->icons[i].win, x, (BAR_HEIGHT - TRAY_ICON_SIZE) / 2);
        x -= TRAY_ICON_GAP;
    }
    return x;
}

// ---- drawing ---------------------------------------------------------

// Screen-space x-range of the volume module's icon+text, in the most
// recent draw() call. Padded a few px on each side for easier scroll
// targeting. Updated every frame since the module's width changes
// with the percent text ("5%" vs "100%") and the bar's own width.
static int vol_hit_x0 = 0;
static int vol_hit_x1 = 0;

static int text_width_approx(Slice s) {
    // Matches the 8px/char monospace assumption already used for the
    // left-side status truncation below.
    return (int)s.len * 8;
}

static void draw(cairo_t *cr, int width, Slice status, EthState eth, Slice kbd,
    VolState vol, int tray_left_edge) {
    color_set_source(BG, cr);
    cairo_paint(cr);

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);

    // Left: clock or custom status command, truncated so it never
    // runs into the right-side module cluster. Starts past
    // ACCENT_STRIPE_W + a small buffer so the text never sits under
    // the accent stripe drawn later at the bar's left edge (they used
    // to overlap when this started at EDGE_GAP=0, same x as the stripe).
    color_set_source(FG, cr);
    const int left_text_x = ACCENT_STRIPE_W + 4;
    int max_left_w = tray_left_edge - EDGE_GAP - left_text_x - 2;
    if (max_left_w < 0)
        max_left_w = 0;
    size_t max_chars = (size_t)(max_left_w / 8);
    size_t shown = status.len < max_chars ? status.len : max_chars;
    char sbuf[MAX_STATUS + 1];
    memcpy(sbuf, status.ptr, shown);
    sbuf[shown] = '\0';
    cairo_move_to(cr, left_text_x, BAR_HEIGHT / 2 + 5);
    cairo_show_text(cr, sbuf);

    // Right cluster, laid out right-to-left: [tray] [volume] [kbd] [eth]
    double x = (double)(tray_left_edge - 10);
    const double y_mid = BAR_HEIGHT / 2;

    // Volume: 4-bar icon + "NN%" text, icon on the left of its text.
    const double vol_right = x; // right edge of the volume module (before text)
    char volbuf[8];
    const char *voltxt;
    if (vol.ok) {
        if (snprintf(volbuf, sizeof(volbuf), "%u%%", (unsigned)vol.percent) >= (int)sizeof(volbuf))
            voltxt = "--";
        else
            voltxt = volbuf;
    } else {
        voltxt = "no wpctl";
    }
    x -= (double)text_width_approx(mk_slice(voltxt, strlen(voltxt)));
    color_set_source(FG, cr);
    cairo_move_to(cr, x, BAR_HEIGHT / 2 + 5);
    cairo_show_text(cr, voltxt);
    x -= 20;
    draw_volume_icon(cr, x, y_mid, vol);
    vol_hit_x0 = (int)x - 4;
    vol_hit_x1 = (int)vol_right + 4;
    x -= 14; // gap before next module

    // Keyboard layout: plain 2-3 letter code.
    char kbdbuf[10];
    size_t klen = kbd.len < 9 ? kbd.len : 9;
    memcpy(kbdbuf, kbd.ptr, klen);
    kbdbuf[klen] = '\0';
    x -= (double)text_width_approx(mk_slice(kbd.ptr, klen));
    color_set_source(FG, cr);
    cairo_move_to(cr, x, BAR_HEIGHT / 2 + 5);
    cairo_show_text(cr, kbdbuf);
    x -= 18;

    // Ethernet: small plug glyph, color-coded.
    draw_eth_icon(cr, x - 10, y_mid, eth);

    // Frame accents (unchanged from the original bar).
    color_set_source(DIM, cr);
    cairo_rectangle(cr, 0, BAR_HEIGHT - 1, width, 1);
    cairo_fill(cr);
    color_set_source(ACCENT, cr);
    cairo_rectangle(cr, 0, 0, ACCENT_STRIPE_W, BAR_HEIGHT);
    cairo_fill(cr);
    // Matching stripe on the right edge -- width is the bar's own
    // (already WINDOW_GAP-inset) drawable width, so this sits right at
    // the bar's real right edge, not the display's.
    cairo_rectangle(cr, (double)(width - ACCENT_STRIPE_W), 0, ACCENT_STRIPE_W, BAR_HEIGHT);
    cairo_fill(cr);

    cairo_surface_flush(cairo_get_target(cr));
}

int main(int argc, char **argv) {
    // The Zig version took a std.process.Init and read its command
    // from init.minimal.args.vector[1]; argv[1] is the same slot here.
    // That version duped the argument into an arena -- needless in C,
    // since argv strings already outlive this never-returning main
    // loop.
    const char *command = NULL;
    bool have_command = false;
    if (argc > 1) {
        command = argv[1];
        have_command = true;
    }

    Display *disp = XOpenDisplay(NULL);
    if (disp == NULL) {
        fprintf(stderr, "railbar: cannot open X display\n");
        return 0; // the Zig original returns (successfully) from main on this path too
    }

    // XKB must be requested before we start calling XkbGetState.
    int xkb_opcode = 0;
    int xkb_event = 0;
    int xkb_error = 0;
    int xkb_major = XkbMajorVersion;
    int xkb_minor = XkbMinorVersion;
    (void)XkbQueryExtension(disp, &xkb_opcode, &xkb_event, &xkb_error, &xkb_major, &xkb_minor);

    int screen = XDefaultScreen(disp);
    Window root = XRootWindow(disp, screen);
    int screen_w = XDisplayWidth(disp, screen);

    // The bar's own window is inset by WINDOW_GAP and narrower than
    // the display by 2*WINDOW_GAP, so there's real empty root-window
    // background visible on each side -- an actual gap, not just
    // internal drawing padding. bar_w is what all the drawing code
    // below treats as "the bar's width"; screen_w (the true display
    // width) is only needed for placing the window and the strut.
    int bar_x = WINDOW_GAP;
    int bar_w = screen_w - 2 * WINDOW_GAP;

    // override_redirect so railwm never tiles or hides it.
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask;

    Window win = XCreateWindow(disp, root, bar_x, 0,
        (unsigned)bar_w, (unsigned)BAR_HEIGHT, 0,
        CopyFromParent, InputOutput, NULL,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    // Advertise a top strut so railwm (or any WM honoring
    // _NET_WM_STRUT_PARTIAL) keeps tiled windows below the bar --
    // set BEFORE mapping so the WM sees it the moment we appear.
    // top_start_x/top_end_x are real screen coordinates, so they match
    // the bar's actual (inset) horizontal extent, not the full display
    // width -- windows can use the WINDOW_GAP slivers on each side of
    // the bar, same as any other gap in the layout.
    Atom strut_atom = XInternAtom(disp, "_NET_WM_STRUT_PARTIAL", False);
    Atom dock_atom = XInternAtom(disp, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Atom wtype_atom = XInternAtom(disp, "_NET_WM_WINDOW_TYPE", False);
    // "CARDINAL"/"ATOM" are predefined property types (XA_CARDINAL /
    // XA_ATOM); Xatom.h only ships them as macros, so look them up by
    // name the same way everything else gets interned.
    Atom cardinal_atom = XInternAtom(disp, "CARDINAL", False);
    Atom atom_type = XInternAtom(disp, "ATOM", False);
    // Values are 32-bit CARDINAL words (format 32); the item types must
    // be uint32_t or the server re-reads them as 4-byte words and the layout
    // shifts (top would land on index 4 instead of 2).
    uint32_t strut[STRUT_PARTIAL_LEN] = {
        0, 0, BAR_HEIGHT, 0, // left, right, top, bottom
        0, 0, // left_start_y, left_end_y
        0, 0, // right_start_y, right_end_y
        (uint32_t)bar_x, (uint32_t)(bar_x + bar_w - 1), // top_start_x, top_end_x
        0, 0, // bottom_start_x, bottom_end_x
    };
    (void)XChangeProperty(disp, win, strut_atom, cardinal_atom, 32, PropModeReplace,
        (const unsigned char *)strut, STRUT_PARTIAL_LEN);
    (void)XChangeProperty(disp, win, wtype_atom, atom_type, 32, PropModeReplace,
        (const unsigned char *)&dock_atom, 1);

    (void)XMapRaised(disp, win);

    cairo_surface_t *surface = cairo_xlib_surface_create(disp, win, XDefaultVisual(disp, screen),
        bar_w, BAR_HEIGHT);
    cairo_t *cr = cairo_create(surface);

    (void)XSelectInput(disp, win, ExposureMask | StructureNotifyMask | ButtonPressMask);
    (void)XFlush(disp);

    Tray tray;
    memset(&tray, 0, sizeof(tray)); // Zig's `Tray{}`
    tray_init(&tray, disp, screen, win, root);

    char scratch_buf[MAX_STATUS];
    LayoutList layouts = query_layout_list(scratch_buf, sizeof(scratch_buf));

    char status_buf[MAX_STATUS];
    size_t status_len = 0;
    VolState vol = { 0, false, false };
    EthState eth = ETH_NONE;
    Slice kbd_name = mk_slice("??", 2);

    while (true) {
        if (have_command)
            status_len = run_command(status_buf, sizeof(status_buf), command);
        else
            status_len = get_clock(status_buf, sizeof(status_buf));
        vol = query_volume(scratch_buf, sizeof(scratch_buf));
        eth = query_ethernet();
        kbd_name = current_layout_name(disp, &layouts);

        int tray_left = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
        draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tray_left);

        // Sleep in 250ms slices so resize/expose/tray events still get
        // noticed promptly; REFRESH_MS is only a target interval.
        int64_t elapsed = 0;
        while (elapsed < REFRESH_MS) {
            int64_t capped = elapsed < REFRESH_MS ? elapsed : (int64_t)REFRESH_MS;
            int timeout = REFRESH_MS - (int)capped;
            int64_t a = (int64_t)250 * 1000;
            int64_t b = (int64_t)timeout * 1000;
            unsigned sleep_us = (unsigned)(a < b ? a : b);
            (void)usleep(sleep_us);
            elapsed += 250;

            // Re-read the active XKB group every tick (cheap: one X
            // round-trip, no shelling out) so Alt+Space feels instant
            // instead of waiting for the once-a-second refresh below.
            Slice new_kbd = current_layout_name(disp, &layouts);
            if (!slice_eql(new_kbd, kbd_name)) {
                kbd_name = new_kbd;
                int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
            }

            while (XPending(disp) > 0) {
                XEvent ev;
                (void)XNextEvent(disp, &ev);
                switch (ev.type) {
                case Expose: {
                    int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                    draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
                    break;
                }
                case ClientMessage: {
                    if (ev.xclient.message_type == tray.atom_opcode && ev.xclient.data.l[1] == 0) {
                        // SYSTEM_TRAY_REQUEST_DOCK: data.l[2] is the icon window.
                        Window icon_win = (Window)ev.xclient.data.l[2];
                        tray_handle_dock_request(&tray, disp, win, icon_win);
                        int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                        draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
                    }
                    break;
                }
                case ButtonPress: {
                    // Buttons 4/5 are the X11 scroll-wheel convention
                    // (up/down); only act when the pointer is over
                    // the volume module's last-drawn extent.
                    int bx = ev.xbutton.x;
                    if (bx >= vol_hit_x0 && bx <= vol_hit_x1 &&
                        (ev.xbutton.button == 4 || ev.xbutton.button == 5)) {
                        bool scroll_up = ev.xbutton.button == 4;
                        adjust_volume(scroll_up);
                        nudge_volume_locally(&vol, scroll_up);
                        int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                        draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
                    }
                    break;
                }
                case DestroyNotify: {
                    if (tray_remove_if_present(&tray, ev.xdestroywindow.window)) {
                        int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                        draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
                    }
                    break;
                }
                case ReparentNotify: {
                    // Icon reparented away from us (client withdrew itself).
                    if (ev.xreparent.parent != win &&
                        tray_remove_if_present(&tray, ev.xreparent.window)) {
                        int tl = tray_layout(&tray, disp, bar_w - EDGE_GAP - RIGHT_MARGIN);
                        draw(cr, bar_w, mk_slice(status_buf, status_len), eth, kbd_name, vol, tl);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
}
