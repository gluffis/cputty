/*
 * cluster.c: "cluster SSH" mode for Windows PuTTY, built as cputty.exe.
 *
 * The Windows counterpart of unix/cluster-gtk.c. cputty opens one
 * PuTTY terminal window per host named on the command line, plus a
 * console window. Keystrokes typed into the console are delivered to
 * every session ticked in the console's host list; typing into an
 * individual terminal window affects only that session. The console
 * also lists host groups (see cluster-groups.h): clicking a group
 * opens a session to each of its hosts.
 *
 * All sessions run in this one process. Each console keystroke is
 * passed to every ticked session's own window procedure, so terminal
 * modes (application cursor keys, keypad mode, etc.) are honoured per
 * session. window.c arranges for the final character translation of
 * a broadcast keystroke to happen only once (see key_broadcast there),
 * so that dead keys work.
 *
 * window.c is compiled with PUTTY_CLUSTER for this program, which
 * makes WinMain call cluster_setup() instead of creating one session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "win-gui-seat.h"
#include "putty-rc.h"
#include "cluster-groups.h"
#include "cluster-layout.h"

#include <commctrl.h>

#define CONSOLE_CLASS_NAME "PuTTYClusterConsole"

enum {
    IDC_HINT = 100, IDC_ECHO, IDC_HIDE, IDC_HOSTS,
    IDC_ALL, IDC_NONE, IDC_INVERT, IDC_STATUS,
    IDC_GROUPS_LABEL, IDC_GROUPS, IDC_SAVE_GROUP, IDC_NEW_SESSION,
    IDC_ARRANGE, IDC_ADD_HOSTS,
};

/* Posted to the console to show "Add hosts" once the message loop is
 * running, when nothing was opened at startup */
#define WM_CLUSTER_ADD_HOSTS (WM_APP + 1)

/* Size of the console window when it opens, and the smallest size at
 * which its layout works */
#define CONSOLE_INITIAL_WIDTH 440
#define CONSOLE_INITIAL_HEIGHT 630
#define CONSOLE_MIN_WIDTH 360
#define CONSOLE_MIN_HEIGHT 460

#define ECHO_TIMER_ID 1
/* Clear the echo field after this long without typing, so text typed
 * at a password prompt doesn't stay on screen indefinitely. */
#define ECHO_IDLE_CLEAR_MS 15000
/* Longest text kept in the echo field; older text scrolls off. */
#define ECHO_MAX_CHARS 200

static HWND console_hwnd, hint_hwnd, echo_hwnd, hide_hwnd, hosts_hwnd;
static HWND all_hwnd, none_hwnd, invert_hwnd, status_hwnd;
static HWND groups_label_hwnd, groups_hwnd, save_group_hwnd;
static HWND new_session_hwnd, arrange_hwnd, add_hosts_hwnd;

/* Sessions currently open, in creation order, and for each one how to
 * open it again (its host spec, also its label in the host list) */
static WinGuiSeat **sessions;
static char **session_specs;
static size_t nsessions, sessions_size, session_specs_size;

/* The command line, kept so that hosts opened later (from groups) get
 * the same options as the ones named on it */
static CmdlineArgList *cmdline_args;
static size_t cmdline_nargs;
static bool *cmdline_is_host;
static bool password_option_used;      /* -pw already processed once */
static bool arrange_at_start;          /* a group with arrange=yes */

/* Host groups as last read from the file, and the file's text then
 * (NULL if there was no file), to tell when it has changed */
static ClusterGroups *groups;
static char *groups_text;

/* Session windows cascade so that each new one is visibly separate */
static size_t cascade_count;

static wchar_t echo_text[ECHO_MAX_CHARS + 1];
static size_t echo_len;

/* ----------------------------------------------------------------------
 * Messages.
 */

/* For messages containing group names or hosts, which are UTF-8 */
static int console_message_utf8(const char *msg, UINT flags)
{
    wchar_t *wmsg = dup_mb_to_wc(CP_UTF8, msg);
    wchar_t *wtitle = dup_mb_to_wc(CP_UTF8, appname);
    int ret = MessageBoxW(console_hwnd, wmsg, wtitle, flags);
    sfree(wmsg);
    sfree(wtitle);
    return ret;
}

/* For error messages from the system, in the local code page */
static void console_error_local(const char *msg)
{
    char *title = dupprintf("%s Error", appname);
    MessageBoxA(console_hwnd, msg, title, MB_OK | MB_ICONERROR);
    sfree(title);
}

/* ----------------------------------------------------------------------
 * Window placement.
 */

static RECT get_work_area(void)
{
    RECT r;
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &r, 0)) {
        r.left = r.top = 0;
        r.right = GetSystemMetrics(SM_CXSCREEN);
        r.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return r;
}

/* Choose where the next session window goes: cascading down and to
 * the right from the top left of the screen, over about half of it,
 * then starting again a little further across */
static void choose_next_window_position(void)
{
    RECT wa = get_work_area();
    int step = GetSystemMetrics(SM_CYCAPTION) +
        2 * GetSystemMetrics(SM_CYFRAME);
    if (step < 16)
        step = 16;

    int w = wa.right - wa.left, h = wa.bottom - wa.top;
    int extent = (w < h ? w : h) / 2;
    size_t per_run = extent / step > 0 ? extent / step : 1;
    size_t runs = (w / 2) / (3 * step);
    if (runs == 0)
        runs = 1;
    size_t k = cascade_count % per_run;
    size_t run = (cascade_count / per_run) % runs;
    cascade_count++;

    win_set_next_window_position(wa.left + (int)(k * step + run * 3 * step),
                                 wa.top + (int)(k * step));
}

/* Put the console in the bottom right corner, clear of the cascade */
static void place_console(void)
{
    RECT wa = get_work_area(), wr;
    GetWindowRect(console_hwnd, &wr);
    SetWindowPos(console_hwnd, NULL, wa.right - (wr.right - wr.left),
                 wa.bottom - (wr.bottom - wr.top), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/*
 * On Windows 10 and later, a window's rectangle includes invisible
 * resize borders. To make tiles meet edge to edge, find out how wide
 * they are (all zero if that can't be found out).
 */
DECL_WINDOWS_FUNCTION(static, HRESULT, DwmGetWindowAttribute,
                      (HWND, DWORD, PVOID, DWORD));
#define DWMWA_EXTENDED_FRAME_BOUNDS_VALUE 9

static RECT invisible_borders(HWND hwnd)
{
    static bool tried;
    RECT inv = { 0, 0, 0, 0 }, wr, fr;

    if (!tried) {
        HMODULE dwm = load_system32_dll("dwmapi.dll");
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(dwm, DwmGetWindowAttribute);
        tried = true;
    }
    if (p_DwmGetWindowAttribute && GetWindowRect(hwnd, &wr) &&
        SUCCEEDED(p_DwmGetWindowAttribute(
                      hwnd, DWMWA_EXTENDED_FRAME_BOUNDS_VALUE,
                      &fr, sizeof(fr)))) {
        inv.left = fr.left - wr.left;
        inv.top = fr.top - wr.top;
        inv.right = wr.right - fr.right;
        inv.bottom = wr.bottom - fr.bottom;
        /* Ignore anything implausible */
        if (inv.left < 0 || inv.left > 32 || inv.top < 0 || inv.top > 32 ||
            inv.right < 0 || inv.right > 32 || inv.bottom < 0 ||
            inv.bottom > 32)
            inv.left = inv.top = inv.right = inv.bottom = 0;
    }
    return inv;
}

/*
 * Fit a window into a tile of the screen. A window that can't shrink
 * that far (the console has a minimum size) is kept against the
 * tile's bottom right corner, so it stays on screen.
 */
static void place_in_tile(HWND hwnd, const ClusterRect *tile,
                          int min_w, int min_h)
{
    if (IsIconic(hwnd) || IsZoomed(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    RECT inv = invisible_borders(hwnd);
    int w = tile->w, h = tile->h;
    if (w < min_w)
        w = min_w;
    if (h < min_h)
        h = min_h;
    int x = tile->x + tile->w - w, y = tile->y + tile->h - h;
    if (w == tile->w)
        x = tile->x;
    if (h == tile->h)
        y = tile->y;

    SetWindowPos(hwnd, NULL, x - inv.left, y - inv.top,
                 w + inv.left + inv.right, h + inv.top + inv.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/*
 * "Arrange windows": tile every session window and the console evenly
 * over the screen, the console last (at the bottom right).
 */
static void arrange_windows(void)
{
    RECT wa = get_work_area();

    if (nsessions == 0) {
        /* Only the console: put it back in its corner, at its usual
         * size, rather than filling the screen with it */
        ClusterRect corner = { wa.right - 1, wa.bottom - 1, 1, 1 };
        place_in_tile(console_hwnd, &corner, CONSOLE_INITIAL_WIDTH,
                      CONSOLE_INITIAL_HEIGHT);
        SetForegroundWindow(console_hwnd);
        return;
    }

    size_t n = nsessions + 1;
    ClusterRect area = { wa.left, wa.top, wa.right - wa.left,
                         wa.bottom - wa.top };
    ClusterRect *tiles = snewn(n, ClusterRect);
    cluster_layout_tiles(n, area, tiles);

    for (size_t i = 0; i < nsessions; i++)
        place_in_tile(sessions[i]->term_hwnd, &tiles[i], 1, 1);
    place_in_tile(console_hwnd, &tiles[nsessions], CONSOLE_MIN_WIDTH,
                  CONSOLE_MIN_HEIGHT);
    SetForegroundWindow(console_hwnd);

    sfree(tiles);
}

/* ----------------------------------------------------------------------
 * Session list.
 */

static int find_list_item(WinGuiSeat *wgs)
{
    LVFINDINFOA fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)wgs;
    return (int)SendMessageA(hosts_hwnd, LVM_FINDITEMA, (WPARAM)-1,
                             (LPARAM)&fi);
}

static bool session_enabled(WinGuiSeat *wgs)
{
    int idx = find_list_item(wgs);
    return idx >= 0 && ListView_GetCheckState(hosts_hwnd, idx);
}

static size_t count_enabled(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nsessions; i++)
        if (session_enabled(sessions[i]))
            n++;
    return n;
}

static size_t count_connected(void)
{
    size_t n = 0;
    for (size_t i = 0; i < nsessions; i++)
        if (!sessions[i]->session_closed && sessions[i]->backend)
            n++;
    return n;
}

static void update_status(void)
{
    size_t enabled = count_enabled();
    char *status = dupprintf("Sending input to %u of %u session%s",
                             (unsigned)enabled, (unsigned)nsessions,
                             nsessions == 1 ? "" : "s");
    SetWindowTextA(status_hwnd, status);
    sfree(status);

    char *title = dupprintf("%s console (%u/%u active)", appname,
                            (unsigned)enabled, (unsigned)nsessions);
    SetWindowTextA(console_hwnd, title);
    sfree(title);

    /* Grey out the echo field when typing would go nowhere */
    EnableWindow(echo_hwnd, enabled > 0);
}

static void add_session(WinGuiSeat *wgs, const char *spec)
{
    sgrowarray(sessions, sessions_size, nsessions);
    sgrowarray(session_specs, session_specs_size, nsessions);
    sessions[nsessions] = wgs;
    session_specs[nsessions] = dupstr(spec);
    nsessions++;

    wchar_t *label = dup_mb_to_wc(CP_UTF8, *spec ? spec : "(unnamed session)");
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = INT_MAX;              /* append */
    item.pszText = label;
    item.lParam = (LPARAM)wgs;
    int idx = (int)SendMessageW(hosts_hwnd, LVM_INSERTITEMW, 0,
                                (LPARAM)&item);
    sfree(label);
    if (idx >= 0)
        ListView_SetCheckState(hosts_hwnd, idx, TRUE);
    update_status();
}

/* Called from window.c's WM_DESTROY for a session window */
void cluster_session_window_destroyed(WinGuiSeat *wgs)
{
    for (size_t i = 0; i < nsessions; i++) {
        if (sessions[i] == wgs) {
            sfree(session_specs[i]);
            memmove(sessions + i, sessions + i + 1,
                    (nsessions - i - 1) * sizeof(*sessions));
            memmove(session_specs + i, session_specs + i + 1,
                    (nsessions - i - 1) * sizeof(*session_specs));
            nsessions--;
            break;
        }
    }
    int idx = find_list_item(wgs);
    if (idx >= 0)
        ListView_DeleteItem(hosts_hwnd, idx);

    win_free_session(wgs);
    update_status();

    /* The console stays open with no sessions left, so that more can
     * be opened from it; cputty exits when the console is closed. */
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
 * Start a session from a prepared Conf (taking ownership of it),
 * optionally via the configuration box first. Used for the command
 * line hosts, for groups, and for New/Duplicate/Saved Session in a
 * session's menu.
 */
void cluster_launch_session(Conf *conf, bool needs_config_box,
                            const char *hostspec)
{
    if (needs_config_box) {
        if (!do_config(conf)) {
            conf_free(conf);
            return;
        }
        hostspec = NULL;               /* the host may have been changed */
    }

    if (hostspec && !cluster_host_spec_valid(hostspec))
        hostspec = NULL;
    char *spec = hostspec ? dupstr(hostspec) : host_spec_from_conf(conf);

    prepare_session(conf);
    choose_next_window_position();
    WinGuiSeat *wgs = win_new_session_window(conf, SW_SHOWNORMAL);
    add_session(wgs, spec);
    sfree(spec);
}

/*
 * Open a host given as a host spec (a command-line host argument or a
 * group member, optionally with :port), or with spec NULL, whatever
 * the command line's options alone describe. If that isn't enough to
 * connect, the configuration box is offered if allow_config_box is
 * set; otherwise nothing is opened and false is returned.
 */
static bool launch_host_spec(const char *spec, bool allow_config_box)
{
    char *host = NULL;
    int port = -1;
    if (spec)
        cluster_host_spec_split(spec, &host, &port);

    cmdline_reset_host_state();
    Conf *conf = conf_new();
    conf_set_int(conf, CONF_logtype, LGTYP_NONE);
    do_defaults(NULL, conf);

    for (size_t i = 0; i < cmdline_nargs; i++) {
        if (cmdline_is_host[i])
            continue;
        CmdlineArg *arg = cmdline_args->args[i];
        const char *p = cmdline_arg_to_str(arg);
        /*
         * -pw is processed only once: cmdline.c blanks the argument
         * in the process command line after reading it, but keeps its
         * own copy of the password, which every session then uses.
         */
        if (password_option_used && !strcmp(p, "-pw")) {
            i++;
            continue;
        }
        int ret = cmdline_process_param(arg, cmdline_args->args[i + 1], 1,
                                        conf);
        if (ret == 2)
            i++;
    }
    if (host) {
        /* The spec is UTF-8; command-line arguments want the local
         * code page as their main form */
        wchar_t *whost = dup_mb_to_wc(CP_UTF8, host);
        char *acphost = dup_wc_to_mb(CP_ACP, whost, "?");
        if (host[0] == '-' || acphost[0] == '-') {
            /* Specs are checked for a leading '-' before splitting,
             * but "[-x]:22" unwraps to "-x", and code-page conversion
             * can turn other characters into '-'. Check what would be
             * parsed, so a host is never read as an option. */
            char *msg = dupprintf("\"%s\" is not a valid host.", spec);
            console_message_utf8(msg, MB_OK | MB_ICONWARNING);
            sfree(msg);
            sfree(whost);
            sfree(acphost);
            sfree(host);
            conf_free(conf);
            return false;
        }
        CmdlineArg *harg = cmdline_arg_from_str(cmdline_args, acphost);
        cmdline_process_param(harg, NULL, 1, conf);
        sfree(whost);
        sfree(acphost);
        sfree(host);
    }
    cmdline_run_saved(conf);
    password_option_used = true;

    if (port > 0)
        conf_set_int(conf, CONF_port, port);

    bool launchable = cmdline_host_ok(conf);
    if (!launchable && !allow_config_box) {
        conf_free(conf);
        return false;
    }
    cluster_launch_session(conf, !launchable, spec);
    return true;
}

/* ----------------------------------------------------------------------
 * Host groups.
 */

static void rebuild_group_list(void)
{
    ListView_DeleteAllItems(groups_hwnd);
    for (size_t i = 0; i < groups->ngroups; i++) {
        const ClusterGroup *g = &groups->groups[i];
        strbuf *sb = strbuf_new();
        put_fmt(sb, "%s (%u%s):", g->name, (unsigned)g->nhosts,
                g->arrange ? ", arranged" : "");
        for (size_t j = 0; j < g->nhosts; j++)
            put_fmt(sb, " %s", g->hosts[j]);
        wchar_t *text = dup_mb_to_wc(CP_UTF8, sb->s);
        strbuf_free(sb);

        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)i;
        item.pszText = text;
        item.lParam = (LPARAM)i;
        SendMessageW(groups_hwnd, LVM_INSERTITEMW, 0, (LPARAM)&item);
        sfree(text);
    }
    ListView_SetColumnWidth(groups_hwnd, 0, LVSCW_AUTOSIZE_USEHEADER);
    SetWindowTextA(groups_label_hwnd, groups->ngroups ?
                   "Groups (click to open, right-click to delete):" :
                   "Groups: none yet. Tick hosts, then Save as group.");
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
            console_error_local(err);
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
    rebuild_group_list();
}

/* Name of the group shown at a position in the list, or NULL */
static char *group_name_at(int item)
{
    if (item < 0 || (size_t)item >= groups->ngroups)
        return NULL;
    return dupstr(groups->groups[item].name);
}

/* Open every host in a group, working from a copy of the host list,
 * which stays valid even if the groups are reloaded meanwhile */
static void launch_group_hosts(const ClusterGroup *g)
{
    size_t n = g->nhosts;
    char **hosts = snewn(n, char *);
    for (size_t i = 0; i < n; i++)
        hosts[i] = dupstr(g->hosts[i]);
    for (size_t i = 0; i < n; i++) {
        launch_host_spec(hosts[i], true);
        sfree(hosts[i]);
    }
    sfree(hosts);
}

static void open_group(const char *name)
{
    load_groups(true);                 /* pick up any edits */
    const ClusterGroup *g = cluster_groups_find(groups, name);
    if (!g) {
        char *msg = dupprintf("There is no longer a group called \"%s\".",
                              name);
        console_message_utf8(msg, MB_OK | MB_ICONWARNING);
        sfree(msg);
        return;
    }

    bool arrange = g->arrange;
    launch_group_hosts(g);

    if (arrange) {
        arrange_windows();             /* also brings the console forward */
    } else {
        /* Bring the console back in front of the new windows, since
         * it's where you type to them */
        SetForegroundWindow(console_hwnd);
    }
}

static void delete_group(const char *name)
{
    char *msg = dupprintf("Delete the group \"%s\"?\n"
                          "Its open sessions are not affected.", name);
    int answer = console_message_utf8(
        msg, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    sfree(msg);
    if (answer != IDYES)
        return;

    char *err;
    if (!cluster_groups_store(name, NULL, 0, false, &err)) {
        console_error_local(err);
        sfree(err);
    }
    load_groups(true);
}

static void group_context_menu(int item)
{
    char *name = group_name_at(item);
    if (!name)
        return;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open all hosts");
    char *label = dupprintf("Delete group \"%s\"...", name);
    wchar_t *wlabel = dup_mb_to_wc(CP_UTF8, label);
    AppendMenuW(menu, MF_STRING, 2, wlabel);
    sfree(wlabel);
    sfree(label);

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, console_hwnd, NULL);
    DestroyMenu(menu);

    if (cmd == 1)
        open_group(name);
    else if (cmd == 2)
        delete_group(name);
    sfree(name);
}

/*
 * The "Save as group" dialog, asking for a name. Its template is
 * built in memory so that no resource file is needed.
 */
enum { IDC_GN_INTRO = 200, IDC_GN_NAME, IDC_GN_HOSTS, IDC_GN_ARRANGE };

struct group_name_dialog {
    const char *intro, *hosts;         /* UTF-8 */
    char *result;                      /* UTF-8, set on OK */
    bool arrange;                      /* set on OK */
};

static void dlg_set_text_utf8(HWND dlg, int id, const char *text)
{
    wchar_t *w = dup_mb_to_wc(CP_UTF8, text);
    SetDlgItemTextW(dlg, id, w);
    sfree(w);
}

static INT_PTR CALLBACK GroupNameProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    struct group_name_dialog *ctx =
        (struct group_name_dialog *)GetWindowLongPtr(hwnd, DWLP_USER);

    switch (msg) {
      case WM_INITDIALOG:
        ctx = (struct group_name_dialog *)lParam;
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);
        dlg_set_text_utf8(hwnd, IDC_GN_INTRO, ctx->intro);
        dlg_set_text_utf8(hwnd, IDC_GN_HOSTS, ctx->hosts);
        SendDlgItemMessageW(hwnd, IDC_GN_NAME, EM_LIMITTEXT, 100, 0);
        SetFocus(GetDlgItem(hwnd, IDC_GN_NAME));
        return FALSE;                  /* we set the focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK: {
            wchar_t buf[128];
            GetDlgItemTextW(hwnd, IDC_GN_NAME, buf, lenof(buf));
            char *raw = dup_wc_to_mb(CP_UTF8, buf, "?");
            char *name = cluster_group_name_trim(raw);
            sfree(raw);
            if (!cluster_group_name_valid(name)) {
                MessageBoxW(hwnd, L"A group name can't be empty, contain "
                            L"spaces, or start with \"-\" or \"#\".",
                            L"Save as group", MB_OK | MB_ICONWARNING);
                sfree(name);
                SetFocus(GetDlgItem(hwnd, IDC_GN_NAME));
                return TRUE;
            }
            ctx->result = name;
            ctx->arrange = IsDlgButtonChecked(hwnd, IDC_GN_ARRANGE) ==
                BST_CHECKED;
            EndDialog(hwnd, IDOK);
            return TRUE;
          }
          case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void dlg_put_word(strbuf *sb, WORD w) { put_data(sb, &w, sizeof(w)); }
static void dlg_put_dword(strbuf *sb, DWORD d) { put_data(sb, &d, sizeof(d)); }
static void dlg_put_wstr(strbuf *sb, const wchar_t *s)
{
    put_data(sb, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

static void dlg_add_item(strbuf *sb, DWORD style, short x, short y,
                         short cx, short cy, WORD id, WORD class_atom,
                         const wchar_t *text)
{
    while (sb->len % 4)                /* items are DWORD-aligned */
        put_byte(sb, 0);
    dlg_put_dword(sb, style | WS_CHILD | WS_VISIBLE);
    dlg_put_dword(sb, 0);              /* extended style */
    dlg_put_word(sb, x);
    dlg_put_word(sb, y);
    dlg_put_word(sb, cx);
    dlg_put_word(sb, cy);
    dlg_put_word(sb, id);
    dlg_put_word(sb, 0xFFFF);          /* predefined class follows */
    dlg_put_word(sb, class_atom);
    dlg_put_wstr(sb, text);
    dlg_put_word(sb, 0);               /* no creation data */
}

/* Returns the name entered (UTF-8), or NULL if cancelled, and whether
 * "arrange the windows" was ticked */
static char *ask_group_name(const char *intro, const char *hosts,
                            bool *arrange)
{
    enum { BUTTON = 0x0080, EDIT = 0x0081, STATIC = 0x0082 };
    strbuf *t = strbuf_new();
    dlg_put_dword(t, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP |
                  WS_CAPTION | WS_SYSMENU);
    dlg_put_dword(t, 0);               /* extended style */
    dlg_put_word(t, 6);                /* number of items */
    dlg_put_word(t, 0);                /* x, y, cx, cy */
    dlg_put_word(t, 0);
    dlg_put_word(t, 230);
    dlg_put_word(t, 112);
    dlg_put_word(t, 0);                /* no menu */
    dlg_put_word(t, 0);                /* default class */
    dlg_put_wstr(t, L"Save as group");
    dlg_put_word(t, 8);                /* font size and face */
    dlg_put_wstr(t, L"MS Shell Dlg");
    dlg_add_item(t, SS_LEFT | SS_NOPREFIX, 7, 7, 216, 10, IDC_GN_INTRO,
                 STATIC, L"");
    dlg_add_item(t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 7, 20, 216, 13,
                 IDC_GN_NAME, EDIT, L"");
    dlg_add_item(t, SS_LEFT | SS_NOPREFIX, 7, 39, 216, 34, IDC_GN_HOSTS,
                 STATIC, L"");
    dlg_add_item(t, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 75, 216, 10,
                 IDC_GN_ARRANGE, BUTTON,
                 L"Arrange the windows when this group is opened");
    dlg_add_item(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 119, 92, 50, 14, IDOK,
                 BUTTON, L"Save");
    dlg_add_item(t, BS_PUSHBUTTON | WS_TABSTOP, 173, 92, 50, 14, IDCANCEL,
                 BUTTON, L"Cancel");

    struct group_name_dialog ctx = { intro, hosts, NULL, false };
    INT_PTR r = DialogBoxIndirectParamW(hinst, (LPCDLGTEMPLATEW)t->s,
                                        console_hwnd, GroupNameProc,
                                        (LPARAM)&ctx);
    strbuf_free(t);
    if (r != IDOK) {
        sfree(ctx.result);
        return NULL;
    }
    *arrange = ctx.arrange;
    return ctx.result;
}

/* "Save as group": store the ticked sessions' hosts under a name */
static void save_group(void)
{
    char **hosts = NULL;
    size_t nhosts = 0, hostsize = 0, unsaveable = 0;
    for (size_t i = 0; i < nsessions; i++) {
        const char *spec = session_specs[i];
        if (!session_enabled(sessions[i]))
            continue;
        if (!cluster_host_spec_valid(spec)) {
            unsaveable++;
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < nhosts; j++)
            if (!strcmp(hosts[j], spec))
                dup = true;
        if (!dup) {
            sgrowarray(hosts, hostsize, nhosts);
            hosts[nhosts++] = dupstr(spec);
        }
    }

    if (nhosts == 0) {
        console_message_utf8("There are no ticked hosts to save. Tick the "
                             "hosts you want in the group first.",
                             MB_OK | MB_ICONINFORMATION);
        sfree(hosts);
        return;
    }

    char *intro = dupprintf("Save the %u ticked host%s as a group named:",
                            (unsigned)nhosts, nhosts == 1 ? "" : "s");
    strbuf *list = strbuf_new();
    for (size_t i = 0; i < nhosts; i++)
        put_fmt(list, "%s%s", i ? " " : "", hosts[i]);
    if (unsaveable)
        put_fmt(list, "\n(%u ticked session%s can't be saved: a host name "
                "with spaces in it can't go in a group.)",
                (unsigned)unsaveable, unsaveable == 1 ? "" : "s");

    bool arrange = false;
    char *name = ask_group_name(intro, list->s, &arrange);
    sfree(intro);
    strbuf_free(list);

    if (name) {
        load_groups(false);
        bool go_ahead = true;
        if (cluster_groups_find(groups, name)) {
            char *msg = dupprintf("There is already a group called \"%s\".\n"
                                  "Replace it?", name);
            go_ahead = console_message_utf8(
                msg, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            sfree(msg);
        }
        if (go_ahead) {
            char *err;
            if (!cluster_groups_store(name, hosts, nhosts, arrange, &err)) {
                console_error_local(err);
                sfree(err);
            }
            load_groups(true);
        }
        sfree(name);
    }

    for (size_t i = 0; i < nhosts; i++)
        sfree(hosts[i]);
    sfree(hosts);
}

static void new_session(void)
{
    Conf *conf = conf_new();
    do_defaults(NULL, conf);
    cluster_launch_session(conf, true, NULL);
}

/*
 * The "Add hosts" dialog: type one or more hosts (or group names),
 * separated by spaces or new lines, and each is opened as if it had
 * been named on the command line. Shown by the console's button, and
 * at startup if there was nothing to open.
 */
enum { IDC_AH_INTRO = 300, IDC_AH_HOSTS };

struct add_hosts_dialog {
    char *result;                      /* UTF-8, set on OK */
};

static INT_PTR CALLBACK AddHostsProc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam)
{
    struct add_hosts_dialog *ctx =
        (struct add_hosts_dialog *)GetWindowLongPtr(hwnd, DWLP_USER);

    switch (msg) {
      case WM_INITDIALOG:
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);
        SetFocus(GetDlgItem(hwnd, IDC_AH_HOSTS));
        return FALSE;                  /* we set the focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK: {
            HWND edit = GetDlgItem(hwnd, IDC_AH_HOSTS);
            int len = GetWindowTextLengthW(edit);
            wchar_t *buf = snewn(len + 1, wchar_t);
            GetWindowTextW(edit, buf, len + 1);
            char *text = dup_wc_to_mb(CP_UTF8, buf, "?");
            sfree(buf);
            if (!text[strspn(text, " \t\r\n")]) {
                MessageBoxW(hwnd, L"Type at least one host, for example "
                            L"user@host or host:port.", L"Add hosts",
                            MB_OK | MB_ICONINFORMATION);
                sfree(text);
                SetFocus(edit);
                return TRUE;
            }
            ctx->result = text;
            EndDialog(hwnd, IDOK);
            return TRUE;
          }
          case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Returns the text entered (UTF-8), or NULL if cancelled */
static char *ask_hosts(void)
{
    enum { BUTTON = 0x0080, EDIT = 0x0081, STATIC = 0x0082 };
    strbuf *t = strbuf_new();
    dlg_put_dword(t, DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP |
                  WS_CAPTION | WS_SYSMENU);
    dlg_put_dword(t, 0);               /* extended style */
    dlg_put_word(t, 4);                /* number of items */
    dlg_put_word(t, 0);                /* x, y, cx, cy */
    dlg_put_word(t, 0);
    dlg_put_word(t, 250);
    dlg_put_word(t, 140);
    dlg_put_word(t, 0);                /* no menu */
    dlg_put_word(t, 0);                /* default class */
    dlg_put_wstr(t, L"Add hosts");
    dlg_put_word(t, 8);                /* font size and face */
    dlg_put_wstr(t, L"MS Shell Dlg");
    dlg_add_item(t, SS_LEFT | SS_NOPREFIX, 7, 7, 236, 34, IDC_AH_INTRO,
                 STATIC,
                 L"Hosts to connect to, separated by spaces or new lines. "
                 L"Each can be host, user@host, host:port, the name of a "
                 L"saved session, or the name of a group. Options from "
                 L"the command line apply to every host.");
    dlg_add_item(t, WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                 ES_AUTOVSCROLL, 7, 44, 236, 70, IDC_AH_HOSTS, EDIT, L"");
    dlg_add_item(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 139, 120, 50, 14, IDOK,
                 BUTTON, L"Open");
    dlg_add_item(t, BS_PUSHBUTTON | WS_TABSTOP, 193, 120, 50, 14, IDCANCEL,
                 BUTTON, L"Cancel");

    struct add_hosts_dialog ctx = { NULL };
    INT_PTR r = DialogBoxIndirectParamW(hinst, (LPCDLGTEMPLATEW)t->s,
                                        console_hwnd, AddHostsProc,
                                        (LPARAM)&ctx);
    strbuf_free(t);
    if (r != IDOK) {
        sfree(ctx.result);
        return NULL;
    }
    return ctx.result;
}

static void add_hosts(void)
{
    char *text = ask_hosts();
    if (!text)
        return;

    load_groups(true);                 /* pick up any edits */
    strbuf *invalid = strbuf_new();
    bool arrange = false;
    const char *seps = " \t\r\n";
    for (char *p = text + strspn(text, seps); *p;
         p += strspn(p, seps)) {
        size_t len = strcspn(p, seps);
        char *word = dupprintf("%.*s", (int)len, p);
        p += len;

        const ClusterGroup *g = cluster_groups_find(groups, word);
        if (g) {
            if (g->arrange)
                arrange = true;
            launch_group_hosts(g);
        } else if (cluster_host_spec_valid(word)) {
            launch_host_spec(word, true);
        } else {
            put_fmt(invalid, "%s%s", invalid->len ? " " : "", word);
        }
        sfree(word);
    }
    sfree(text);

    if (invalid->len) {
        char *msg = dupprintf("These are not valid hosts and were not "
                              "opened:\n%s", invalid->s);
        console_message_utf8(msg, MB_OK | MB_ICONWARNING);
        sfree(msg);
    }
    strbuf_free(invalid);

    if (arrange)
        arrange_windows();             /* also brings the console forward */
    else
        SetForegroundWindow(console_hwnd);
}

/* ----------------------------------------------------------------------
 * The echo field. Keystrokes go to the sessions as they are typed
 * (so Tab completion and full-screen programs work); this is only a
 * local reflection of the current input line. Return or Escape (or
 * Ctrl-C / Ctrl-U, which discard a shell's input line) clear it.
 */

static void echo_refresh(void)
{
    echo_text[echo_len] = L'\0';
    SetWindowTextW(echo_hwnd, echo_text);
    SendMessageW(echo_hwnd, EM_SETSEL, echo_len, echo_len);
}

static void echo_clear(void)
{
    echo_len = 0;
    echo_refresh();
}

static void echo_append(const wchar_t *chars, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (chars[i] < 0x20 || chars[i] == 0x7F)
            continue;                  /* control characters: not shown */
        if (echo_len == ECHO_MAX_CHARS) {
            memmove(echo_text, echo_text + 1,
                    (ECHO_MAX_CHARS - 1) * sizeof(wchar_t));
            echo_len--;
        }
        echo_text[echo_len++] = chars[i];
    }
    echo_refresh();
}

static void echo_backspace(void)
{
    if (echo_len > 0) {
        echo_len--;
        /* Don't leave half a surrogate pair */
        if (echo_len > 0 && IS_LOW_SURROGATE(echo_text[echo_len]) &&
            IS_HIGH_SURROGATE(echo_text[echo_len - 1]))
            echo_len--;
    }
    echo_refresh();
}

static void echo_restart_idle_timer(void)
{
    SetTimer(console_hwnd, ECHO_TIMER_ID, ECHO_IDLE_CLEAR_MS, NULL);
}

/* ----------------------------------------------------------------------
 * Broadcasting input.
 */

static void broadcast_key(const MSG *msg)
{
    bool key_down = (msg->message == WM_KEYDOWN ||
                     msg->message == WM_SYSKEYDOWN);
    if (key_down)
        echo_restart_idle_timer();

    /* Work from a snapshot, in case a session closes during delivery */
    size_t n = nsessions;
    WinGuiSeat **targets = snewn(n ? n : 1, WinGuiSeat *);
    memcpy(targets, sessions, n * sizeof(*targets));
    bool *enabled = snewn(n ? n : 1, bool);
    for (size_t i = 0; i < n; i++)
        enabled[i] = session_enabled(targets[i]);

    wchar_t chars[4];
    win_broadcast_key_begin();
    for (size_t i = 0; i < n; i++) {
        bool still_open = false;
        for (size_t j = 0; j < nsessions; j++)
            if (sessions[j] == targets[i])
                still_open = true;
        if (still_open && enabled[i] && !targets[i]->session_closed)
            SendMessageW(targets[i]->term_hwnd, msg->message,
                         msg->wParam, msg->lParam);
    }
    int nchars = win_broadcast_key_end(chars, lenof(chars));

    sfree(targets);
    sfree(enabled);

    if (!key_down || count_enabled() == 0)
        return;

    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool altgr = ctrl && GetKeyState(VK_RMENU) < 0;
    switch (msg->wParam) {
      case VK_RETURN:
      case VK_ESCAPE:
        echo_clear();
        return;
      case VK_BACK:
        echo_backspace();
        return;
      case 'C':
      case 'U':
        if (ctrl && !altgr) {
            echo_clear();
            return;
        }
        break;
    }
    if (nchars > 0)
        echo_append(chars, nchars);
}

static void broadcast_char(wchar_t wc)
{
    echo_restart_idle_timer();
    for (size_t i = 0; i < nsessions; i++)
        if (session_enabled(sessions[i]) && !sessions[i]->session_closed)
            SendMessageW(sessions[i]->term_hwnd, WM_CHAR, wc, 0);
    if (count_enabled() > 0)
        echo_append(&wc, 1);
}

/*
 * Called from the main message loop before dispatching each message.
 * Takes keyboard input aimed at the console (or any of its controls)
 * and broadcasts it instead. Returns true if the message was consumed.
 */
bool cluster_filter_message(MSG *msg)
{
    if (!console_hwnd ||
        (msg->hwnd != console_hwnd && !IsChild(console_hwnd, msg->hwnd)))
        return false;

    switch (msg->message) {
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP:
        if (msg->wParam == VK_PROCESSKEY)
            return false;     /* IME: its result arrives as WM_CHAR */
        if (msg->wParam == VK_PACKET) {
            /* Unicode input injected by e.g. an on-screen keyboard */
            if (msg->message == WM_KEYDOWN)
                TranslateMessage(msg);
            return true;
        }
        if ((msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP) &&
            (msg->wParam == VK_F4 || msg->wParam == VK_SPACE)) {
            /* Alt+F4 and Alt+Space act on the console itself */
            if (msg->hwnd != console_hwnd) {
                DefWindowProcA(console_hwnd, msg->message, msg->wParam,
                               msg->lParam);
                return true;
            }
            return false;
        }
        broadcast_key(msg);
        return true;

      case WM_CHAR:
        broadcast_char((wchar_t)msg->wParam);
        return true;

      case WM_SYSCHAR:
      case WM_DEADCHAR:
      case WM_SYSDEADCHAR:
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------------
 * The console window.
 */

static void set_all_checks(int mode)  /* 0 none, 1 all, 2 invert */
{
    int count = ListView_GetItemCount(hosts_hwnd);
    for (int i = 0; i < count; i++) {
        BOOL state = (mode == 1 ? TRUE : mode == 0 ? FALSE :
                      !ListView_GetCheckState(hosts_hwnd, i));
        ListView_SetCheckState(hosts_hwnd, i, state);
    }
    update_status();
}

static void layout_console(int w, int h)
{
    const int m = 8, row = 24, gap = 6, groups_height = 96;
    int y = m, cw = w - 2 * m;
    MoveWindow(hint_hwnd, m, y, cw, 2 * row - 8, TRUE);
    y += 2 * row - 8 + gap;
    MoveWindow(echo_hwnd, m, y, cw, row, TRUE);
    y += row + gap;
    MoveWindow(hide_hwnd, m, y, cw, row - 4, TRUE);
    y += row - 4 + gap;

    /* From the bottom up: status, window buttons, groups, host buttons */
    int bottom = h - m;
    MoveWindow(status_hwnd, m, bottom - (row - 6), cw, row - 6, TRUE);
    bottom -= row - 6 + gap;

    int third = (cw - 2 * gap) / 3;
    MoveWindow(add_hosts_hwnd, m, bottom - row, third, row, TRUE);
    MoveWindow(new_session_hwnd, m + third + gap, bottom - row, third, row,
               TRUE);
    MoveWindow(arrange_hwnd, m + 2 * (third + gap), bottom - row,
               cw - 2 * (third + gap), row, TRUE);
    bottom -= row + gap;

    MoveWindow(save_group_hwnd, m, bottom - row, cw, row, TRUE);
    bottom -= row + 2;

    MoveWindow(groups_hwnd, m, bottom - groups_height, cw, groups_height,
               TRUE);
    bottom -= groups_height + 2;
    MoveWindow(groups_label_hwnd, m, bottom - (row - 8), cw, row - 8, TRUE);
    bottom -= row - 8 + gap;

    int bw = (cw - 2 * gap) / 3;
    MoveWindow(all_hwnd, m, bottom - row, bw, row, TRUE);
    MoveWindow(none_hwnd, m + bw + gap, bottom - row, bw, row, TRUE);
    MoveWindow(invert_hwnd, m + 2 * (bw + gap), bottom - row,
               cw - 2 * (bw + gap), row, TRUE);
    bottom -= row + gap;

    MoveWindow(hosts_hwnd, m, y, cw, bottom - y > 40 ? bottom - y : 40, TRUE);
    ListView_SetColumnWidth(hosts_hwnd, 0, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(groups_hwnd, 0, LVSCW_AUTOSIZE_USEHEADER);
}

static LRESULT CALLBACK ConsoleWndProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_NETEVENT:
        /* Network events for all sessions are routed to this window,
         * which lives as long as the process does. */
        winselgui_response(message, wParam, lParam);
        return 0;

      case WM_SIZE:
        layout_console(LOWORD(lParam), HIWORD(lParam));
        return 0;

      case WM_CLUSTER_ADD_HOSTS:
        if (nsessions == 0)            /* nothing opened meanwhile */
            add_hosts();
        return 0;

      case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = CONSOLE_MIN_WIDTH;
        mmi->ptMinTrackSize.y = CONSOLE_MIN_HEIGHT;
        return 0;
      }

      case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && groups)
            load_groups(false);        /* show any edits to the file */
        break;

      case WM_TIMER:
        if (wParam == ECHO_TIMER_ID) {
            KillTimer(hwnd, ECHO_TIMER_ID);
            echo_clear();
        }
        return 0;

      case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
              case IDC_ALL: set_all_checks(1); break;
              case IDC_NONE: set_all_checks(0); break;
              case IDC_INVERT: set_all_checks(2); break;
              case IDC_SAVE_GROUP: save_group(); break;
              case IDC_NEW_SESSION: new_session(); break;
              case IDC_ADD_HOSTS: add_hosts(); break;
              case IDC_ARRANGE: arrange_windows(); break;
              case IDC_HIDE: {
                bool hide = SendMessageA(hide_hwnd, BM_GETCHECK, 0, 0)
                    == BST_CHECKED;
                SendMessageW(echo_hwnd, EM_SETPASSWORDCHAR,
                             hide ? 0x25CF : 0, 0);
                InvalidateRect(echo_hwnd, NULL, TRUE);
                break;
              }
            }
            /* Keep keyboard focus on the console, not the button */
            SetFocus(hwnd);
        }
        return 0;

      case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->idFrom == IDC_HOSTS && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lParam;
            if ((lv->uChanged & LVIF_STATE) &&
                ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
                update_status();
        } else if (nm->idFrom == IDC_GROUPS &&
                   (nm->code == LVN_ITEMACTIVATE || nm->code == NM_RCLICK)) {
            int item = ((NMITEMACTIVATE *)lParam)->iItem;
            if (nm->code == NM_RCLICK) {
                group_context_menu(item);
            } else {
                char *name = group_name_at(item);
                if (name) {
                    open_group(name);
                    sfree(name);
                }
            }
        }
        return 0;
      }

      case WM_CLOSE: {
        /* As in cssh, closing the console ends the whole cluster, so
         * ask first if that would drop any live connections. */
        size_t connected = count_connected();
        if (connected > 0) {
            char *title = dupprintf("%s Exit Confirmation", appname);
            char *msg = dupprintf(
                "Closing the console will close all sessions.\n"
                "%u session%s still connected. Close anyway?",
                (unsigned)connected, connected == 1 ? " is" : "s are");
            int answer = MessageBoxA(hwnd, msg, title,
                                     MB_ICONWARNING | MB_YESNO |
                                     MB_DEFBUTTON2);
            sfree(title);
            sfree(msg);
            if (answer != IDYES)
                return 0;
        }
        PostQuitMessage(0);
        return 0;
      }
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static HWND make_control(const char *cls, const char *text, DWORD style,
                         DWORD exstyle, int id)
{
    HWND w = CreateWindowExA(exstyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, console_hwnd, (HMENU)(INT_PTR)id,
                             hinst, NULL);
    SendMessageA(w, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
                 TRUE);
    return w;
}

static HWND make_list(int id, DWORD exstyles)
{
    HWND w = make_control(WC_LISTVIEWA, "",
                          LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL |
                          LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, id);
    ListView_SetExtendedListViewStyle(w, exstyles | LVS_EX_FULLROWSELECT);
    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = 300;
    SendMessageA(w, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    return w;
}

static void create_console_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ConsoleWndProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CONSOLE_CLASS_NAME;
    RegisterClassA(&wc);

    console_hwnd = CreateWindowExA(
        0, CONSOLE_CLASS_NAME, appname, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CONSOLE_INITIAL_WIDTH,
        CONSOLE_INITIAL_HEIGHT, NULL, NULL, hinst, NULL);
    if (!console_hwnd)
        modalfatalbox("Unable to create cluster console window: %s",
                      win_strerror(GetLastError()));

    hint_hwnd = make_control(
        "STATIC", "Type here to send keystrokes to every ticked session.\r\n"
        "Type in a terminal window to talk to that session alone.",
        0, 0, IDC_HINT);

    /* The echo field is display-only: all typing is captured by
     * cluster_filter_message, so it never needs keyboard focus. A
     * Unicode control, so typed non-ASCII characters show properly. */
    echo_hwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 10, 10, console_hwnd, (HMENU)(INT_PTR)IDC_ECHO, hinst, NULL);
    SendMessageW(echo_hwnd, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    hide_hwnd = make_control("BUTTON", "Hide typed text (e.g. for passwords)",
                             BS_AUTOCHECKBOX, 0, IDC_HIDE);

    hosts_hwnd = make_list(IDC_HOSTS, LVS_EX_CHECKBOXES);

    all_hwnd = make_control("BUTTON", "All", BS_PUSHBUTTON, 0, IDC_ALL);
    none_hwnd = make_control("BUTTON", "None", BS_PUSHBUTTON, 0, IDC_NONE);
    invert_hwnd = make_control("BUTTON", "Invert", BS_PUSHBUTTON, 0,
                               IDC_INVERT);

    /* Groups: a single click on one opens all its hosts */
    groups_label_hwnd = make_control("STATIC", "", SS_NOPREFIX, 0,
                                     IDC_GROUPS_LABEL);
    groups_hwnd = make_list(IDC_GROUPS, LVS_EX_ONECLICKACTIVATE |
                            LVS_EX_UNDERLINEHOT | LVS_EX_INFOTIP);
    save_group_hwnd = make_control("BUTTON", "Save ticked hosts as group...",
                                   BS_PUSHBUTTON, 0, IDC_SAVE_GROUP);
    add_hosts_hwnd = make_control("BUTTON", "Add hosts...",
                                  BS_PUSHBUTTON, 0, IDC_ADD_HOSTS);
    new_session_hwnd = make_control("BUTTON", "New session...",
                                    BS_PUSHBUTTON, 0, IDC_NEW_SESSION);
    arrange_hwnd = make_control("BUTTON", "Arrange windows",
                                BS_PUSHBUTTON, 0, IDC_ARRANGE);

    status_hwnd = make_control("STATIC", "", 0, 0, IDC_STATUS);

    RECT rc;
    GetClientRect(console_hwnd, &rc);
    layout_console(rc.right, rc.bottom);
    update_status();
}

/* ----------------------------------------------------------------------
 * Command line. Every non-option argument is a host, or the name of a
 * group of hosts. Each host's Conf is built by parsing the options
 * plus that one host, exactly as PuTTY would parse 'putty [options]
 * host', so user@host, -load, -l, -P, -i and so on all work.
 */

static void launch_from_cmdline(void)
{
    settings_set_default_protocol(be_default_protocol);
    {
        const struct BackendVtable *vt =
            backend_vt_from_proto(be_default_protocol);
        settings_set_default_port(0); /* illegal */
        if (vt)
            settings_set_default_port(vt->default_port);
    }

    cmdline_args = cmdline_arg_list_from_GetCommandLineW();
    cmdline_nargs = 0;
    while (cmdline_args->args[cmdline_nargs])
        cmdline_nargs++;

    /* Scanning pass: find the host arguments, and check the options */
    cmdline_is_host = snewn(cmdline_nargs ? cmdline_nargs : 1, bool);
    memset(cmdline_is_host, 0,
           (cmdline_nargs ? cmdline_nargs : 1) * sizeof(bool));
    size_t nhosts = 0;
    Conf *scratch = conf_new();
    for (size_t i = 0; i < cmdline_nargs; i++) {
        CmdlineArg *arg = cmdline_args->args[i];
        const char *p = cmdline_arg_to_str(arg);
        if (p[0] != '-') {
            cmdline_is_host[i] = true;
            nhosts++;
            continue;
        }
        int ret = cmdline_process_param(arg, cmdline_args->args[i + 1], -1,
                                        scratch);
        if (ret == -2)
            cmdline_error("option \"%s\" requires an argument", p);
        else if (ret == 2)
            i++;                       /* skip the option's value */
        else if (ret == 0)
            cmdline_error("unknown option \"%s\"", p);
    }
    conf_free(scratch);

    if (nhosts == 0) {
        /* No hosts given: open a session given with -load, if any.
         * Otherwise just show the console, where a group or a new
         * session can be opened. */
        launch_host_spec(NULL, false);
        return;
    }

    for (size_t i = 0; i < cmdline_nargs; i++) {
        if (!cmdline_is_host[i])
            continue;
        CmdlineArg *arg = cmdline_args->args[i];
        const char *spec = cmdline_arg_to_utf8(arg);
        if (!spec)
            spec = cmdline_arg_to_str(arg);

        const ClusterGroup *g = cluster_groups_find(groups, spec);
        if (g) {
            if (g->arrange)
                arrange_at_start = true;
            launch_group_hosts(g);
        } else {
            launch_host_spec(spec, true);
        }
    }
}

void cluster_setup(const char *cmdline, int show)
{
    (void)cmdline;  /* we read GetCommandLineW, as putty.c does */

    create_console_window();

    /* Route network events for every session to the console, which
     * outlives any individual session window. */
    winselgui_set_hwnd(console_hwnd);

    load_groups(true);
    launch_from_cmdline();

    /* Show the console last, so it ends up in front with the focus */
    place_console();
    ShowWindow(console_hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
    SetForegroundWindow(console_hwnd);
    if (arrange_at_start)
        arrange_windows();

    /* Nothing to connect to: ask for some hosts, rather than leaving
     * an empty console */
    if (nsessions == 0)
        PostMessage(console_hwnd, WM_CLUSTER_ADD_HOSTS, 0, 0);
}
