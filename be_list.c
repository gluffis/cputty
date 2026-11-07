/*
 * Source file that is rebuilt per application, and provides the list
 * of backends, the default protocol, and the application name.
 *
 * This file expects the build system to provide some per-application
 * definitions on the compiler command line. So you don't just add it
 * directly to the sources list for an application. Instead you call
 * the be_list() function defined in setup.cmake, e.g.
 *
 *    be_list(target-name AppName [SSH])
 *
 * This translates into the following command-line macro definitions
 * used by the code below:
 *
 *  - APPNAME should be defined to the name of the program, in
 *    user-facing capitalisation (e.g. PuTTY rather than putty).
 *    Unquoted: it's easier to stringify it in the preprocessor than
 *    to persuade cmake to put the right quotes on the command line on
 *    all build platforms.
 *
 *  - SSH should be defined to 1 if the two SSH backends (SSH proper,
 *    and bare-ssh-connection) should be added to the backends[] list,
 *    or 0 if they should not be. These are the only network protocol
 *    backends: Telnet, Rlogin, SUPDUP, Raw and serial-line support
 *    have been removed.
 */

#include <stdio.h>
#include "putty.h"

const char *const appname = STR(APPNAME);

/*
 * Define the default protocol for the application. Applications
 * without SSH (such as pterm) don't need this variable anyway, so
 * just set it to -1.
 */
#if SSH
const int be_default_protocol = PROT_SSH;
#else
const int be_default_protocol = -1;
#endif

/*
 * List all the configured backends, in the order they should appear
 * in the config box.
 */
const struct BackendVtable *const backends[] = {
    /* SSH is the remote-login protocol. */
#if SSH
    &ssh_backend,
#endif

    /*
     * Bare ssh-connection / PSUSAN is a niche protocol and goes well
     * down the list.
     */
#if SSH
    &sshconn_backend,
#endif

    /*
     * Done. Null pointer to mark the end of the list.
     */
    NULL
};

/*
 * Number of backends at the start of the above list that should have
 * radio buttons in the config UI.
 *
 * Only SSH gets a radio button; the rest (bare ssh-connection) are
 * relegated to a dropdown list.
 */
const size_t n_ui_backends =
    0
#if SSH
    + 1
#endif
    ;
