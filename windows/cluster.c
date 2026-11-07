/*
 * cluster.c: "cluster SSH" mode for Windows PuTTY, built as cputty.exe.
 *
 * The Windows counterpart of unix/cluster-gtk.c. cputty opens one
 * PuTTY terminal window per host named on the command line, plus a
 * console window. Keystrokes typed into the console are delivered to
 * every session ticked in the console's host list; typing into an
 * individual terminal window affects only that session.
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

#include <commctrl.h>

#define CONSOLE_CLASS_NAME "PuTTYClusterConsole"

enum {
    IDC_HINT = 100, IDC_ECHO, IDC_HIDE, IDC_HOSTS,
    IDC_ALL, IDC_NONE, IDC_INVERT, IDC_STATUS,
};

#define ECHO_TIMER_ID 1
/* Clear the echo field after this long without typing, so text typed
 * at a password prompt doesn't stay on screen indefinitely. */
#define ECHO_IDLE_CLEAR_MS 15000
/* Longest text kept in the echo field; older text scrolls off. */
#define ECHO_MAX_CHARS 200

static HWND console_hwnd, hint_hwnd, echo_hwnd, hide_hwnd, hosts_hwnd;
static HWND all_hwnd, none_hwnd, invert_hwnd, status_hwnd;

/* Sessions currently open, in creation order */
static WinGuiSeat **sessions;
static size_t nsessions, sessions_size;

/* Number of modal configuration boxes open that may add a session */
static int pending_config_boxes;

static wchar_t echo_text[ECHO_MAX_CHARS + 1];
static size_t echo_len;

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

static void quit_if_nothing_left(void)
{
    if (nsessions == 0 && pending_config_boxes == 0)
        PostQuitMessage(0);
}

static void add_session(WinGuiSeat *wgs, const char *label)
{
    sgrowarray(sessions, sessions_size, nsessions);
    sessions[nsessions++] = wgs;

    LVITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = INT_MAX;              /* append */
    item.pszText = (char *)(label && *label ? label : "(unnamed session)");
    item.lParam = (LPARAM)wgs;
    int idx = (int)SendMessageA(hosts_hwnd, LVM_INSERTITEMA, 0,
                                (LPARAM)&item);
    if (idx >= 0)
        ListView_SetCheckState(hosts_hwnd, idx, TRUE);
    update_status();
}

/* Called from window.c's WM_DESTROY for a session window */
void cluster_session_window_destroyed(WinGuiSeat *wgs)
{
    for (size_t i = 0; i < nsessions; i++) {
        if (sessions[i] == wgs) {
            memmove(sessions + i, sessions + i + 1,
                    (nsessions - i - 1) * sizeof(*sessions));
            nsessions--;
            break;
        }
    }
    int idx = find_list_item(wgs);
    if (idx >= 0)
        ListView_DeleteItem(hosts_hwnd, idx);

    win_free_session(wgs);
    update_status();
    quit_if_nothing_left();
}

/*
 * Start a session from a prepared Conf (taking ownership of it),
 * optionally via the configuration box first. Used for the command
 * line hosts and for New/Duplicate/Saved Session in a session's menu.
 */
void cluster_launch_session(Conf *conf, bool needs_config_box)
{
    if (needs_config_box) {
        pending_config_boxes++;
        bool ok = do_config(conf);
        pending_config_boxes--;
        if (!ok) {
            conf_free(conf);
            quit_if_nothing_left();
            return;
        }
    }

    prepare_session(conf);

    /* CONF_username may hold a UTF-8 or local-charset string */
    const char *user = conf_get_str_ambi(conf, CONF_username, NULL);
    const char *host = conf_get_str(conf, CONF_host);
    char *label = (user && *user) ? dupprintf("%s@%s", user, host)
                                  : dupstr(host);

    WinGuiSeat *wgs = win_new_session_window(conf, SW_SHOWNORMAL);
    add_session(wgs, label);
    sfree(label);
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
    const int m = 8, row = 24, gap = 6;
    int y = m, cw = w - 2 * m;
    MoveWindow(hint_hwnd, m, y, cw, 2 * row - 8, TRUE);
    y += 2 * row - 8 + gap;
    MoveWindow(echo_hwnd, m, y, cw, row, TRUE);
    y += row + gap;
    MoveWindow(hide_hwnd, m, y, cw, row - 4, TRUE);
    y += row - 4 + gap;

    int bottom = h - m;
    MoveWindow(status_hwnd, m, bottom - (row - 6), cw, row - 6, TRUE);
    bottom -= row - 6 + gap;
    int bw = (cw - 2 * gap) / 3;
    MoveWindow(all_hwnd, m, bottom - row, bw, row, TRUE);
    MoveWindow(none_hwnd, m + bw + gap, bottom - row, bw, row, TRUE);
    MoveWindow(invert_hwnd, m + 2 * (bw + gap), bottom - row,
               cw - 2 * (bw + gap), row, TRUE);
    bottom -= row + gap;

    MoveWindow(hosts_hwnd, m, y, cw, bottom - y > 40 ? bottom - y : 40, TRUE);
    ListView_SetColumnWidth(hosts_hwnd, 0, LVSCW_AUTOSIZE_USEHEADER);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 440, NULL, NULL, hinst, NULL);
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

    hosts_hwnd = make_control(WC_LISTVIEWA, "",
                              LVS_REPORT | LVS_NOCOLUMNHEADER |
                              LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              WS_EX_CLIENTEDGE, IDC_HOSTS);
    ListView_SetExtendedListViewStyle(hosts_hwnd,
                                      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = 300;
    SendMessageA(hosts_hwnd, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);

    all_hwnd = make_control("BUTTON", "All", BS_PUSHBUTTON, 0, IDC_ALL);
    none_hwnd = make_control("BUTTON", "None", BS_PUSHBUTTON, 0, IDC_NONE);
    invert_hwnd = make_control("BUTTON", "Invert", BS_PUSHBUTTON, 0,
                               IDC_INVERT);
    status_hwnd = make_control("STATIC", "", 0, 0, IDC_STATUS);

    RECT rc;
    GetClientRect(console_hwnd, &rc);
    layout_console(rc.right, rc.bottom);
    update_status();
}

/* ----------------------------------------------------------------------
 * Command line. Every non-option argument is a host. Each host's Conf
 * is built by parsing the options plus that one host, exactly as
 * PuTTY would parse 'putty [options] host', so user@host, -load, -l,
 * -P, -i and so on all work.
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

    CmdlineArgList *arglist = cmdline_arg_list_from_GetCommandLineW();
    size_t nargs = 0;
    while (arglist->args[nargs])
        nargs++;

    /* Scanning pass: find the host arguments, and check the options */
    bool *is_host = snewn(nargs ? nargs : 1, bool);
    memset(is_host, 0, (nargs ? nargs : 1) * sizeof(bool));
    size_t nhosts = 0;
    Conf *scratch = conf_new();
    for (size_t i = 0; i < nargs; i++) {
        CmdlineArg *arg = arglist->args[i];
        const char *p = cmdline_arg_to_str(arg);
        if (p[0] != '-') {
            is_host[i] = true;
            nhosts++;
            continue;
        }
        int ret = cmdline_process_param(arg, arglist->args[i + 1], -1,
                                        scratch);
        if (ret == -2)
            cmdline_error("option \"%s\" requires an argument", p);
        else if (ret == 2)
            i++;                       /* skip the option's value */
        else if (ret == 0)
            cmdline_error("unknown option \"%s\"", p);
    }
    conf_free(scratch);

    for (size_t h = 0; h < (nhosts ? nargs : 1); h++) {
        if (nhosts && !is_host[h])
            continue;

        cmdline_reset_host_state();
        Conf *conf = conf_new();
        conf_set_int(conf, CONF_logtype, LGTYP_NONE);
        do_defaults(NULL, conf);

        for (size_t i = 0; i < nargs; i++) {
            if (is_host[i] && i != h)
                continue;
            CmdlineArg *arg = arglist->args[i];
            const char *p = cmdline_arg_to_str(arg);
            /*
             * -pw is processed only for the first session: cmdline.c
             * blanks the argument in the process command line after
             * reading it, but keeps its own copy of the password,
             * which every session then uses.
             */
            if (nsessions + pending_config_boxes > 0 && !strcmp(p, "-pw")) {
                i++;
                continue;
            }
            int ret = cmdline_process_param(arg, arglist->args[i + 1], 1,
                                            conf);
            if (ret == 2)
                i++;
        }
        cmdline_run_saved(conf);
        cluster_launch_session(conf, !cmdline_host_ok(conf));
    }
    sfree(is_host);
}

void cluster_setup(const char *cmdline, int show)
{
    (void)cmdline;  /* we read GetCommandLineW, as putty.c does */

    create_console_window();

    /* Route network events for every session to the console, which
     * outlives any individual session window. */
    winselgui_set_hwnd(console_hwnd);

    launch_from_cmdline();

    /* Show the console last, so it ends up in front with the focus */
    ShowWindow(console_hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
    SetForegroundWindow(console_hwnd);

    quit_if_nothing_left();
}
