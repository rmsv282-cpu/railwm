// Minimal EWMH support -- direct C port of ewmh.zig (which was itself
// a port of the C ewmh.c/ewmh.h, which was itself a port of scrollwm's
// ewmh.py). Same scope: enough for a status bar to see window/focus
// state, and enough to tell a window's declared type so it can be
// floated instead of tiled.

#ifndef RAILWM_EWMH_H
#define RAILWM_EWMH_H

#include <X11/Xlib.h>
#include <stdbool.h>

#define EWMH_WM_NAME "railwm"

typedef enum {
    ATOM_NET_SUPPORTED,
    ATOM_NET_SUPPORTING_WM_CHECK,
    ATOM_NET_WM_NAME,
    ATOM_NET_CLIENT_LIST,
    ATOM_NET_CLIENT_LIST_STACKING,
    ATOM_NET_NUMBER_OF_DESKTOPS,
    ATOM_NET_CURRENT_DESKTOP,
    ATOM_NET_DESKTOP_NAMES,
    ATOM_NET_ACTIVE_WINDOW,
    ATOM_NET_WM_WINDOW_TYPE,
    ATOM_NET_WM_WINDOW_TYPE_DIALOG,
    ATOM_NET_WM_WINDOW_TYPE_UTILITY,
    ATOM_NET_WM_WINDOW_TYPE_SPLASH,
    ATOM_NET_WM_WINDOW_TYPE_TOOLBAR,
    ATOM_NET_WM_WINDOW_TYPE_NORMAL,
    ATOM_NET_WM_STATE,
    ATOM_NET_WM_STATE_FULLSCREEN,
    ATOM_NET_WM_STATE_MODAL,
    ATOM_NET_WM_STRUT_PARTIAL,
    ATOM_UTF8_STRING,
    ATOM_WM_TRANSIENT_FOR,
    ATOM_WM_PROTOCOLS,
    ATOM_WM_DELETE_WINDOW,
    /* Must match the number of tags above (mirrors the original
     * ATOM_COUNT). */
    ATOM_COUNT
} AtomIndex;

typedef struct {
    Display *disp;
    Window root;
    Atom atoms[ATOM_COUNT];
    Window supporting_win;
} EWMH;

void ewmh_init(EWMH *e, Display *disp, Window root);
void ewmh_announce_support(EWMH *e);
void ewmh_set_client_list(EWMH *e, const unsigned long *window_ids, int n);
void ewmh_set_active_window(EWMH *e, unsigned long window_id);

/* Adds or removes _NET_WM_STATE_FULLSCREEN from a window's
 * _NET_WM_STATE property, preserving whatever other states the client
 * already had set. See ewmh.c for why a spec-following client
 * (Chromium included) needs this write to actually confirm the
 * transition. */
void ewmh_set_fullscreen_state(EWMH *e, Window win, bool enable);

/* A window floats (instead of joining the scroll strip) if it declares
 * a floating _NET_WM_WINDOW_TYPE, if it's transient for another
 * window, or if it sets _NET_WM_STATE_MODAL -- plus the i3-style
 * "non-resizable == dialog" heuristic. See ewmh.c. */
bool ewmh_should_float(EWMH *e, Window win);

#endif /* RAILWM_EWMH_H */
