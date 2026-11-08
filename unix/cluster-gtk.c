/*
 * cluster-gtk.c: "cluster SSH" mode for GTK PuTTY, built as cputty.
 *
 * In the style of the classic cssh/ctel tools, cputty opens one
 * terminal window per host named on the command line, plus a small
 * console window. Anything typed into the console is sent to every
 * session that is currently enabled in the console's host list.
 * Typing directly into an individual terminal window still affects
 * only that session.
 *
 * All sessions run in this one process. Each key pressed in the
 * console is handed to every enabled session's own key handler in
 * window.c, so that per-session terminal state (application cursor
 * keys, keypad mode, Backspace/Delete configuration and so on) is
 * honoured in each session separately.
 *
 * The console also lists host groups (see cluster-groups.h): clicking
 * a group opens a session to each of its hosts.
 *
 * The rest of the GTK front end is shared with ordinary Unix PuTTY:
 * this file is linked together with main-gtk-simple.c compiled with
 * GTK_CLUSTER defined, which provides command-line parsing and hands
 * control to cluster_main().
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#ifndef NOT_X_WINDOWS
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"
#include "gtkcompat.h"
#include "gtkmisc.h"
#include "cluster-groups.h"
#include "cluster-layout.h"

#if !GTK_CHECK_VERSION(2,18,0)
#error cputty requires GTK 2.18 or later
#endif

typedef struct ClusterSession {
    GtkFrontend *frontend;
    GtkWidget *window;          /* the session's terminal window */
    GtkWidget *check;           /* its enable/disable box in the console */
    char *hostspec;             /* how to open it again (Save as group) */
} ClusterSession;

/* Sessions currently open, in the order they were created */
static ClusterSession **sessions;
static size_t nsessions, sessions_size;

/* Positions (in the CmdlineArgList) of host arguments on the command line */
static size_t *host_arg_indices;
static size_t nhost_args, host_args_size;

/*
 * Copies of argv[0] and the command line's options (everything except
 * the host arguments). Each host's command line is built from these
 * plus the host, so options apply to every host, including the hosts
 * of groups opened later from the console.
 */
static char **option_words;
static size_t noption_words, option_words_size;

/* main-gtk-simple.c's -geometry string, set by each command-line pass */
static const char *const *cmdline_geometry_ptr;

/*
 * Host spec of the session window about to be created, which is also
 * its label in the console. Set immediately before calling
 * new_session_window(), which calls back into
 * make_gtk_toplevel_window() synchronously.
 */
static const char *pending_session_hostspec;

static GtkWidget *console_window, *console_host_box, *console_status;
static GtkWidget *console_echo;        /* shows what has been typed */
static GtkWidget *console_close_dialog; /* open close-confirmation box */
static GtkWidget *console_groups_box;  /* one button per host group */
static GtkWidget *save_group_dialog;   /* open "Save as group" box */
static GtkWidget *group_menu;          /* a group button's context menu */
static guint echo_idle_timer;          /* source id, or 0 if none */

/* Host groups as last read from the file, and the file's text then
 * (NULL if there was no file), to tell when it has changed */
static ClusterGroups *groups;
static char *groups_text;

/* Longest text kept in the echo field; older text scrolls off the
 * front. Keeps it bounded in full-screen programs, where Return
 * (which clears it) may not be typed for a long time. */
#define ECHO_MAX_CHARS 200

/* Clear the echo field after this long without typing, so that text
 * typed at a password prompt doesn't stay on screen indefinitely. */
#define ECHO_IDLE_CLEAR_SECONDS 15
static GtkIMContext *console_imc;

/* Session windows cascade down and to the right by this many pixels,
 * so that each new one is visibly separate from the last */
#define CASCADE_STEP 32
static size_t cascade_count;

static void show_console_error(const char *msg);

/*
 * Message boxes from the console are modal: while one is open, typing
 * aimed at the console goes to it instead. So even if the window
 * manager doesn't give it the focus, a keypress meant for it (Return
 * to confirm, say) can't end up broadcast to the sessions.
 */
static GtkWidget *console_message_box(
    GtkWidget *parentwin, const char *title, const char *msg, int minwid,
    bool selectable, const struct message_box_buttons *buttons,
    post_dialog_fn_t after, void *afterctx)
{
    GtkWidget *box = create_message_box(parentwin, title, msg, minwid,
                                        selectable, buttons, after,
                                        afterctx);
    gtk_window_set_modal(GTK_WINDOW(box), true);
    return box;
}

/* ----------------------------------------------------------------------
 * Window placement.
 */

/* The usable area of the primary monitor (excluding panels) */
static void get_workarea(GdkRectangle *wa)
{
#if GTK_CHECK_VERSION(3,22,0)
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor)
        monitor = gdk_display_get_monitor(display, 0);
    if (monitor) {
        gdk_monitor_get_workarea(monitor, wa);
    } else {
        wa->x = wa->y = 0;
        wa->width = 1024;
        wa->height = 768;
    }
#elif GTK_CHECK_VERSION(3,4,0)
    GdkScreen *screen = gdk_screen_get_default();
    gdk_screen_get_monitor_workarea(
        screen, gdk_screen_get_primary_monitor(screen), wa);
#else
    wa->x = wa->y = 0;
    wa->width = gdk_screen_width();
    wa->height = gdk_screen_height();
#endif
}

/*
 * Geometry string for the next session window: the size from the
 * user's -geometry (if any), and a position cascading from the
 * -geometry position, or from the top left of the screen. Returns
 * NULL for no geometry at all.
 */
static char *cascade_geometry(const char *user_geometry)
{
#ifdef NOT_X_WINDOWS
    /* Without X there is no positioning, so just pass it through */
    return user_geometry ? dupstr(user_geometry) : NULL;
#else
    int x = 0, y = 0, flags = 0;
    unsigned w = 0, h = 0;
    if (user_geometry)
        flags = XParseGeometry(user_geometry, &x, &y, &w, &h);

    GdkRectangle wa;
    get_workarea(&wa);

    /* An offset from the left (top) edge, or from the right (bottom)
     * edge if the user's -geometry gave a negative position */
    bool xneg = (flags & XValue) && (flags & XNegative);
    bool yneg = (flags & YValue) && (flags & YNegative);
    int xoff = (flags & XValue) ? (xneg ? -x : x) : wa.x;
    int yoff = (flags & YValue) ? (yneg ? -y : y) : wa.y;

    /* Cascade over about half the screen, then start again a little
     * further across, so that the windows stay mostly on screen */
    int extent = (wa.width < wa.height ? wa.width : wa.height) / 2;
    size_t per_run = extent / CASCADE_STEP > 0 ? extent / CASCADE_STEP : 1;
    size_t runs = (wa.width / 2) / (3 * CASCADE_STEP);
    if (runs == 0)
        runs = 1;
    size_t step = cascade_count % per_run;
    size_t run = (cascade_count / per_run) % runs;
    cascade_count++;
    xoff += (int)(step * CASCADE_STEP + run * 3 * CASCADE_STEP);
    yoff += (int)(step * CASCADE_STEP);

    strbuf *sb = strbuf_new();
    if (flags & WidthValue)
        put_fmt(sb, "%u", w);
    if (flags & HeightValue)
        put_fmt(sb, "x%u", h);
    put_fmt(sb, "%c%d%c%d", xneg ? '-' : '+', xoff, yneg ? '-' : '+', yoff);
    return strbuf_to_str(sb);
#endif
}

/* Put the console in the bottom right corner, clear of the cascade of
 * session windows starting at the top left */
static void place_console(void)
{
    GdkRectangle wa;
    int w, h;
    get_workarea(&wa);
    gtk_window_get_size(GTK_WINDOW(console_window), &w, &h);
    gtk_window_set_gravity(GTK_WINDOW(console_window),
                           GDK_GRAVITY_SOUTH_EAST);
    gtk_window_move(GTK_WINDOW(console_window),
                    wa.x + wa.width - w, wa.y + wa.height - h);
}

/* How much a window's frame (title bar and borders) adds to its size,
 * or zero if it isn't on screen yet */
static void get_frame_extra(GtkWidget *win, int *dw, int *dh)
{
    *dw = *dh = 0;
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw)
        return;

    GdkRectangle frame;
    int w, h;
    gdk_window_get_frame_extents(gw, &frame);
#if GTK_CHECK_VERSION(2,24,0)
    w = gdk_window_get_width(gw);
    h = gdk_window_get_height(gw);
#else
    gdk_drawable_get_size(GDK_DRAWABLE(gw), &w, &h);
#endif
    if (frame.width > w)
        *dw = frame.width - w;
    if (frame.height > h)
        *dh = frame.height - h;
}

/* Smallest size the console can usefully be */
static void console_min_size(int *w, int *h)
{
    GtkRequisition req;
#if GTK_CHECK_VERSION(3,0,0)
    gtk_widget_get_preferred_size(console_window, &req, NULL);
#else
    gtk_widget_size_request(console_window, &req);
#endif
    *w = req.width;
    *h = req.height;
}

/*
 * Fit a window, frame and all, into a tile of the screen. A window
 * that can't shrink that far (the console has a minimum size) spills
 * out of the tile away from the corner it's anchored to; the console
 * is anchored to the bottom right, so it stays on screen.
 */
static void place_in_tile(GtkWidget *widget, const ClusterRect *tile,
                          int min_w, int min_h)
{
    GtkWindow *win = GTK_WINDOW(widget);
    gtk_window_unfullscreen(win);
    gtk_window_unmaximize(win);
    gtk_window_deiconify(win);

    int dw, dh;
    get_frame_extra(widget, &dw, &dh);
    int w = tile->w - dw, h = tile->h - dh;
    if (w < min_w)
        w = min_w;
    if (h < min_h)
        h = min_h;

    /*
     * The window manager interprets a position according to the
     * window's gravity, and a change of gravity isn't reliably seen in
     * time for the move, so work with the gravity the window already
     * has. With a corner gravity, that corner of the frame is put
     * where the same corner of the window (frame aside) would be.
     */
    GdkGravity g = gtk_window_get_gravity(win);
    if (g != GDK_GRAVITY_NORTH_WEST && g != GDK_GRAVITY_NORTH_EAST &&
        g != GDK_GRAVITY_SOUTH_WEST && g != GDK_GRAVITY_SOUTH_EAST) {
        gtk_window_set_gravity(win, GDK_GRAVITY_NORTH_WEST);
        g = GDK_GRAVITY_NORTH_WEST;
    }
    bool right = (g == GDK_GRAVITY_NORTH_EAST || g == GDK_GRAVITY_SOUTH_EAST);
    bool bottom = (g == GDK_GRAVITY_SOUTH_WEST || g == GDK_GRAVITY_SOUTH_EAST);

    /*
     * A position anchored at the right or bottom is worked out from
     * the window's size as it is now (the move goes to the window
     * manager before the resize), which puts the anchored corner of
     * the frame on the tile's corner. The window manager then keeps
     * that corner in place as the window takes its new size.
     */
    int cur_w, cur_h;
    gtk_window_get_size(win, &cur_w, &cur_h);
    int x = right ? tile->x + tile->w - cur_w : tile->x;
    int y = bottom ? tile->y + tile->h - cur_h : tile->y;

    gtk_window_move(win, x, y);
    gtk_window_resize(win, w, h);
}

/*
 * "Arrange windows": tile every session window and the console evenly
 * over the screen, the console last (at the bottom right).
 */
static void arrange_windows(void)
{
    GdkRectangle wa;
    get_workarea(&wa);
    int min_w, min_h;
    console_min_size(&min_w, &min_h);

    if (nsessions == 0) {
        /* Only the console: put it back in its corner, at its usual
         * size, rather than filling the screen with it */
        ClusterRect corner = { wa.x + wa.width - 1, wa.y + wa.height - 1,
                               1, 1 };
        place_in_tile(console_window, &corner, min_w, min_h);
        gtk_window_present(GTK_WINDOW(console_window));
        return;
    }

    size_t n = nsessions + 1;
    ClusterRect area = { wa.x, wa.y, wa.width, wa.height };
    ClusterRect *tiles = snewn(n, ClusterRect);
    cluster_layout_tiles(n, area, tiles);

    for (size_t i = 0; i < nsessions; i++)
        place_in_tile(sessions[i]->window, &tiles[i], 1, 1);
    place_in_tile(console_window, &tiles[nsessions], min_w, min_h);
    gtk_window_present(GTK_WINDOW(console_window));

    sfree(tiles);
}

/*
 * Arrange the windows once newly opened ones are on screen: until the
 * window manager has framed them, their frame size isn't known. Checks
 * every 100ms; waits at most a second for frames once the windows are
 * mapped (some window managers don't add any), and three in all.
 */
static guint arrange_timer;
static int arrange_ticks;

static bool window_ready(GtkWidget *win)
{
    if (!gtk_widget_get_mapped(win))
        return false;
    int dw, dh;
    get_frame_extra(win, &dw, &dh);
    return dw > 0 || dh > 0;
}

static gboolean arrange_when_ready(gpointer data)
{
    bool mapped = gtk_widget_get_mapped(console_window);
    bool framed = window_ready(console_window);
    for (size_t i = 0; i < nsessions; i++) {
        if (!gtk_widget_get_mapped(sessions[i]->window))
            mapped = false;
        if (!window_ready(sessions[i]->window))
            framed = false;
    }

    arrange_ticks++;
    if ((mapped && (framed || arrange_ticks >= 10)) || arrange_ticks >= 30) {
        arrange_timer = 0;
        arrange_windows();
        return false;
    }
    return true;
}

static void arrange_soon(void)
{
    arrange_ticks = 0;
    if (!arrange_timer)
        arrange_timer = g_timeout_add(100, arrange_when_ready, NULL);
}

/* ----------------------------------------------------------------------
 * Session list management.
 */

static bool session_enabled(ClusterSession *sess)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sess->check));
}

static size_t count_enabled_sessions(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nsessions; i++)
        if (session_enabled(sessions[i]))
            n++;
    return n;
}

static void update_console_status(void)
{
    if (!console_window)
        return;

    char *status = dupprintf("Sending input to %zu of %zu session%s",
                             count_enabled_sessions(), nsessions,
                             nsessions == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(console_status), status);
    sfree(status);

    char *title = dupprintf("%s console (%zu/%zu active)", appname,
                            count_enabled_sessions(), nsessions);
    gtk_window_set_title(GTK_WINDOW(console_window), title);
    sfree(title);

    /* Grey out the echo field when typing would go nowhere */
    gtk_widget_set_sensitive(console_echo, count_enabled_sessions() > 0);
}

static bool session_still_open(ClusterSession *sess)
{
    for (size_t i = 0; i < nsessions; i++)
        if (sessions[i] == sess)
            return true;
    return false;
}

static void session_toggled(GtkToggleButton *button, gpointer data)
{
    update_console_status();
}

static void session_window_destroyed(GtkWidget *widget, gpointer data)
{
    ClusterSession *sess = (ClusterSession *)data;

    for (size_t i = 0; i < nsessions; i++) {
        if (sessions[i] == sess) {
            memmove(sessions + i, sessions + i + 1,
                    (nsessions - i - 1) * sizeof(*sessions));
            nsessions--;
            break;
        }
    }

    if (sess->check)
        gtk_widget_destroy(sess->check);
    sfree(sess->hostspec);
    sfree(sess);
    update_console_status();
}

/*
 * Called by window.c to create each session's toplevel window. We use
 * the opportunity to register the session with the console.
 */
GtkWidget *make_gtk_toplevel_window(GtkFrontend *frontend)
{
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    ClusterSession *sess = snew(ClusterSession);
    sess->frontend = frontend;
    sess->window = win;
    sess->hostspec = (pending_session_hostspec && *pending_session_hostspec ?
                      dupstr(pending_session_hostspec) : NULL);

    sess->check = gtk_check_button_new_with_label(
        sess->hostspec ? sess->hostspec : "(unnamed session)");

    /* The console takes all keystrokes itself, so its controls are
     * operated with the mouse only and never take keyboard focus. */
    gtk_widget_set_can_focus(sess->check, false);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sess->check), true);
    g_signal_connect(G_OBJECT(sess->check), "toggled",
                     G_CALLBACK(session_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(console_host_box), sess->check,
                       false, false, 0);
    gtk_widget_show(sess->check);

    /*
     * Connected before window.c connects its own "destroy" handler,
     * so this runs first and the session stops receiving broadcast
     * input before its GtkFrontend is freed.
     */
    g_signal_connect(G_OBJECT(win), "destroy",
                     G_CALLBACK(session_window_destroyed), sess);

    sgrowarray(sessions, sessions_size, nsessions);
    sessions[nsessions++] = sess;
    update_console_status();

    return win;
}

/*
 * Called by window.c when a session window has closed. The console
 * stays open even with no sessions left, so that more can be opened
 * from it; cputty exits when the console is closed.
 */
void session_window_closed(void)
{
}

/* How to open the session described by a Conf again, as a host spec */
static char *host_spec_from_conf(Conf *conf)
{
    const struct BackendVtable *vt =
        backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
    return cluster_host_spec_make(
        conf_get_str_ambi(conf, CONF_username, NULL),
        conf_get_str(conf, CONF_host), conf_get_int(conf, CONF_port),
        vt && vt->default_port ? vt->default_port : 22);
}

/*
 * Open a session window for 'conf' (taking ownership of it). hostspec
 * is how to open the host again, or NULL to work it out from the Conf.
 */
static void open_session_window(Conf *conf, const char *hostspec,
                                const char *user_geometry)
{
    char *spec = hostspec ? dupstr(hostspec) : host_spec_from_conf(conf);
    char *geometry = cascade_geometry(user_geometry);

    pending_session_hostspec = spec;
    new_session_window(conf, geometry);
    pending_session_hostspec = NULL;

    sfree(geometry);
    sfree(spec);
}

/* ----------------------------------------------------------------------
 * Launching sessions, both at startup and from the session windows'
 * context menus (New Session, Duplicate Session and Saved Sessions). In
 * cputty these all open in-process, so the new window joins the
 * cluster console.
 */

struct config_box_ctx {
    Conf *conf;
    char *geometry_string;
};

static void post_config_box(void *vctx, int result)
{
    struct config_box_ctx *ctx = (struct config_box_ctx *)vctx;

    if (result > 0)
        open_session_window(ctx->conf, NULL, ctx->geometry_string);
    else
        conf_free(ctx->conf);
    sfree(ctx->geometry_string);
    sfree(ctx);
}

static void launch_via_config_box(Conf *conf, const char *geometry_string)
{
    struct config_box_ctx *ctx = snew(struct config_box_ctx);
    ctx->conf = conf;
    ctx->geometry_string = geometry_string ? dupstr(geometry_string) : NULL;
    initial_config_box(conf, post_config_box, ctx);
}

void launch_duplicate_session(Conf *conf)
{
    assert(conf_launchable(conf));
    open_session_window(conf_copy(conf), NULL, NULL);
}

void launch_saved_session(const char *str)
{
    Conf *conf = conf_new();
    do_defaults(str, conf);

    /* As in ordinary PuTTY, New Session (str == NULL) always offers the
     * configuration box, even if Default Settings name a host. A saved
     * session is remembered by its name, if that can go in a group. */
    if (str && conf_launchable(conf))
        open_session_window(conf, cluster_host_spec_valid(str) ? str : NULL,
                            NULL);
    else
        launch_via_config_box(conf, NULL);
}

void launch_new_session(void)
{
    launch_saved_session(NULL);
}

/* ----------------------------------------------------------------------
 * Command-line handling. main-gtk-simple.c makes an initial scanning
 * pass over the command line, during which it calls
 * cluster_note_host_arg() for each non-option argument. We then build
 * a separate command line per host, consisting of all the options
 * followed by that one host, and parse it exactly as Unix PuTTY would
 * parse 'putty [options] host'. So user@host, -load, -l, -P, -i and
 * so on all behave as they do in PuTTY.
 */

void cluster_note_host_arg(size_t arglist_index)
{
    sgrowarray(host_arg_indices, host_args_size, nhost_args);
    host_arg_indices[nhost_args++] = arglist_index;
}

static bool argv_index_is_host(int argv_index)
{
    /* CmdlineArgList index i corresponds to argv[i+1] */
    for (size_t i = 0; i < nhost_args; i++)
        if (host_arg_indices[i] + 1 == (size_t)argv_index)
            return true;
    return false;
}

static void save_option_words(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (i > 0 && argv_index_is_host(i))
            continue;
        sgrowarray(option_words, option_words_size, noption_words);
        option_words[noption_words++] = dupstr(argv[i]);
    }
}

/*
 * Once the command line's own hosts are open, cmdline.c holds the -pw
 * password, and keeps it for further sessions. So drop -pw from the
 * options used for groups opened later, wiping our copy of it.
 */
static void forget_password_option(void)
{
    size_t out = 0;
    for (size_t i = 0; i < noption_words; i++) {
        if (i > 0 && !strcmp(option_words[i], "-pw") &&
            i + 1 < noption_words) {
            sfree(option_words[i]);
            smemclr(option_words[i + 1], strlen(option_words[i + 1]));
            sfree(option_words[i + 1]);
            i++;
            continue;
        }
        option_words[out++] = option_words[i];
    }
    noption_words = out;
}

/*
 * Build the Conf for one host (or, if host is NULL, for the options
 * alone). Sets *ok to false if the command line was bad; otherwise
 * returns whether the result can be launched without a config box.
 */
static bool conf_for_host(const char *host, Conf *conf, bool *ok)
{
    /*
     * Use private copies of the words: cmdline.c overwrites the -pw
     * argument in place after reading it.
     */
    char **hostargv = snewn(noption_words + 2, char *);
    int hostargc = 0;
    for (size_t i = 0; i < noption_words; i++)
        hostargv[hostargc++] = dupstr(option_words[i]);
    if (host)
        hostargv[hostargc++] = dupstr(host);
    hostargv[hostargc] = NULL;

    cmdline_reset_host_state();
    cluster_per_host_pass = true;
    bool err = do_cmdline(hostargc, hostargv, false, conf);
    if (!err) {
        do_defaults(NULL, conf);
        err = do_cmdline(hostargc, hostargv, true, conf);
    }
    cluster_per_host_pass = false;

    /*
     * hostargv is deliberately never freed: some options keep
     * pointers into its words for the life of the process (-geometry
     * and -name in main-gtk-simple.c), just as they point into the
     * real argv in ordinary PuTTY.
     */
    if (err) {
        *ok = false;
        return false;
    }
    *ok = true;
    cmdline_run_saved(conf);
    return cmdline_host_ok(conf);
}

/*
 * Open a host given as a host spec: a command-line host argument or a
 * group member, optionally with :port. Returns false if its command
 * line was bad.
 */
static bool launch_host_spec(const char *spec)
{
    char *host;
    int port;
    cluster_host_spec_split(spec, &host, &port);

    Conf *conf = conf_new();
    bool ok;
    bool launchable = conf_for_host(host, conf, &ok);
    sfree(host);
    if (!ok) {
        conf_free(conf);
        return false;
    }
    if (port > 0)
        conf_set_int(conf, CONF_port, port);

    /* Read the geometry only now: -geometry is parsed by the per-host
     * pass just made, not by the initial scan. */
    const char *geometry = *cmdline_geometry_ptr;

    if (launchable)
        open_session_window(conf, spec, geometry);
    else
        launch_via_config_box(conf, geometry);
    return true;
}

static void wipe_password_args(int argc, char **argv)
{
    /*
     * Our per-host parsing used copies (see conf_for_host), so do for
     * the real argv what PuTTY would have done, to keep the password
     * out of ps(1) output as far as is possible.
     */
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-pw"))
            smemclr(argv[i+1], strlen(argv[i+1]));
}

/* ----------------------------------------------------------------------
 * Host groups.
 */

static void free_string(gpointer p)
{
    sfree(p);
}

static void free_host_list(char **hosts, size_t nhosts)
{
    for (size_t i = 0; i < nhosts; i++)
        sfree(hosts[i]);
    sfree(hosts);
}

static void group_button_clicked(GtkButton *button, gpointer data);
static gboolean group_button_pressed(GtkWidget *button, GdkEventButton *event,
                                     gpointer data);

static void rebuild_group_buttons(void)
{
    GList *children = gtk_container_get_children(
        GTK_CONTAINER(console_groups_box));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    if (groups->ngroups == 0) {
        GtkWidget *label = gtk_label_new(
            "No groups yet. Tick some hosts and\n"
            "press \"Save as group\" to make one.");
        align_label_left(GTK_LABEL(label));
        gtk_box_pack_start(GTK_BOX(console_groups_box), label,
                           false, false, 0);
        gtk_widget_show(label);
        return;
    }

    for (size_t i = 0; i < groups->ngroups; i++) {
        const ClusterGroup *g = &groups->groups[i];

        char *text = dupprintf("%s (%zu host%s)", g->name, g->nhosts,
                               g->nhosts == 1 ? "" : "s");
        GtkWidget *button = gtk_button_new_with_label(text);
        sfree(text);

        strbuf *tip = strbuf_new();
        for (size_t j = 0; j < g->nhosts; j++)
            put_fmt(tip, "%s%s", j ? " " : "", g->hosts[j]);
        if (g->arrange)
            put_dataz(tip, "\nOpening it arranges the windows.");
        put_dataz(tip, "\nClick to open all; right-click to delete.");
        gtk_widget_set_tooltip_text(button, tip->s);
        strbuf_free(tip);

        gtk_widget_set_can_focus(button, false);
        g_object_set_data_full(G_OBJECT(button), "cputty-group",
                               dupstr(g->name), free_string);
        g_signal_connect(G_OBJECT(button), "clicked",
                         G_CALLBACK(group_button_clicked), NULL);
        g_signal_connect(G_OBJECT(button), "button_press_event",
                         G_CALLBACK(group_button_pressed), NULL);
        gtk_box_pack_start(GTK_BOX(console_groups_box), button,
                           false, false, 0);
        gtk_widget_show(button);
    }
}

/*
 * (Re)read the groups file, updating the console if it has changed.
 * If it can't be read, the groups shown stay as they were.
 */
static void load_groups(bool report_errors)
{
    char *err;
    char *text = cluster_groups_read(&err);
    if (err) {
        if (report_errors)
            show_console_error(err);
        sfree(err);
        if (groups)
            return;
    }

    if (groups && (text && groups_text ? !strcmp(text, groups_text) :
                   text == groups_text)) {
        sfree(text);                   /* unchanged */
        return;
    }

    cluster_groups_free(groups);
    groups = cluster_groups_parse(text);
    sfree(groups_text);
    groups_text = text;
    rebuild_group_buttons();
}

/*
 * Open every host in a group, returning how many had a bad command
 * line. Works from a copy of the host list, which stays valid even if
 * the groups are reloaded meanwhile.
 */
static size_t launch_group_hosts(const ClusterGroup *g)
{
    size_t n = g->nhosts, bad = 0;
    char **hosts = snewn(n, char *);
    for (size_t i = 0; i < n; i++)
        hosts[i] = dupstr(g->hosts[i]);
    for (size_t i = 0; i < n; i++)
        if (!launch_host_spec(hosts[i]))
            bad++;
    free_host_list(hosts, n);
    return bad;
}

static void open_group(const char *name)
{
    load_groups(true);                 /* pick up any edits */
    const ClusterGroup *g = cluster_groups_find(groups, name);
    if (!g) {
        char *msg = dupprintf("There is no longer a group called \"%s\".",
                              name);
        show_console_error(msg);
        sfree(msg);
        return;
    }

    bool arrange = g->arrange;
    size_t bad = launch_group_hosts(g);
    if (arrange)
        arrange_soon();
    if (bad) {
        char *msg = dupprintf("%zu host%s in group \"%s\" could not be "
                              "opened.", bad, bad == 1 ? "" : "s", name);
        show_console_error(msg);
        sfree(msg);
    }

    /* Bring the console back in front of the new windows, since it's
     * where you type to them */
    gtk_window_present(GTK_WINDOW(console_window));
}

static void group_button_clicked(GtkButton *button, gpointer data)
{
    /* Copy the name: reloading the groups may destroy this button */
    char *name = dupstr(g_object_get_data(G_OBJECT(button), "cputty-group"));
    open_group(name);
    sfree(name);
}

static void group_menu_open(GtkMenuItem *item, gpointer data)
{
    char *name = dupstr(g_object_get_data(G_OBJECT(item), "cputty-group"));
    open_group(name);
    sfree(name);
}

static void delete_group_confirmed(void *vctx, int result)
{
    char *name = (char *)vctx;
    if (result > 0) {
        char *err;
        if (!cluster_groups_store(name, NULL, 0, false, &err)) {
            show_console_error(err);
            sfree(err);
        }
        load_groups(true);
    }
    sfree(name);
}

static void group_menu_delete(GtkMenuItem *item, gpointer data)
{
    const char *name = g_object_get_data(G_OBJECT(item), "cputty-group");
    char *title = dupcat(appname, " Delete Group");
    char *msg = dupprintf("Delete the group \"%s\"?\n"
                          "Its open sessions are not affected.", name);
    console_message_box(console_window, title, msg,
                       string_width("Its open sessions are not affected."),
                       false, &buttons_yn, delete_group_confirmed,
                       dupstr(name));
    sfree(title);
    sfree(msg);
}

static GtkWidget *group_menu_item(const char *label, const char *name,
                                  GCallback cb)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(item), "cputty-group", dupstr(name),
                           free_string);
    g_signal_connect(G_OBJECT(item), "activate", cb, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(group_menu), item);
    return item;
}

static gboolean group_button_pressed(GtkWidget *button, GdkEventButton *event,
                                     gpointer data)
{
    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return false;

    const char *name = g_object_get_data(G_OBJECT(button), "cputty-group");

    /* One context menu at a time, replaced each time it's needed */
    if (group_menu)
        gtk_widget_destroy(group_menu);
    group_menu = gtk_menu_new();
    group_menu_item("Open all hosts", name, G_CALLBACK(group_menu_open));
    char *label = dupprintf("Delete group \"%s\"...", name);
    group_menu_item(label, name, G_CALLBACK(group_menu_delete));
    sfree(label);
    gtk_widget_show_all(group_menu);

#if GTK_CHECK_VERSION(3,22,0)
    gtk_menu_popup_at_pointer(GTK_MENU(group_menu), (GdkEvent *)event);
#else
    gtk_menu_popup(GTK_MENU(group_menu), NULL, NULL, NULL, NULL,
                   event->button, event->time);
#endif
    return true;
}

/*
 * "Save as group": stores the ticked sessions' hosts under a name.
 */
struct save_group_ctx {
    char **hosts;
    size_t nhosts, hostsize;
    GtkWidget *entry, *arrange_check;
};

static bool do_save_group(const char *name, char *const *hosts, size_t n,
                          bool arrange)
{
    char *err;
    if (!cluster_groups_store(name, hosts, n, arrange, &err)) {
        show_console_error(err);
        sfree(err);
        return false;
    }
    load_groups(true);
    return true;
}

struct replace_group_ctx {
    char *name;
    char **hosts;
    size_t nhosts;
    bool arrange;
};

static void replace_group_confirmed(void *vctx, int result)
{
    struct replace_group_ctx *rctx = (struct replace_group_ctx *)vctx;
    if (result > 0 && do_save_group(rctx->name, rctx->hosts, rctx->nhosts,
                                    rctx->arrange) && save_group_dialog)
        gtk_widget_destroy(save_group_dialog);
    sfree(rctx->name);
    free_host_list(rctx->hosts, rctx->nhosts);
    sfree(rctx);
}

static void save_group_response(GtkDialog *dialog, gint response,
                                gpointer vctx)
{
    struct save_group_ctx *ctx = (struct save_group_ctx *)vctx;

    if (response != GTK_RESPONSE_OK) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }

    char *name = cluster_group_name_trim(
        gtk_entry_get_text(GTK_ENTRY(ctx->entry)));
    bool arrange = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(ctx->arrange_check));
    if (!cluster_group_name_valid(name)) {
        show_console_error("A group name can't be empty, contain spaces, "
                           "or start with \"-\" or \"#\".");
        sfree(name);
        return;                        /* leave the dialog open */
    }

    if (cluster_groups_find(groups, name)) {
        /* Confirm first, with its own copy of everything it needs, in
         * case this dialog goes away in the meantime */
        struct replace_group_ctx *rctx = snew(struct replace_group_ctx);
        rctx->name = name;
        rctx->arrange = arrange;
        rctx->nhosts = ctx->nhosts;
        rctx->hosts = snewn(ctx->nhosts, char *);
        for (size_t i = 0; i < ctx->nhosts; i++)
            rctx->hosts[i] = dupstr(ctx->hosts[i]);

        char *title = dupcat(appname, " Replace Group");
        char *msg = dupprintf("There is already a group called \"%s\".\n"
                              "Replace it?", name);
        console_message_box(GTK_WIDGET(dialog), title, msg,
                           string_width("There is already a group called "
                                        "\"something\"."),
                           false, &buttons_yn, replace_group_confirmed, rctx);
        sfree(title);
        sfree(msg);
        return;
    }

    if (do_save_group(name, ctx->hosts, ctx->nhosts, arrange))
        gtk_widget_destroy(GTK_WIDGET(dialog));
    sfree(name);
}

static void save_group_destroyed(GtkWidget *widget, gpointer vctx)
{
    struct save_group_ctx *ctx = (struct save_group_ctx *)vctx;
    free_host_list(ctx->hosts, ctx->nhosts);
    sfree(ctx);
    save_group_dialog = NULL;
}

static void save_group_clicked(GtkButton *button, gpointer data)
{
    if (save_group_dialog) {
        gtk_window_present(GTK_WINDOW(save_group_dialog));
        return;
    }

    struct save_group_ctx *ctx = snew(struct save_group_ctx);
    memset(ctx, 0, sizeof(*ctx));
    size_t unsaveable = 0;
    for (size_t i = 0; i < nsessions; i++) {
        const char *spec = sessions[i]->hostspec;
        if (!session_enabled(sessions[i]))
            continue;
        if (!spec || !cluster_host_spec_valid(spec)) {
            unsaveable++;
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < ctx->nhosts; j++)
            if (!strcmp(ctx->hosts[j], spec))
                dup = true;
        if (!dup) {
            sgrowarray(ctx->hosts, ctx->hostsize, ctx->nhosts);
            ctx->hosts[ctx->nhosts++] = dupstr(spec);
        }
    }

    if (ctx->nhosts == 0) {
        free_host_list(ctx->hosts, ctx->nhosts);
        sfree(ctx);
        show_console_error("There are no ticked hosts to save. Tick the "
                           "hosts you want in the group first.");
        return;
    }

    /*
     * Modal, so that while it's open, typing aimed at the console goes
     * to this dialog instead: even if the window manager doesn't give
     * the dialog the focus, the group name can't end up broadcast to
     * the sessions.
     */
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Save as group", GTK_WINDOW(console_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_set_spacing(GTK_BOX(content), 6);

    char *intro = dupprintf("Save the %zu ticked host%s as a group named:",
                            ctx->nhosts, ctx->nhosts == 1 ? "" : "s");
    GtkWidget *label = gtk_label_new(intro);
    sfree(intro);
    align_label_left(GTK_LABEL(label));
    gtk_box_pack_start(GTK_BOX(content), label, false, false, 0);

    ctx->entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(ctx->entry), 100);
    gtk_entry_set_activates_default(GTK_ENTRY(ctx->entry), true);
    gtk_box_pack_start(GTK_BOX(content), ctx->entry, false, false, 0);

    strbuf *list = strbuf_new();
    for (size_t i = 0; i < ctx->nhosts; i++)
        put_fmt(list, "%s%s", i ? " " : "", ctx->hosts[i]);
    if (unsaveable)
        put_fmt(list, "\n(%zu ticked session%s can't be saved: a host "
                "name with spaces in it can't go in a group.)", unsaveable,
                unsaveable == 1 ? "" : "s");
    GtkWidget *hosts = gtk_label_new(list->s);
    strbuf_free(list);
    align_label_left(GTK_LABEL(hosts));
    gtk_label_set_line_wrap(GTK_LABEL(hosts), true);
    gtk_box_pack_start(GTK_BOX(content), hosts, false, false, 0);

    ctx->arrange_check = gtk_check_button_new_with_label(
        "Arrange the windows when this group is opened");
    gtk_box_pack_start(GTK_BOX(content), ctx->arrange_check, false, false, 0);

    g_signal_connect(G_OBJECT(dialog), "response",
                     G_CALLBACK(save_group_response), ctx);
    g_signal_connect(G_OBJECT(dialog), "destroy",
                     G_CALLBACK(save_group_destroyed), ctx);
    save_group_dialog = dialog;
    gtk_widget_show_all(dialog);

    /* Ask for the focus using the click's timestamp, which window
     * managers' focus stealing prevention accepts, so that the name
     * can be typed straight away (rather than going to the console) */
    gtk_widget_grab_focus(ctx->entry);
    gtk_window_present_with_time(GTK_WINDOW(dialog),
                                 gtk_get_current_event_time());
}

static void new_session_clicked(GtkButton *button, gpointer data)
{
    launch_new_session();
}

static void arrange_windows_clicked(GtkButton *button, gpointer data)
{
    arrange_windows();
}

/* ----------------------------------------------------------------------
 * The console window.
 */

static void console_message_done(void *vctx, int result)
{
}

static void show_console_error(const char *msg)
{
    char *title = dupcat(appname, " Error");
    console_message_box(console_window, title, msg,
                       string_width("There are no ticked hosts to save."),
                       true, &buttons_ok, console_message_done, NULL);
    sfree(title);
}

/*
 * The echo field. Keystrokes are sent to the sessions as they are
 * typed (so that Tab completion, full-screen programs and so on work),
 * so this is only a local reflection of the current input line: text
 * is appended as typed, BackSpace removes the last character, and
 * Return (or ^C / ^U, which discard a shell's input line) clears it.
 * It can't know what the remote ends do with the input, e.g. Tab
 * completion or editing with arrow keys.
 */
static void echo_clear(void)
{
    gtk_entry_set_text(GTK_ENTRY(console_echo), "");
}

static gboolean echo_idle_timeout(gpointer data)
{
    echo_idle_timer = 0;
    echo_clear();
    return false;                      /* one-shot */
}

/* Called on every keystroke typed in the console */
static void echo_restart_idle_timer(void)
{
    if (echo_idle_timer)
        g_source_remove(echo_idle_timer);
    echo_idle_timer = g_timeout_add_seconds(ECHO_IDLE_CLEAR_SECONDS,
                                            echo_idle_timeout, NULL);
}

static void echo_append(const char *utf8)
{
    GtkEditable *ed = GTK_EDITABLE(console_echo);
    gint pos = gtk_entry_get_text_length(GTK_ENTRY(console_echo));
    gtk_editable_insert_text(ed, utf8, -1, &pos);

    /* Positions here are in characters, not bytes, so this is UTF-8 safe */
    gint len = gtk_entry_get_text_length(GTK_ENTRY(console_echo));
    if (len > ECHO_MAX_CHARS)
        gtk_editable_delete_text(ed, 0, len - ECHO_MAX_CHARS);

    gtk_editable_set_position(ed, -1);
}

static void echo_backspace(void)
{
    gint len = gtk_entry_get_text_length(GTK_ENTRY(console_echo));
    if (len > 0)
        gtk_editable_delete_text(GTK_EDITABLE(console_echo), len - 1, len);
}

static void echo_key_event(GdkEventKey *event)
{
    if (event->type != GDK_KEY_PRESS)
        return;

    switch (event->keyval) {
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
      case GDK_KEY_Escape:             /* e.g. leaving vi insert mode */
        echo_clear();
        return;
      case GDK_KEY_BackSpace:
        echo_backspace();
        return;
    }

    if ((event->state & GDK_CONTROL_MASK) &&
        (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C ||
         event->keyval == GDK_KEY_u || event->keyval == GDK_KEY_U))
        echo_clear();
}

static void hide_echo_toggled(GtkToggleButton *button, gpointer data)
{
    gtk_entry_set_visibility(GTK_ENTRY(console_echo),
                             !gtk_toggle_button_get_active(button));
}

static void broadcast_key_event(GdkEventKey *event)
{
    /*
     * Work from a snapshot, re-checking each session is still open
     * before using it, in case delivering input to one session causes
     * another's window to be destroyed.
     */
    size_t n = nsessions;
    ClusterSession **targets = snewn(n ? n : 1, ClusterSession *);
    memcpy(targets, sessions, n * sizeof(*targets));

    for (size_t i = 0; i < n; i++)
        if (session_still_open(targets[i]) && session_enabled(targets[i]))
            gtk_frontend_inject_key_event(targets[i]->frontend, event);

    sfree(targets);
}

static void console_im_commit(GtkIMContext *imc, gchar *str, gpointer data)
{
    /* Only echo what is actually being sent somewhere */
    if (count_enabled_sessions() > 0)
        echo_append(str);

    size_t n = nsessions;
    ClusterSession **targets = snewn(n ? n : 1, ClusterSession *);
    memcpy(targets, sessions, n * sizeof(*targets));

    for (size_t i = 0; i < n; i++)
        if (session_still_open(targets[i]) && session_enabled(targets[i]))
            gtk_frontend_inject_text(targets[i]->frontend, str);

    sfree(targets);
}

static gboolean console_key_event(GtkWidget *widget, GdkEventKey *event,
                                  gpointer data)
{
    /*
     * Ordinary text goes through the console's own input method once
     * (so compose and dead keys work) and is then broadcast as text
     * by console_im_commit. Everything the input method doesn't want
     * (Return, Tab, arrows, Ctrl and Alt combinations, function keys)
     * is passed to each session's key handler for it to translate.
     *
     * We always claim the event, so that Tab, Space etc. never
     * operate the console's own controls.
     */
    if (event->type == GDK_KEY_PRESS)
        echo_restart_idle_timer();
    if (!gtk_im_context_filter_keypress(console_imc, event)) {
        if (count_enabled_sessions() > 0)
            echo_key_event(event);
        broadcast_key_event(event);
    }
    return true;
}

static gboolean console_focus_event(GtkWidget *widget, GdkEventFocus *event,
                                    gpointer data)
{
    if (event->in) {
        gtk_im_context_focus_in(console_imc);
        load_groups(false);            /* show any edits to the file */
    } else {
        gtk_im_context_focus_out(console_imc);
    }
    return false;
}

static void console_realised(GtkWidget *widget, gpointer data)
{
    gtk_im_context_set_client_window(console_imc,
                                     gtk_widget_get_window(widget));
}

static size_t count_connected_sessions(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nsessions; i++)
        if (gtk_frontend_is_connected(sessions[i]->frontend))
            n++;
    return n;
}

static void console_close_confirmed(void *vctx, int result)
{
    console_close_dialog = NULL;
    if (result > 0)
        gtk_main_quit();
}

static gboolean console_delete(GtkWidget *widget, GdkEvent *event,
                               gpointer data)
{
    /*
     * As in cssh, closing the console ends the whole cluster. Since
     * that drops every connection at once, ask first if any session
     * is still connected, as each terminal window does for itself.
     */
    size_t connected = count_connected_sessions();
    if (connected == 0) {
        gtk_main_quit();
        return true;
    }

    if (console_close_dialog) {
        gtk_window_present(GTK_WINDOW(console_close_dialog));
        return true;
    }

    char *title = dupcat(appname, " Exit Confirmation");
    char *msg = dupprintf("Closing the console will close all sessions.\n"
                          "%zu session%s still connected. Close anyway?",
                          connected, connected == 1 ? " is" : "s are");
    console_close_dialog = console_message_box(
        console_window, title, msg,
        string_width("Closing the console will close all sessions."),
        false, &buttons_yn, console_close_confirmed, NULL);
    sfree(title);
    sfree(msg);
    return true;
}

enum SelectMode { SELECT_ALL, SELECT_NONE, SELECT_INVERT };

static void select_sessions(enum SelectMode mode)
{
    for (size_t i = 0; i < nsessions; i++) {
        GtkToggleButton *tb = GTK_TOGGLE_BUTTON(sessions[i]->check);
        bool active = (mode == SELECT_ALL ? true :
                       mode == SELECT_NONE ? false :
                       !gtk_toggle_button_get_active(tb));
        gtk_toggle_button_set_active(tb, active);
    }
}

static void select_all_clicked(GtkButton *b, gpointer d)
{ select_sessions(SELECT_ALL); }
static void select_none_clicked(GtkButton *b, gpointer d)
{ select_sessions(SELECT_NONE); }
static void select_invert_clicked(GtkButton *b, gpointer d)
{ select_sessions(SELECT_INVERT); }

static GtkWidget *make_console_button(const char *label, GCallback cb)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_can_focus(button, false);
    g_signal_connect(G_OBJECT(button), "clicked", cb, NULL);
    gtk_widget_show(button);
    return button;
}

static GtkWidget *make_scrolled_box(int height, GtkWidget **box_out)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height);

    GtkWidget *box = gtk_vbox_new(false, 0);
#if GTK_CHECK_VERSION(3,8,0)
    gtk_container_add(GTK_CONTAINER(scroll), box);
#else
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), box);
#endif
    gtk_widget_show(box);
    gtk_widget_show(scroll);
    *box_out = box;
    return scroll;
}

static void create_console_window(void)
{
    console_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_set_border_width(GTK_CONTAINER(console_window), 8);

    GtkWidget *vbox = gtk_vbox_new(false, 6);
    gtk_container_add(GTK_CONTAINER(console_window), vbox);
    gtk_widget_show(vbox);

    GtkWidget *hint = gtk_label_new(
        "Type here to send keystrokes to every ticked session.\n"
        "Type in a terminal window to talk to that session alone.");
    align_label_left(GTK_LABEL(hint));
    gtk_box_pack_start(GTK_BOX(vbox), hint, false, false, 0);
    gtk_widget_show(hint);

    /*
     * The echo field is display-only: all typing is captured by the
     * console window itself (console_key_event), so the field must
     * never take focus or handle keys of its own.
     */
    console_echo = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(console_echo), false);
    gtk_widget_set_can_focus(console_echo, false);
    gtk_entry_set_width_chars(GTK_ENTRY(console_echo), 40);
    gtk_box_pack_start(GTK_BOX(vbox), console_echo, false, false, 0);
    gtk_widget_show(console_echo);

    GtkWidget *hide_echo = gtk_check_button_new_with_label(
        "Hide typed text (e.g. for passwords)");
    gtk_widget_set_can_focus(hide_echo, false);
    g_signal_connect(G_OBJECT(hide_echo), "toggled",
                     G_CALLBACK(hide_echo_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), hide_echo, false, false, 0);
    gtk_widget_show(hide_echo);

    /* The lists are small at minimum, so the console can be tiled
     * small (Arrange windows), and grow with the window */
    gtk_box_pack_start(GTK_BOX(vbox), make_scrolled_box(96, &console_host_box),
                       true, true, 0);

    GtkWidget *buttons = gtk_hbox_new(false, 4);
    gtk_box_pack_start(GTK_BOX(buttons),
                       make_console_button("All",
                                           G_CALLBACK(select_all_clicked)),
                       true, true, 0);
    gtk_box_pack_start(GTK_BOX(buttons),
                       make_console_button("None",
                                           G_CALLBACK(select_none_clicked)),
                       true, true, 0);
    gtk_box_pack_start(GTK_BOX(buttons),
                       make_console_button("Invert",
                                           G_CALLBACK(select_invert_clicked)),
                       true, true, 0);
    gtk_box_pack_start(GTK_BOX(vbox), buttons, false, false, 0);
    gtk_widget_show(buttons);

    /* Host groups: a button per group, and a way to make one */
    GtkWidget *groups_frame = gtk_frame_new("Groups");
    gtk_box_pack_start(GTK_BOX(vbox), groups_frame, true, true, 0);
    gtk_widget_show(groups_frame);
    GtkWidget *groups_vbox = gtk_vbox_new(false, 4);
    gtk_container_set_border_width(GTK_CONTAINER(groups_vbox), 4);
    gtk_container_add(GTK_CONTAINER(groups_frame), groups_vbox);
    gtk_widget_show(groups_vbox);
    gtk_box_pack_start(GTK_BOX(groups_vbox),
                       make_scrolled_box(72, &console_groups_box),
                       true, true, 0);
    gtk_box_pack_start(GTK_BOX(groups_vbox),
                       make_console_button("Save ticked hosts as group...",
                                           G_CALLBACK(save_group_clicked)),
                       false, false, 0);

    /* Opening a session, and arranging the windows */
    GtkWidget *window_buttons = gtk_hbox_new(false, 4);
    gtk_box_pack_start(GTK_BOX(window_buttons),
                       make_console_button("New session...",
                                           G_CALLBACK(new_session_clicked)),
                       true, true, 0);
    GtkWidget *arrange = make_console_button(
        "Arrange windows", G_CALLBACK(arrange_windows_clicked));
    gtk_widget_set_tooltip_text(
        arrange, "Tile all the session windows and this console evenly "
        "over the screen");
    gtk_box_pack_start(GTK_BOX(window_buttons), arrange, true, true, 0);
    gtk_box_pack_start(GTK_BOX(vbox), window_buttons, false, false, 0);
    gtk_widget_show(window_buttons);

    console_status = gtk_label_new("");
    align_label_left(GTK_LABEL(console_status));
    gtk_box_pack_start(GTK_BOX(vbox), console_status, false, false, 0);
    gtk_widget_show(console_status);

    console_imc = gtk_im_multicontext_new();
    g_signal_connect(G_OBJECT(console_imc), "commit",
                     G_CALLBACK(console_im_commit), NULL);

    g_signal_connect(G_OBJECT(console_window), "key_press_event",
                     G_CALLBACK(console_key_event), NULL);
    g_signal_connect(G_OBJECT(console_window), "key_release_event",
                     G_CALLBACK(console_key_event), NULL);
    g_signal_connect(G_OBJECT(console_window), "focus_in_event",
                     G_CALLBACK(console_focus_event), NULL);
    g_signal_connect(G_OBJECT(console_window), "focus_out_event",
                     G_CALLBACK(console_focus_event), NULL);
    g_signal_connect(G_OBJECT(console_window), "realize",
                     G_CALLBACK(console_realised), NULL);
    g_signal_connect(G_OBJECT(console_window), "delete_event",
                     G_CALLBACK(console_delete), NULL);

    /* Open at a comfortable size, though it can be made smaller */
    gtk_window_set_default_size(GTK_WINDOW(console_window), -1, 580);

    update_console_status();
}

/* ----------------------------------------------------------------------
 * Entry point, called from main() in main-gtk-simple.c once GTK is
 * initialised and the command line has been scanned.
 */
int cluster_main(int argc, char **argv,
                 const char *const *geometry_string_ptr)
{
    cmdline_geometry_ptr = geometry_string_ptr;
    save_option_words(argc, argv);
    bool arrange_at_start = false;     /* a group with arrange=yes */

    /* Create (but don't yet show) the console, so that session
     * windows can register with it as they are made. */
    create_console_window();
    load_groups(true);

    if (nhost_args == 0) {
        /* No hosts given: open a session given with -load, if any.
         * Otherwise just show the console, where a group or a new
         * session can be opened. */
        Conf *conf = conf_new();
        bool ok;
        if (conf_for_host(NULL, conf, &ok)) {
            open_session_window(conf, NULL, *cmdline_geometry_ptr);
        } else {
            conf_free(conf);
            if (!ok)
                exit(1);
        }
    } else {
        /* Each argument is a group name or a host */
        for (size_t i = 0; i < nhost_args; i++) {
            const char *arg = argv[host_arg_indices[i] + 1];
            const ClusterGroup *g = cluster_groups_find(groups, arg);
            if (g) {
                if (g->arrange)
                    arrange_at_start = true;
                if (launch_group_hosts(g))
                    exit(1);
            } else if (!launch_host_spec(arg)) {
                exit(1);
            }
        }
    }

    forget_password_option();
    wipe_password_args(argc, argv);

    /* Show the console last, so it's on top and has the focus. */
    place_console();
    gtk_widget_show(console_window);
    gtk_window_present(GTK_WINDOW(console_window));
    if (arrange_at_start)
        arrange_soon();

    gtk_main();
    return 0;
}
