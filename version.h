/*
 * This header file provides the various versioning-related #defines
 * for a particular PuTTY build.
 *
 * When my automated build system does a full build, Buildscr
 * completely overwrites this file with information derived from the
 * circumstances and type of that build. The information _here_ is
 * default stuff used for local development runs of 'make'.
 */

/*
 * cputty release version. (The SSH version string deliberately does
 * not use upstream's "Release-X.Y" form, so that servers applying
 * version-based workarounds don't mistake this for official PuTTY.)
 */
#define RELEASE 1.0.0
#define TEXTVER "cputty 1.0.0 (based on PuTTY 0.85)"
#define SSHVER "-cputty-1.0.0"
#define BINARY_VERSION 1,0,0,0
