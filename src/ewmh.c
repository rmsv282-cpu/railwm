// Minimal EWMH support -- direct C port of ewmh.zig (which was itself
// a port of the C ewmh.c/ewmh.h, which was itself a port of scrollwm's
// ewmh.py). Same scope: enough for a status bar to see window/focus
// state, and enough to tell a window's declared type so it can be
// floated instead of tiled.

#include "ewmh.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <string.h>

/* The atom table, same layout as the original `atom_names[ATOM_COUNT]`
 * array. The enum tag is only used to look entries up (see
 * ewmh_init) and to decide which atoms go in _NET_SUPPORTED. */
static const struct {
    AtomIndex idx;
    const char *name;
} atom_names_list[ATOM_COUNT] = {
    { ATOM_NET_SUPPORTED, "_NET_SUPPORTED" },
    { ATOM_NET_SUPPORTING_WM_CHECK, "_NET_SUPPORTING_WM_CHECK" },
    { ATOM_NET_WM_NAME, "_NET_WM_NAME" },
    { ATOM_NET_CLIENT_LIST, "_NET_CLIENT_LIST" },
    { ATOM_NET_CLIENT_LIST_STACKING, "_NET_CLIENT_LIST_STACKING" },
    { ATOM_NET_NUMBER_OF_DESKTOPS, "_NET_NUMBER_OF_DESKTOPS" },
    { ATOM_NET_CURRENT_DESKTOP, "_NET_CURRENT_DESKTOP" },
    { ATOM_NET_DESKTOP_NAMES, "_NET_DESKTOP_NAMES" },
    { ATOM_NET_ACTIVE_WINDOW, "_NET_ACTIVE_WINDOW" },
    { ATOM_NET_WM_WINDOW_TYPE, "_NET_WM_WINDOW_TYPE" },
    { ATOM_NET_WM_WINDOW_TYPE_DIALOG, "_NET_WM_WINDOW_TYPE_DIALOG" },
    { ATOM_NET_WM_WINDOW_TYPE_UTILITY, "_NET_WM_WINDOW_TYPE_UTILITY" },
    { ATOM_NET_WM_WINDOW_TYPE_SPLASH, "_NET_WM_WINDOW_TYPE_SPLASH" },
    { ATOM_NET_WM_WINDOW_TYPE_TOOLBAR, "_NET_WM_WINDOW_TYPE_TOOLBAR" },
    { ATOM_NET_WM_WINDOW_TYPE_NORMAL, "_NET_WM_WINDOW_TYPE_NORMAL" },
    { ATOM_NET_WM_STATE, "_NET_WM_STATE" },
    { ATOM_NET_WM_STATE_FULLSCREEN, "_NET_WM_STATE_FULLSCREEN" },
    { ATOM_NET_WM_STATE_MODAL, "_NET_WM_STATE_MODAL" },
    { ATOM_NET_WM_STRUT_PARTIAL, "_NET_WM_STRUT_PARTIAL" },
    { ATOM_UTF8_STRING, "UTF8_STRING" },
    { ATOM_WM_TRANSIENT_FOR, "WM_TRANSIENT_FOR" },
    { ATOM_WM_PROTOCOLS, "WM_PROTOCOLS" },
    { ATOM_WM_DELETE_WINDOW, "WM_DELETE_WINDOW" },
};

void ewmh_init(EWMH *e, Display *disp, Window root) {
    e->disp = disp;
    e->root = root;
    e->supporting_win = 0;
    for (int i = 0; i < ATOM_COUNT; i++)
        e->atoms[atom_names_list[i].idx] = XInternAtom(disp, atom_names_list[i].name, False);
}

void ewmh_announce_support(EWMH *e) {
    // _NET_SUPPORTED holds every _NET_* atom we intern.
    Atom supported[ATOM_COUNT];
    int n = 0;
    for (int i = 0; i < ATOM_COUNT; i++) {
        if (strncmp(atom_names_list[i].name, "_NET_", 5) == 0) {
            supported[n] = e->atoms[atom_names_list[i].idx];
            n += 1;
        }
    }
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_SUPPORTED],
        XA_ATOM, 32, PropModeReplace, (unsigned char *)supported, n);

    // A small unmapped window, per spec, whose _NET_WM_NAME
    // identifies us.
    Window check = XCreateSimpleWindow(e->disp, e->root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(e->disp, check, e->atoms[ATOM_NET_WM_NAME],
        e->atoms[ATOM_UTF8_STRING], 8, PropModeReplace,
        (unsigned char *)EWMH_WM_NAME, (int)strlen(EWMH_WM_NAME));

    Window check_id = check;
    XChangeProperty(e->disp, check, e->atoms[ATOM_NET_SUPPORTING_WM_CHECK],
        XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check_id, 1);
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_SUPPORTING_WM_CHECK],
        XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check_id, 1);
    e->supporting_win = check;

    long one = 1;
    long zero = 0;
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_NUMBER_OF_DESKTOPS],
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_CURRENT_DESKTOP],
        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&zero, 1);

    const char *name = "scroll";
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_DESKTOP_NAMES],
        e->atoms[ATOM_UTF8_STRING], 8, PropModeReplace,
        (unsigned char *)name, (int)(strlen(name) + 1));
}

void ewmh_set_client_list(EWMH *e, const unsigned long *window_ids, int n) {
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_CLIENT_LIST],
        XA_WINDOW, 32, PropModeReplace, (unsigned char *)window_ids, n);
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_CLIENT_LIST_STACKING],
        XA_WINDOW, 32, PropModeReplace, (unsigned char *)window_ids, n);
}

void ewmh_set_active_window(EWMH *e, unsigned long window_id) {
    XChangeProperty(e->disp, e->root, e->atoms[ATOM_NET_ACTIVE_WINDOW],
        XA_WINDOW, 32, PropModeReplace, (unsigned char *)&window_id, 1);
}

/* Reads an XA_ATOM list property into `out` (capacity `out_cap` atoms)
 * and returns how many were written. */
static size_t get_atom_list_property(EWMH *e, Window win, Atom prop,
    Atom *out, size_t out_cap) {
    Atom type_ret = 0;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(e->disp, win, prop, 0, 64, False, XA_ATOM,
        &type_ret, &format, &nitems, &bytes_after, &data);
    if (status != Success || data == NULL)
        return 0;

    size_t n = (size_t)nitems;
    if (n > out_cap)
        n = out_cap;
    memcpy(out, data, n * sizeof(Atom));
    XFree(data);
    return n;
}

/* Reads a single-item property of the given type into `out`. Returns
 * false (leaving `out` untouched) if the property is absent or empty. */
static bool get_window_property_single(EWMH *e, Window win, Atom prop,
    Atom type_, unsigned long *out) {
    Atom actual_type = 0;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int status = XGetWindowProperty(e->disp, win, prop, 0, 1, False, type_,
        &actual_type, &format, &nitems, &bytes_after, &data);
    if (status != Success || data == NULL || nitems == 0)
        return false;

    // Xlib hands format-32 data back in `long` units, same as the
    // original's direct cast through *c_ulong.
    memcpy(out, data, sizeof(unsigned long));
    XFree(data);
    return true;
}

/* Adds or removes _NET_WM_STATE_FULLSCREEN from a window's
 * _NET_WM_STATE property, preserving whatever other states (e.g.
 * _NET_WM_STATE_MODAL) the client already had set.
 *
 * Resizing and raising a window to cover the screen is only half of
 * what "going fullscreen" means over EWMH. A spec-following client --
 * Chromium/Chrome included, which is what YouTube's fullscreen actually
 * runs inside of -- doesn't consider the transition confirmed until
 * this property reflects it. Skip the write and the browser's own
 * fullscreen state tracking (hiding chrome, the "press Esc to exit"
 * overlay, video sizing) can end up out of sync with what the WM
 * actually did to the window. */
void ewmh_set_fullscreen_state(EWMH *e, Window win, bool enable) {
    Atom states[64];
    size_t n = get_atom_list_property(e, win, e->atoms[ATOM_NET_WM_STATE],
        states, sizeof(states) / sizeof(states[0]));

    Atom fs_atom = e->atoms[ATOM_NET_WM_STATE_FULLSCREEN];
    bool has_fs = false;
    for (size_t i = 0; i < n; i++) {
        if (states[i] == fs_atom) {
            has_fs = true;
            break;
        }
    }

    if (enable && !has_fs && n < sizeof(states) / sizeof(states[0])) {
        states[n] = fs_atom;
        n += 1;
    } else if (!enable && has_fs) {
        size_t out_n = 0;
        for (size_t i = 0; i < n; i++) {
            if (states[i] != fs_atom) {
                states[out_n] = states[i];
                out_n += 1;
            }
        }
        n = out_n;
    } else {
        return; // already in the desired state, nothing to write
    }

    XChangeProperty(e->disp, win, e->atoms[ATOM_NET_WM_STATE],
        XA_ATOM, 32, PropModeReplace, (unsigned char *)states, (int)n);
}

/* A window floats (instead of joining the scroll strip) if it
 * declares a floating _NET_WM_WINDOW_TYPE, if it's transient for
 * another window (the classic "I'm a dialog owned by X" signal that
 * predates _NET_WM_WINDOW_TYPE and plenty of apps still rely on), or
 * if it sets _NET_WM_STATE_MODAL. The last one matters specifically
 * for portal-spawned dialogs (xdg-desktop-portal's file chooser, etc.)
 * -- those come from a separate process from the app that requested
 * them, so WM_TRANSIENT_FOR isn't always reliably set cross-process,
 * but a modal dialog reliably sets MODAL regardless. */
bool ewmh_should_float(EWMH *e, Window win) {
    Atom types[64];
    size_t n = get_atom_list_property(e, win, e->atoms[ATOM_NET_WM_WINDOW_TYPE],
        types, sizeof(types) / sizeof(types[0]));

    for (size_t i = 0; i < n; i++) {
        if (types[i] == e->atoms[ATOM_NET_WM_WINDOW_TYPE_DIALOG] ||
            types[i] == e->atoms[ATOM_NET_WM_WINDOW_TYPE_UTILITY] ||
            types[i] == e->atoms[ATOM_NET_WM_WINDOW_TYPE_SPLASH] ||
            types[i] == e->atoms[ATOM_NET_WM_WINDOW_TYPE_TOOLBAR]) {
            return true;
        }
    }

    unsigned long transient_for = 0;
    if (get_window_property_single(e, win, e->atoms[ATOM_WM_TRANSIENT_FOR],
            XA_WINDOW, &transient_for))
        return true;

    Atom states[64];
    size_t sn = get_atom_list_property(e, win, e->atoms[ATOM_NET_WM_STATE],
        states, sizeof(states) / sizeof(states[0]));
    for (size_t i = 0; i < sn; i++) {
        if (states[i] == e->atoms[ATOM_NET_WM_STATE_MODAL])
            return true;
    }

    // Same heuristic i3 uses in manage.c: a window that declares
    // itself non-resizable (min size == max size in both dimensions)
    // is almost always a dialog, regardless of whether it also set any
    // of the atoms above -- catches cases where a portal dialog
    // doesn't set MODAL or WM_TRANSIENT_FOR correctly either.
    XSizeHints hints;
    long supplied = 0;
    if (XGetWMNormalHints(e->disp, win, &hints, &supplied) != 0) {
        if ((hints.flags & PMaxSize) != 0 && (hints.flags & PMinSize) != 0 &&
            hints.max_width > 0 && hints.max_height > 0 &&
            hints.min_width == hints.max_width &&
            hints.min_height == hints.max_height) {
            return true;
        }
    }

    return false;
}
