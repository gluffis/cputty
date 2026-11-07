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
 * The rest of the GTK front end is shared with ordinary Unix PuTTY:
 * this file is linked together with main-gtk-simple.c compiled with
 * GTK_CLUSTER defined, which provides command-line parsing and hands
 * control to cluster_main().
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#define MAY_REFER_TO_GTK_IN_HEADERS

#include "putty.h"
#include "gtkcompat.h"
#include "gtkmisc.h"

#if !GTK_CHECK_VERSION(2,18,0)
#error cputty requires GTK 2.18 or later
#endif

typedef struct ClusterSession {
    GtkFrontend *frontend;
    GtkWidget *window;          /* the session's terminal window */
    GtkWidget *check;           /* its enable/disable box in the console */
} ClusterSession;

/* Sessions currently open, in the order they were created */
static ClusterSession **sessions;
static size_t nsessions, sessions_size;

/* Positions (in the CmdlineArgList) of host arguments on the command line */
static size_t *host_arg_indices;
static size_t nhost_args, host_args_size;

/*
 * Label for the session window about to be created. Set immediately
 * before calling new_session_window(), which calls back into
 * make_gtk_toplevel_window() synchronously.
 */
static const char *pending_session_label;

/* Number of configuration boxes open whose outcome may add a session */
static int pending_config_boxes;

static GtkWidget *console_window, *console_host_box, *console_status;
static GtkWidget *console_echo;        /* shows what has been typed */
static GtkWidget *console_close_dialog; /* open close-confirmation box */
static guint echo_idle_timer;          /* source id, or 0 if none */

/* Longest text kept in the echo field; older text scrolls off the
 * front. Keeps it bounded in full-screen programs, where Return
 * (which clears it) may not be typed for a long time. */
#define ECHO_MAX_CHARS 200

/* Clear the echo field after this long without typing, so that text
 * typed at a password prompt doesn't stay on screen indefinitely. */
#define ECHO_IDLE_CLEAR_SECONDS 15
static GtkIMContext *console_imc;

/* ----------------------------------------------------------------------
 * Session list management.
 */

static size_t count_enabled_sessions(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nsessions; i++)
        if (gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(sessions[i]->check)))
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

static void quit_if_nothing_left(void)
{
    if (nsessions == 0 && pending_config_boxes == 0)
        gtk_main_quit();
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

    char *label = dupstr(pending_session_label && *pending_session_label ?
                         pending_session_label : "(unnamed session)");
    sess->check = gtk_check_button_new_with_label(label);
    sfree(label);

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

void session_window_closed(void)
{
    quit_if_nothing_left();
}

static void open_session_window(Conf *conf, const char *label,
                                const char *geometry_string)
{
    pending_session_label = label;
    new_session_window(conf, geometry_string);
    pending_session_label = NULL;
}

/* ----------------------------------------------------------------------
 * Launching sessions, both at startup and from the session windows'
 * context menus (New Session, Duplicate Session, Saved Sessions). In
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

    pending_config_boxes--;
    if (result > 0) {
        char *label = dupstr(conf_get_str(ctx->conf, CONF_host));
        open_session_window(ctx->conf, label, ctx->geometry_string);
        sfree(label);
    } else {
        conf_free(ctx->conf);
    }
    sfree(ctx->geometry_string);
    sfree(ctx);

    quit_if_nothing_left();
}

static void launch_via_config_box(Conf *conf, const char *geometry_string)
{
    struct config_box_ctx *ctx = snew(struct config_box_ctx);
    ctx->conf = conf;
    ctx->geometry_string = geometry_string ? dupstr(geometry_string) : NULL;
    pending_config_boxes++;
    initial_config_box(conf, post_config_box, ctx);
}

void launch_duplicate_session(Conf *conf)
{
    assert(conf_launchable(conf));
    open_session_window(conf_copy(conf), conf_get_str(conf, CONF_host),
                        NULL);
}

void launch_saved_session(const char *str)
{
    Conf *conf = conf_new();
    do_defaults(str, conf);

    /* As in ordinary PuTTY, New Session (str == NULL) always offers the
     * configuration box, even if Default Settings name a host. */
    if (str && conf_launchable(conf))
        open_session_window(conf, conf_get_str(conf, CONF_host), NULL);
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

/*
 * Build the Conf for one host (or, if host is NULL, for the options
 * alone). Returns whether it is launchable without a config box.
 */
static bool conf_for_host(int argc, char **argv, const char *host,
                          Conf *conf)
{
    /*
     * Use private copies of the argument words: cmdline.c overwrites
     * the -pw argument in place after reading it, which would leave
     * nothing for the next host.
     */
    char **hostargv = snewn(argc + 2, char *);
    int hostargc = 0;
    hostargv[hostargc++] = dupstr(argv[0]);
    for (int i = 1; i < argc; i++)
        if (!argv_index_is_host(i))
            hostargv[hostargc++] = dupstr(argv[i]);
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
    if (err)
        exit(1);
    cmdline_run_saved(conf);

    /*
     * hostargv is deliberately never freed: some options keep
     * pointers into its words for the life of the process (-geometry
     * and -name in main-gtk-simple.c), just as they point into the
     * real argv in ordinary PuTTY. cmdline.c has already blanked the
     * -pw word itself after taking its own copy of the password.
     */
    return cmdline_host_ok(conf);
}

static void launch_from_cmdline(int argc, char **argv, const char *host,
                                const char *const *geometry_string_ptr)
{
    Conf *conf = conf_new();
    bool launchable = conf_for_host(argc, argv, host, conf);

    /* Read the geometry only now: -geometry is parsed by the per-host
     * pass just made, not by the initial scan. */
    const char *geometry = *geometry_string_ptr;

    if (launchable)
        open_session_window(conf, host ? host : conf_get_str(conf, CONF_host),
                            geometry);
    else
        launch_via_config_box(conf, geometry);
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
 * The console window.
 */

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
        if (session_still_open(targets[i]) &&
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(targets[i]->check)))
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
        if (session_still_open(targets[i]) &&
            gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(targets[i]->check)))
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
    if (event->in)
        gtk_im_context_focus_in(console_imc);
    else
        gtk_im_context_focus_out(console_imc);
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
    console_close_dialog = create_message_box(
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

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, true, true, 0);
    gtk_widget_show(scroll);

    console_host_box = gtk_vbox_new(false, 0);
#if GTK_CHECK_VERSION(3,8,0)
    gtk_container_add(GTK_CONTAINER(scroll), console_host_box);
#else
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll),
                                          console_host_box);
#endif
    gtk_widget_show(console_host_box);

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

    update_console_status();
}

/* ----------------------------------------------------------------------
 * Entry point, called from main() in main-gtk-simple.c once GTK is
 * initialised and the command line has been scanned.
 */
int cluster_main(int argc, char **argv,
                 const char *const *geometry_string_ptr)
{
    /* Create (but don't yet show) the console, so that session
     * windows can register with it as they are made. */
    create_console_window();

    if (nhost_args == 0) {
        /* No hosts given: behave like plain PuTTY for the first session
         * (connect to a -load'ed session, or show the config box). More
         * can be added with New Session or Duplicate Session. */
        launch_from_cmdline(argc, argv, NULL, geometry_string_ptr);
    } else {
        for (size_t i = 0; i < nhost_args; i++)
            launch_from_cmdline(argc, argv, argv[host_arg_indices[i] + 1],
                                geometry_string_ptr);
    }

    wipe_password_args(argc, argv);

    /* Show the console last, so it's on top and has the focus. */
    gtk_widget_show(console_window);
    gtk_window_present(GTK_WINDOW(console_window));

    gtk_main();
    return 0;
}
