// railnotify -- a minimal D-Bus notification daemon for railwm.
//
// Registers org.freedesktop.Notifications on the session bus and shows
// incoming notifications as small override-redirect popups stacked in
// the top-right corner of the screen (raillauncher-style, same palette).
// Support is intentionally small but spec-compliant: Notify,
// CloseNotification, GetCapabilities, GetServerInformation, and the
// NotificationClosed signal on timeout. No actions, no hints, no body
// markup -- that matches the minimal ethos of the rest of the WM.

// clock_gettime/CLOCK_MONOTONIC are hidden under strict -std=c11
// unless a POSIX feature level is requested up front.
#define _POSIX_C_SOURCE 200809L

#include <dbus/dbus.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <poll.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTIFS 16
#define MAX_TEXT 512
#define NOTIF_W 320
#define NOTIF_H 54
#define NOTIF_MARGIN 6
#define NOTIF_GAP 6
#define DEFAULT_TIMEOUT_MS 5000
#define MAX_TIMEOUT_MS 10000
#define DBUS_SERVICE "org.freedesktop.Notifications"
#define DBUS_IFACE "org.freedesktop.Notifications"
#define DBUS_PATH "/org/freedesktop/Notifications"

typedef struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Color;

// Same palette as railwm's own defaults (see wm.c).
static const Color BG = { .r = 0x0a, .g = 0x14, .b = 0x28 };
static const Color FG = { .r = 0xe8, .g = 0xe8, .b = 0xe0 };
static const Color ACCENT = { .r = 0xff, .g = 0xbf, .b = 0x00 };
static const Color DIM = { .r = 0x4d, .g = 0x38, .b = 0x00 };

static void setSource(Color self, cairo_t *cr) {
    cairo_set_source_rgb(cr,
        (double)self.r / 255.0,
        (double)self.g / 255.0,
        (double)self.b / 255.0);
}

// Copies a possibly-null C string into a fixed sentinel buffer,
// returning the byte length written. Bounds the amount of memory a
// chatty peer can make the daemon touch.
static size_t copyCStr(char *dst, const char *src) {
    if (src) {
        size_t n = strlen(src);
        if (n > MAX_TEXT - 1) n = MAX_TEXT - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
        return n;
    }
    dst[0] = 0;
    return 0;
}

static int64_t nowMs(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

typedef struct Notif {
    uint32_t id;
    bool active;
    char summary[MAX_TEXT];
    size_t summary_len;
    char body[MAX_TEXT];
    size_t body_len;
    int64_t expires_ms;
    Window win;
    cairo_surface_t *surface;
    cairo_t *cr;
} Notif;

static DBusConnection *g_conn = NULL;
static Display *g_disp = NULL;
static int g_screen = 0;
static Window g_root = 0;
static int g_screen_w = 0;
static Notif g_notifs[MAX_NOTIFS];
static uint32_t g_next_id = 1;

// Picks the slot for a notification. A non-zero replaces_id reclaims
// the matching existing popup; otherwise the first free slot wins, and
// a full table evicts whichever popup expires earliest.
static Notif *findSlot(uint32_t replaces_id) {
    if (replaces_id != 0) {
        for (int i = 0; i < MAX_NOTIFS; i++) {
            Notif *n = &g_notifs[i];
            if (n->active && n->id == replaces_id) return n;
        }
    }
    for (int i = 0; i < MAX_NOTIFS; i++) {
        Notif *n = &g_notifs[i];
        if (!n->active) return n;
    }
    Notif *oldest = &g_notifs[0];
    for (int i = 0; i < MAX_NOTIFS; i++) {
        Notif *n = &g_notifs[i];
        if (n->expires_ms < oldest->expires_ms) oldest = n;
    }
    return oldest;
}

static void makeWindow(Notif *n) {
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask;

    n->win = XCreateWindow(g_disp, g_root, 0, 0,
        (unsigned)NOTIF_W, (unsigned)NOTIF_H, 0,
        CopyFromParent, InputOutput, NULL,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
    n->surface = cairo_xlib_surface_create(g_disp, n->win, XDefaultVisual(g_disp, g_screen), NOTIF_W, NOTIF_H);
    n->cr = cairo_create(n->surface);
    (void)XMapWindow(g_disp, n->win);
}

static void destroyWindow(Notif *n) {
    if (n->cr) cairo_destroy(n->cr);
    if (n->surface) cairo_surface_destroy(n->surface);
    if (n->win != 0) (void)XDestroyWindow(g_disp, n->win);
    n->cr = NULL;
    n->surface = NULL;
    n->win = 0;
}

static void drawNotif(Notif *n) {
    cairo_t *cr = n->cr;
    if (cr == NULL) return;

    setSource(BG, cr);
    cairo_paint(cr);

    setSource(ACCENT, cr);
    cairo_rectangle(cr, 0, 0, 3, NOTIF_H);
    cairo_fill(cr);

    if (n->summary_len > 0) {
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        setSource(ACCENT, cr);
        cairo_move_to(cr, 12, 20);
        cairo_show_text(cr, n->summary);
    }
    if (n->body_len > 0) {
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        setSource(FG, cr);
        cairo_move_to(cr, 12, 38);
        cairo_show_text(cr, n->body);
    }

    setSource(DIM, cr);
    cairo_rectangle(cr, 0, NOTIF_H - 1, NOTIF_W, 1);
    cairo_fill(cr);

    cairo_surface_flush(cairo_get_target(cr));
}

// Repositions every active popup in the top-right corner (newest at
// the top, at most 4 visible to avoid covering the whole edge) and
// re-renders the ones that moved.
static void relayout(void) {
    int y = NOTIF_MARGIN;
    size_t count = 0;
    for (int i = 0; i < MAX_NOTIFS; i++) {
        Notif *n = &g_notifs[i];
        if (n->active) {
            (void)XMoveWindow(g_disp, n->win, g_screen_w - NOTIF_W - NOTIF_MARGIN, y);
            drawNotif(n);
            y += NOTIF_H + NOTIF_GAP;
            count += 1;
        }
        if (count >= 4) break;
    }
    (void)XSync(g_disp, False);
}

static uint32_t addNotif(uint32_t replaces_id, const char *summary, const char *body, int timeout_ms) {
    Notif *n = findSlot(replaces_id);

    n->summary_len = copyCStr(n->summary, summary);
    n->body_len = copyCStr(n->body, body);
    int64_t t;
    if (timeout_ms > 0) {
        t = timeout_ms;
        if (t > MAX_TIMEOUT_MS) t = MAX_TIMEOUT_MS;
        if (t < 1000) t = 1000;
    } else {
        t = DEFAULT_TIMEOUT_MS;
    }
    n->expires_ms = nowMs() + t;

    if (!n->active) {
        n->id = g_next_id;
        g_next_id += 1;
        n->active = true;
    }
    destroyWindow(n); // if the slot held a previous popup, drop its window
    makeWindow(n);
    relayout();
    return n->id;
}

static void dropNotif(size_t idx) {
    Notif *n = &g_notifs[idx];
    if (!n->active) return;
    n->active = false;
    destroyWindow(n);
    relayout();
    (void)XSync(g_disp, False);
}

static void sendClosedSignal(uint32_t id) {
    if (g_conn) {
        DBusConnection *conn = g_conn;
        DBusMessage *sig = dbus_message_new_signal(DBUS_PATH, DBUS_IFACE, "NotificationClosed");
        if (sig == NULL) return;
        DBusMessageIter iter;
        dbus_message_iter_init_append(sig, &iter);
        uint32_t idv = id;
        uint32_t reason = 1; // Expired
        (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &idv);
        (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &reason);
        (void)dbus_connection_send(conn, sig, NULL);
        (void)dbus_connection_flush(conn);
        dbus_message_unref(sig);
    }
}

static void expirePass(void) {
    int64_t now = nowMs();
    for (size_t i = 0; i < MAX_NOTIFS; i++) {
        Notif *n = &g_notifs[i];
        if (n->active && n->expires_ms != 0 && now >= n->expires_ms) {
            sendClosedSignal(n->id);
            dropNotif(i);
        }
    }
}

// ---- D-Bus method reply helpers --------------------------------------

static void appendUint32(DBusMessageIter *iter, uint32_t v) {
    (void)dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &v);
}

static void appendString(DBusMessageIter *iter, const char *s) {
    (void)dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &s);
}

typedef void (*ReplyBody)(DBusMessageIter *iter);

// Builds and sends a method-return reply to `msg` whose payload is
// produced by `body`.
static void sendReply(DBusMessage *msg, ReplyBody body) {
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL) return;
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    body(&iter);
    (void)dbus_connection_send(g_conn, reply, NULL);
    dbus_message_unref(reply);
}

static void replyCapabilities(DBusMessageIter *iter) {
    DBusMessageIter sub;
    (void)dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &sub);
    const char *cap = "body";
    (void)dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &cap);
    (void)dbus_message_iter_close_container(iter, &sub);
}

static void replyServerInfo(DBusMessageIter *iter) {
    appendString(iter, "railnotify");
    appendString(iter, "railwm");
    appendString(iter, "0.1");
    appendString(iter, "1.2");
}

static void replyVoid(DBusMessageIter *iter) { (void)iter; }

// ---- D-Bus method implementations ------------------------------------

static void handleNotify(DBusMessage *msg) {
    DBusMessageIter iter;
    (void)dbus_message_iter_init(msg, &iter);

    // app_name (s, ignored)
    const char *app_name = NULL;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&iter, &app_name);
    (void)dbus_message_iter_next(&iter);

    // replaces_id (u)
    uint32_t replaces_id = 0;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) return;
    dbus_message_iter_get_basic(&iter, &replaces_id);
    (void)dbus_message_iter_next(&iter);

    // app_icon (s, ignored)
    const char *app_icon = NULL;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&iter, &app_icon);
    (void)dbus_message_iter_next(&iter);

    // summary (s)
    const char *summary = NULL;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&iter, &summary);
    (void)dbus_message_iter_next(&iter);

    // body (s)
    const char *body = NULL;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) return;
    dbus_message_iter_get_basic(&iter, &body);
    (void)dbus_message_iter_next(&iter);

    // actions (as) -- skipped
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) return;
    (void)dbus_message_iter_next(&iter);

    // hints (a{sv}) -- skipped
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) return;
    (void)dbus_message_iter_next(&iter);

    // expire_timeout (i)
    int timeout = 0;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT32) return;
    dbus_message_iter_get_basic(&iter, &timeout);
    (void)dbus_message_iter_next(&iter);

    uint32_t id = addNotif(replaces_id, summary, body, timeout);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL) return;
    DBusMessageIter riter;
    dbus_message_iter_init_append(reply, &riter);
    appendUint32(&riter, id);
    (void)dbus_connection_send(g_conn, reply, NULL);
    dbus_message_unref(reply);
    (void)app_name;
    (void)app_icon;
}

static void handleGetCapabilities(DBusMessage *msg) {
    sendReply(msg, replyCapabilities);
}

static void handleGetServerInformation(DBusMessage *msg) {
    sendReply(msg, replyServerInfo);
}

static void handleCloseNotification(DBusMessage *msg) {
    DBusMessageIter iter;
    (void)dbus_message_iter_init(msg, &iter);
    uint32_t id = 0;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
        sendReply(msg, replyVoid);
        return;
    }
    dbus_message_iter_get_basic(&iter, &id);
    for (int i = 0; i < MAX_NOTIFS; i++) {
        Notif *n = &g_notifs[i];
        if (n->active && n->id == id) {
            dropNotif((size_t)i);
            break;
        }
    }
    sendReply(msg, replyVoid);
}

static dbus_bool_t messageFilter(
    DBusConnection *conn,
    DBusMessage *msg,
    void *user_data)
{
    (void)conn;
    (void)user_data;

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *interface = dbus_message_get_interface(msg);
    if (interface == NULL || strcmp(interface, DBUS_IFACE) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *member = dbus_message_get_member(msg);
    if (member == NULL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *m = member;

    if (strcmp(m, "Notify") == 0) {
        handleNotify(msg);
    } else if (strcmp(m, "GetCapabilities") == 0) {
        handleGetCapabilities(msg);
    } else if (strcmp(m, "GetServerInformation") == 0) {
        handleGetServerInformation(msg);
    } else if (strcmp(m, "CloseNotification") == 0) {
        handleCloseNotification(msg);
    } else {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

// ----------------------------------------------------------------------

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // DBusError would only buy detailed diagnostics; every call here
    // accepts a NULL error pointer, so keep it simple and report
    // failure generically.
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, NULL);
    if (conn == NULL) {
        fprintf(stderr, "railnotify: session bus unavailable\n");
        return 0;
    }
    g_conn = conn;

    (void)dbus_connection_set_exit_on_disconnect(conn, 0);

    int req = dbus_bus_request_name(conn, DBUS_SERVICE, DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
    if (req != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "railnotify: cannot own %s (is another daemon running?)\n", DBUS_SERVICE);
        return 0;
    }

    (void)dbus_connection_add_filter(conn, messageFilter, NULL, NULL);

    Display *disp = XOpenDisplay(NULL);
    if (disp == NULL) {
        fprintf(stderr, "railnotify: cannot open X display\n");
        return 0;
    }
    g_disp = disp;
    g_screen = XDefaultScreen(disp);
    g_root = XRootWindow(disp, g_screen);
    g_screen_w = XDisplayWidth(disp, g_screen);

    int dbus_fd = 0;
    (void)dbus_connection_get_unix_fd(conn, &dbus_fd);
    int xfd = XConnectionNumber(disp);

    for (;;) {
        expirePass();

        // Dispatch anything dbus already buffered before we poll.
        if (dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS) {
            (void)dbus_connection_dispatch(conn);
            (void)dbus_connection_flush(conn);
        }

        struct pollfd pfd[2];
        pfd[0].fd = xfd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = dbus_fd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        (void)poll(pfd, 2, 200);

        if ((pfd[1].revents & POLLIN) != 0) {
            // ReadWriteDispatch returns "connection alive", not "data
            // processed", so call it exactly once (non-blocking) and then
            // drain whatever it queued. A `while (...) {}` loop here would
            // spin forever and starve the X event loop below.
            (void)dbus_connection_read_write_dispatch(conn, 0);
            while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS) {}
            (void)dbus_connection_flush(conn);
        }

        while (XPending(disp) > 0) {
            XEvent ev;
            (void)XNextEvent(disp, &ev);
            if (ev.type == Expose) {
                for (int i = 0; i < MAX_NOTIFS; i++) {
                    Notif *n = &g_notifs[i];
                    if (n->active && n->win == ev.xexpose.window) drawNotif(n);
                }
            }
        }
    }
}
