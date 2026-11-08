/*
 * cluster-groups.h: named groups of hosts for cputty's cluster
 * console.
 *
 * Groups are kept in a plain text file, one group per line: the group
 * name followed by its hosts, separated by spaces or tabs, in the
 * style of the classic cssh 'clusters' file. Blank lines and lines
 * starting with '#' are ignored, as is anything after a word starting
 * with '#'.
 *
 * A word containing '=' is a setting for the group rather than a
 * host. The only one so far is arrange=yes (or no), which arranges
 * the windows when the group is opened. Unknown settings are ignored.
 *
 * Each host is anything cputty accepts as a host on its command line
 * (host, user@host, or the name of a saved session), optionally
 * followed by :port. Words starting with '-' are never accepted as
 * group names or hosts, so a groups file can't smuggle in command-line
 * options.
 */

#ifndef PUTTY_CLUSTER_GROUPS_H
#define PUTTY_CLUSTER_GROUPS_H

typedef struct ClusterGroup {
    char *name;
    char **hosts;
    size_t nhosts, hostsize;
    bool arrange;                      /* arrange=yes */
} ClusterGroup;

typedef struct ClusterGroups {
    ClusterGroup *groups;              /* in the order of the file */
    size_t ngroups, groupsize;
} ClusterGroups;

/* Parse the text of a groups file. Never fails: invalid words and
 * lines are skipped, and for a repeated group name the first line
 * wins. text may be NULL (no file). */
ClusterGroups *cluster_groups_parse(const char *text);
void cluster_groups_free(ClusterGroups *cg);
const ClusterGroup *cluster_groups_find(const ClusterGroups *cg,
                                        const char *name);

/* Whether a word is acceptable as a group name or a host. */
bool cluster_group_name_valid(const char *name);
bool cluster_host_spec_valid(const char *spec);

/* Copy of 'name' with leading and trailing white space removed. */
char *cluster_group_name_trim(const char *name);

/*
 * Return a new version of a groups file's text in which group 'name'
 * has the given hosts and settings: its line is replaced (or, if there
 * is none, appended). With nhosts == 0 the group is removed instead.
 * All other lines, comments included, are kept. text may be NULL (no
 * file yet), in which case the result starts with a comment
 * explaining the format. The result always uses LF line endings.
 */
char *cluster_groups_edit(const char *text, const char *name,
                          char *const *hosts, size_t nhosts, bool arrange);

/*
 * Host specs. cluster_host_spec_split separates an optional trailing
 * :port (an IPv6 address needs [brackets] around it for that), giving
 * a newly allocated host part and the port, or -1 if there was none.
 * cluster_host_spec_make builds a spec from a session's settings,
 * leaving the port out if it is the default.
 */
void cluster_host_spec_split(const char *spec, char **host_out,
                             int *port_out);
char *cluster_host_spec_make(const char *user, const char *host, int port,
                             int default_port);

/*
 * Storage, implemented in unix/storage.c and windows/storage.c.
 *
 * cluster_groups_path returns where the file lives (for showing to
 * the user), or NULL if that can't be worked out.
 *
 * cluster_groups_read returns the file's contents, or NULL if it
 * doesn't exist (with *errmsg NULL) or can't be read (with *errmsg
 * set to a message for the user).
 *
 * cluster_groups_write replaces the file's contents, creating its
 * directory if necessary. On failure it returns false and sets
 * *errmsg.
 */
char *cluster_groups_path(void);
char *cluster_groups_read(char **errmsg);
bool cluster_groups_write(const char *text, char **errmsg);

/*
 * Convenience wrappers on the above. cluster_groups_load reads and
 * parses the file, returning an empty set (and setting *errmsg) if it
 * can't be read. cluster_groups_store sets or removes one group in the
 * file, refusing to touch a file it couldn't read.
 */
ClusterGroups *cluster_groups_load(char **errmsg);
bool cluster_groups_store(const char *name, char *const *hosts,
                          size_t nhosts, bool arrange, char **errmsg);

#endif /* PUTTY_CLUSTER_GROUPS_H */
