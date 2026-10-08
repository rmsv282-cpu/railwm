// Scrolling-tiling layout engine -- direct C port of layout.zig (which
// was itself a port of the C layout.c/layout.h, which was itself the
// unit-tested port of the Python layout.py, 6/6 passing in scrollwm).
// Same model: a single workspace holds an ordered strip of columns,
// one window each (no vertical splits in v1). The viewport scrolls
// horizontally so the focused column is always fully visible.
//
// Zero X11 dependency on purpose -- this is pure geometry/list
// bookkeeping you can unit-test without a display server, which is
// what actually let bugs get caught cheaply on the Python side. The
// tests live in tests/test_layout.c (`make test`, no X server needed)
// instead of in a separate test_program.

#include "layout.h"

#include <string.h>

void layout_init(ScrollLayout *sl, int32_t screen_width, int32_t screen_height) {
    memset(sl, 0, sizeof(*sl));
    sl->screen_width = screen_width;
    sl->screen_height = screen_height;
    sl->gap = DEFAULT_GAP;
    sl->default_column_width = DEFAULT_COLUMN_WIDTH;
    sl->min_column_width = DEFAULT_MIN_COLUMN_WIDTH;
}

/* All the pre-existing single-workspace logic goes through this helper
 * instead of a bare `sl.workspaces[active_workspace]` now that there
 * are MAX_WORKSPACES independent strips -- every function's behavior
 * is unchanged, it's just "the active one" instead of "the only one". */
static Workspace *active_ws(ScrollLayout *sl) {
    return &sl->workspaces[sl->active_workspace];
}

Column *layout_focused(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    if (ws->ncolumns == 0)
        return NULL;
    return &ws->columns[ws->focused_index];
}

/* Left edge of column[index] in strip-space (unscrolled) pixels. */
static int32_t strip_x_of(ScrollLayout *sl, int32_t index) {
    Workspace *ws = active_ws(sl);
    int32_t x = sl->gap;
    for (int32_t i = 0; i < index; i++)
        x += ws->columns[i].width + sl->gap;
    return x;
}

/* Adjust viewport_x so the focused column is fully on screen. */
static void scroll_to_focused(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    if (ws->ncolumns == 0) {
        ws->viewport_x = 0;
        return;
    }
    Column *col = layout_focused(sl);
    int32_t left = strip_x_of(sl, ws->focused_index);
    int32_t right = left + col->width;
    int32_t vx = ws->viewport_x;

    if (left < vx + sl->gap)
        vx = left - sl->gap;
    else if (right > vx + sl->screen_width - sl->gap)
        vx = right - sl->screen_width + sl->gap;

    ws->viewport_x = vx > 0 ? vx : 0;
}

bool layout_add_window(ScrollLayout *sl, uint64_t window_id, bool after_focused) {
    Workspace *ws = active_ws(sl);
    if (ws->ncolumns >= MAX_COLUMNS)
        return false;

    int32_t insert_at;
    if (ws->ncolumns == 0) {
        insert_at = 0;
    } else {
        insert_at = after_focused ? ws->focused_index + 1 : ws->ncolumns;
    }

    /* shift everything at/after insert_at right by one */
    for (int32_t i = ws->ncolumns; i > insert_at; i--)
        ws->columns[i] = ws->columns[i - 1];

    /* Explicit, not implicit: this slot may be reused from a
     * previously-removed column (remove_window shifts entries down by
     * struct-copy, it doesn't clear the vacated tail slot), so
     * maximized/saved_width could otherwise carry over stale garbage
     * from whatever used to occupy this array index. Zeroing the whole
     * struct first resets every field, so there's nothing to leak
     * here.
     *
     * width is also explicit rather than relying on a compile-time
     * default: sl->default_column_width may have been overridden by
     * config, and that's the value new columns should actually get. */
    Column *slot = &ws->columns[insert_at];
    memset(slot, 0, sizeof(*slot));
    slot->window_id = window_id;
    slot->width = sl->default_column_width;

    ws->ncolumns += 1;
    ws->focused_index = insert_at;

    scroll_to_focused(sl);
    return true;
}

/* Searches EVERY workspace, not just the active one -- a window can be
 * destroyed (DestroyNotify) regardless of which workspace it's
 * currently on, and the caller has no cheap way to know which one that
 * is ahead of time. Only the workspace actually containing the match
 * gets its focused_index/viewport_x fixed up; every other workspace is
 * untouched, since a removal on one workspace has no bearing on
 * another's focus/scroll state. */
bool layout_remove_window(ScrollLayout *sl, uint64_t window_id) {
    bool removed_any = false;
    for (int32_t w = 0; w < MAX_WORKSPACES; w++) {
        Workspace *ws = &sl->workspaces[w];
        bool removed_here = false;
        for (int32_t i = 0; i < ws->ncolumns;) {
            if (ws->columns[i].window_id == window_id) {
                for (int32_t j = i; j < ws->ncolumns - 1; j++)
                    ws->columns[j] = ws->columns[j + 1];
                ws->ncolumns -= 1;
                removed_here = true;
                removed_any = true;
                /* don't advance i -- the next element just shifted into this slot */
            } else {
                i += 1;
            }
        }
        if (!removed_here)
            continue;

        if (ws->ncolumns > 0) {
            if (ws->focused_index > ws->ncolumns - 1)
                ws->focused_index = ws->ncolumns - 1;
        } else {
            ws->focused_index = 0;
        }
        if (w == sl->active_workspace)
            scroll_to_focused(sl);
    }
    return removed_any;
}

void layout_focus_next(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    if (ws->ncolumns < 2)
        return;
    ws->focused_index = (ws->focused_index + 1) % ws->ncolumns;
    scroll_to_focused(sl);
}

void layout_focus_prev(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    if (ws->ncolumns < 2)
        return;
    ws->focused_index = (ws->focused_index - 1 + ws->ncolumns) % ws->ncolumns;
    scroll_to_focused(sl);
}

void layout_move_focused_right(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    int32_t i = ws->focused_index;
    if (i < ws->ncolumns - 1) {
        Column tmp = ws->columns[i];
        ws->columns[i] = ws->columns[i + 1];
        ws->columns[i + 1] = tmp;
        ws->focused_index = i + 1;
        scroll_to_focused(sl);
    }
}

void layout_move_focused_left(ScrollLayout *sl) {
    Workspace *ws = active_ws(sl);
    int32_t i = ws->focused_index;
    if (i > 0) {
        Column tmp = ws->columns[i];
        ws->columns[i] = ws->columns[i - 1];
        ws->columns[i - 1] = tmp;
        ws->focused_index = i - 1;
        scroll_to_focused(sl);
    }
}

void layout_resize_focused(ScrollLayout *sl, int32_t delta_px) {
    Column *col = layout_focused(sl);
    if (col == NULL)
        return;
    /* A manual resize while maximized is a deliberate override --
     * without clearing this, toggling maximize off afterward would
     * silently discard the resize and jump back to whatever width
     * existed before maximizing, which isn't what a resize keypress
     * should do. */
    col->maximized = false;
    int32_t w = col->width + delta_px;
    col->width = w > sl->min_column_width ? w : sl->min_column_width;
    scroll_to_focused(sl);
}

/* Toggles the focused column between its normal width and filling the
 * whole screen (minus the outer gap). Calling it again restores
 * whatever width it had before maximizing -- including one set by a
 * manual resize, not just default_column_width. */
void layout_toggle_maximize_focused(ScrollLayout *sl) {
    Column *col = layout_focused(sl);
    if (col == NULL)
        return;

    if (col->maximized) {
        col->width = col->saved_width;
        col->maximized = false;
    } else {
        col->saved_width = col->width;
        col->width = sl->screen_width - 2 * sl->gap;
        col->maximized = true;
    }
    scroll_to_focused(sl);
}

/* Switches the active workspace. ws is clamped into
 * [0, MAX_WORKSPACES). No-op if ws is already active. Windows aren't
 * moved between workspaces by this -- see
 * layout_move_focused_to_workspace. Each workspace has its own
 * independent column strip, focus, and scroll position, so switching
 * back to one you were on before restores exactly where you left it. */
void layout_switch_workspace(ScrollLayout *sl, int32_t ws) {
    if (ws < 0)
        ws = 0;
    if (ws >= MAX_WORKSPACES)
        ws = MAX_WORKSPACES - 1;
    /* No early-return on ws == active_workspace anymore: a silent
     * no-op here is indistinguishable, from the outside, between
     * "already there, correctly did nothing" and "thought it already
     * switched but never actually did." Always re-running the switch
     * (even to the same workspace) costs nothing and removes that
     * ambiguity -- the caller's apply_layout() afterward re-syncs
     * everything either way. */
    sl->active_workspace = ws;
    /* Each workspace already keeps its own focused_index/viewport_x
     * (they live in Workspace, not ScrollLayout), so there's nothing
     * to recompute here. */
}

/* Moves the focused window from the active workspace to workspace ws,
 * then switches to ws so the window stays focused and visible rather
 * than leaving you behind on the now-emptier workspace. No-op if
 * there's no focused window, or ws is already the active workspace. */
void layout_move_focused_to_workspace(ScrollLayout *sl, int32_t ws) {
    if (ws < 0)
        ws = 0;
    if (ws >= MAX_WORKSPACES)
        ws = MAX_WORKSPACES - 1;
    if (ws == sl->active_workspace)
        return;

    Column *focused_col = layout_focused(sl);
    if (focused_col == NULL)
        return;
    uint64_t window_id = focused_col->window_id;
    Column moved = *focused_col;

    Workspace *src = active_ws(sl);
    int32_t i = src->focused_index;
    for (int32_t j = i; j < src->ncolumns - 1; j++)
        src->columns[j] = src->columns[j + 1];
    src->ncolumns -= 1;
    if (src->ncolumns > 0) {
        if (src->focused_index > src->ncolumns - 1)
            src->focused_index = src->ncolumns - 1;
    } else {
        src->focused_index = 0;
    }
    /* sl->active_workspace is still src here -- scroll_to_focused()
     * always acts on active_ws(), so this call fixes up src's
     * viewport_x for its new (smaller) column count before we switch
     * away. Without this, src's viewport_x is left exactly as it was
     * pre-removal: correct for the old column count, but not
     * necessarily for the new one, so whatever's left on src can end
     * up scrolled past and rendered off-screen the next time you
     * switch back to it. */
    scroll_to_focused(sl);

    Workspace *dst = &sl->workspaces[ws];
    if (dst->ncolumns < MAX_COLUMNS) {
        /* append -- arriving from elsewhere, there's no "after focus"
         * position on a workspace you weren't on */
        int32_t insert_at = dst->ncolumns;
        dst->columns[insert_at] = moved;
        dst->columns[insert_at].window_id = window_id;
        dst->ncolumns += 1;
        dst->focused_index = insert_at;
    }
    /* If the destination workspace happened to be full, the window is
     * now dropped from src and nowhere -- same MAX_COLUMNS ceiling
     * layout_add_window already has, not a new failure mode, but worth
     * the same caveat: this is a fixed-size array, not a list. */

    sl->active_workspace = ws;
    scroll_to_focused(sl);
}

/* Fills `out` (capacity `out_cap` entries) with every column
 * currently within (or overlapping) the visible viewport, and returns
 * how many entries were written. Off-screen columns are omitted --
 * caller should unmap/skip anything not in this list, same contract as
 * the Python geometry(). */
size_t layout_geometry(ScrollLayout *sl, GeomEntry *out, size_t out_cap) {
    Workspace *ws = active_ws(sl);
    size_t n = 0;
    int32_t h = sl->screen_height - sl->top_inset - 2 * sl->gap;
    for (int32_t i = 0; i < ws->ncolumns; i++) {
        if (n >= out_cap)
            break;
        int32_t strip_x = strip_x_of(sl, i);
        int32_t screen_x = strip_x - ws->viewport_x;
        Column *col = &ws->columns[i];
        if (screen_x + col->width < 0 || screen_x > sl->screen_width)
            continue;
        out[n].window_id = col->window_id;
        out[n].x = screen_x;
        out[n].y = sl->gap + sl->top_inset;
        out[n].w = col->width;
        out[n].h = h;
        n += 1;
    }
    return n;
}
