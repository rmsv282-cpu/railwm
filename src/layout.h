// Scrolling-tiling layout engine -- direct C port of layout.zig (which
// was itself a port of the C layout.c/layout.h, which was itself the
// unit-tested port of the Python layout.py). Same model: a single
// workspace holds an ordered strip of columns, one window each (no
// vertical splits in v1). The viewport scrolls horizontally so the
// focused column is always fully visible.
//
// Zero X11 dependency on purpose -- this is pure geometry/list
// bookkeeping you can unit-test without a display server, which is
// what actually let bugs get caught cheaply on the Python side. The
// tests live in tests/test_layout.c (`make test`, no X server needed).

#ifndef RAILWM_LAYOUT_H
#define RAILWM_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_COLUMNS 64
#define MAX_WORKSPACES 9

/* 656 + 16 gap = exactly 2 columns filling a 1360px-wide screen edge
 * to edge, evenly gapped. These are compile-time defaults only now --
 * the WM's config file can override the actual values used at runtime
 * via ScrollLayout.gap / .default_column_width / .min_column_width
 * below, set once after layout_init() and before any window is added. */
#define DEFAULT_COLUMN_WIDTH 656
#define DEFAULT_MIN_COLUMN_WIDTH 200
#define DEFAULT_GAP 16

typedef struct {
    /* Opaque handle -- an X11 Window id. */
    uint64_t window_id;
    int32_t width;
    /* True if this column is currently maximized. */
    bool maximized;
    /* Width to restore to when un-maximized. */
    int32_t saved_width;
} Column;

typedef struct {
    Column columns[MAX_COLUMNS];
    int32_t ncolumns;
    int32_t focused_index;
    int32_t viewport_x;
} Workspace;

typedef struct {
    uint64_t window_id;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} GeomEntry;

typedef struct {
    int32_t screen_width;
    int32_t screen_height;
    /* Pixels reserved at the top of the screen for a status bar / dock
     * (_NET_WM_STRUT_PARTIAL "top"), set dynamically by the WM as long
     * as a dock is present. Tiled windows start below it. */
    int32_t top_inset;
    Workspace workspaces[MAX_WORKSPACES];
    /* 0-based index into workspaces[]. */
    int32_t active_workspace;

    /* Runtime-configurable (see wm.c load_config()); default to the
     * same values the old compile-time constants held, so behavior is
     * unchanged unless a config file overrides them. Safe to assign
     * directly right after layout_init(), before any add_window call
     * -- nothing here is used until then. */
    int32_t gap;
    int32_t default_column_width;
    int32_t min_column_width;
} ScrollLayout;

void layout_init(ScrollLayout *sl, int32_t screen_width, int32_t screen_height);

/* Returns the currently-focused column, or NULL if the workspace is
 * empty. */
Column *layout_focused(ScrollLayout *sl);

/* Returns true on success, false if MAX_COLUMNS was already reached. */
bool layout_add_window(ScrollLayout *sl, uint64_t window_id, bool after_focused);

/* Returns true if the window was found and removed, false otherwise.
 * Searches EVERY workspace, not just the active one. */
bool layout_remove_window(ScrollLayout *sl, uint64_t window_id);

void layout_focus_next(ScrollLayout *sl);
void layout_focus_prev(ScrollLayout *sl);
void layout_move_focused_right(ScrollLayout *sl);
void layout_move_focused_left(ScrollLayout *sl);
void layout_resize_focused(ScrollLayout *sl, int32_t delta_px);
void layout_toggle_maximize_focused(ScrollLayout *sl);

/* Switches the active workspace. ws is clamped into
 * [0, MAX_WORKSPACES). No-op if ws is already active. */
void layout_switch_workspace(ScrollLayout *sl, int32_t ws);

/* Moves the focused window from the active workspace to workspace ws,
 * then switches to ws so the window stays focused and visible. */
void layout_move_focused_to_workspace(ScrollLayout *sl, int32_t ws);

/* Fills `out` (capacity `out_cap` entries) with every column
 * currently within (or overlapping) the visible viewport, and returns
 * how many entries were written. Off-screen columns are omitted --
 * caller should unmap/skip anything not in this list, same contract as
 * the Python geometry(). */
size_t layout_geometry(ScrollLayout *sl, GeomEntry *out, size_t out_cap);

#endif /* RAILWM_LAYOUT_H */
