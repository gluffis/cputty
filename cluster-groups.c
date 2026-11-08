/*
 * cluster-groups.c: parsing and editing cputty's host groups file.
 * See cluster-groups.h for the file format.
 *
 * This file has no platform dependencies; reading and writing the
 * file itself is done by cluster_groups_read and cluster_groups_write
 * in each platform's storage.c.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "cluster-groups.h"

/* Longest group name or host spec we accept, in bytes */
#define CLUSTER_WORD_MAX 255

static const char cluster_groups_file_header[] =
    "# cputty host groups. Each line is a group name followed by its\n"
    "# hosts, separated by spaces. A host can be written as host,\n"
    "# user@host, host:port, or the name of a saved session. Adding\n"
    "# arrange=yes after the name arranges the windows when the group\n"
    "# is opened. Lines starting with # are comments. For example:\n"
    "#\n"
    "#   web   arrange=yes web1 web2 root@web3:2222\n"
    "#   db    db1 db2\n"
    "\n";

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* A word is valid if it is non-empty, not too long, doesn't start
 * with '-' (it would be taken as an option) or '#' (a comment), and
 * contains no white space or control characters. */
static bool word_valid(const char *word)
{
    size_t len = strlen(word);
    if (len == 0 || len > CLUSTER_WORD_MAX)
        return false;
    if (word[0] == '-' || word[0] == '#')
        return false;
    for (const char *p = word; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == 0x7F)
            return false;
    }
    return true;
}

bool cluster_group_name_valid(const char *name)
{
    return word_valid(name);
}

bool cluster_host_spec_valid(const char *spec)
{
    /* '=' would make it read back as a setting rather than a host */
    return word_valid(spec) && !strchr(spec, '=');
}

/* Case-insensitive comparison of the first n bytes (ASCII only) */
static bool same_nocase(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

/* Parse a yes/no setting value; returns false if it isn't one */
static bool parse_bool(const char *value, bool *out)
{
    static const char *const yes[] = { "yes", "true", "on", "1" };
    static const char *const no[] = { "no", "false", "off", "0" };
    for (size_t i = 0; i < lenof(yes); i++) {
        if (strlen(value) == strlen(yes[i]) &&
            same_nocase(value, yes[i], strlen(value))) {
            *out = true;
            return true;
        }
        if (strlen(value) == strlen(no[i]) &&
            same_nocase(value, no[i], strlen(value))) {
            *out = false;
            return true;
        }
    }
    return false;
}

/* Apply a key=value word to a group; unknown settings are ignored */
static void apply_setting(ClusterGroup *g, const char *word)
{
    const char *eq = strchr(word, '=');
    size_t keylen = eq - word;
    if (keylen == 7 && same_nocase(word, "arrange", 7))
        parse_bool(eq + 1, &g->arrange);
}

char *cluster_group_name_trim(const char *name)
{
    while (*name && isspace((unsigned char)*name))
        name++;
    size_t len = strlen(name);
    while (len > 0 && isspace((unsigned char)name[len - 1]))
        len--;
    return dupprintf("%.*s", (int)len, name);
}

static void free_group(ClusterGroup *g)
{
    sfree(g->name);
    for (size_t i = 0; i < g->nhosts; i++)
        sfree(g->hosts[i]);
    sfree(g->hosts);
}

void cluster_groups_free(ClusterGroups *cg)
{
    if (!cg)
        return;
    for (size_t i = 0; i < cg->ngroups; i++)
        free_group(&cg->groups[i]);
    sfree(cg->groups);
    sfree(cg);
}

const ClusterGroup *cluster_groups_find(const ClusterGroups *cg,
                                        const char *name)
{
    for (size_t i = 0; i < cg->ngroups; i++)
        if (!strcmp(cg->groups[i].name, name))
            return &cg->groups[i];
    return NULL;
}

/*
 * Find the next word in [*pp, end), stopping at a comment. Returns
 * false when there are no more words; otherwise sets *start and *len
 * and advances *pp past the word.
 */
static bool next_word(const char **pp, const char *end,
                      const char **start, size_t *len)
{
    const char *p = *pp;
    while (p < end && is_blank(*p))
        p++;
    if (p >= end || *p == '#')
        return false;
    const char *w = p;
    while (p < end && !is_blank(*p))
        p++;
    *start = w;
    *len = p - w;
    *pp = p;
    return true;
}

static void parse_line(ClusterGroups *cg, const char *p, const char *end)
{
    ClusterGroup g;
    memset(&g, 0, sizeof(g));
    const char *w;
    size_t len;

    while (next_word(&p, end, &w, &len)) {
        char *word = dupprintf("%.*s", (int)len, w);
        if (!g.name) {
            if (!cluster_group_name_valid(word) ||
                cluster_groups_find(cg, word)) {
                sfree(word);
                break;                 /* invalid or repeated: skip line */
            }
            g.name = word;
        } else if (strchr(word, '=')) {
            apply_setting(&g, word);
            sfree(word);
        } else if (cluster_host_spec_valid(word)) {
            sgrowarray(g.hosts, g.hostsize, g.nhosts);
            g.hosts[g.nhosts++] = word;
        } else {
            sfree(word);               /* skip just this host */
        }
    }

    if (g.name && g.nhosts > 0) {
        sgrowarray(cg->groups, cg->groupsize, cg->ngroups);
        cg->groups[cg->ngroups++] = g;
    } else {
        free_group(&g);
    }
}

ClusterGroups *cluster_groups_parse(const char *text)
{
    ClusterGroups *cg = snew(ClusterGroups);
    memset(cg, 0, sizeof(*cg));

    const char *p = text ? text : "";
    while (*p) {
        const char *eol = p + strcspn(p, "\n");
        parse_line(cg, p, eol);
        p = *eol ? eol + 1 : eol;
    }
    return cg;
}

/* Whether the line [p, end) defines the named group */
static bool line_defines_group(const char *p, const char *end,
                               const char *name)
{
    const char *w;
    size_t len;
    return next_word(&p, end, &w, &len) &&
        len == strlen(name) && !memcmp(w, name, len);
}

static void put_group_line(strbuf *out, const char *name,
                           char *const *hosts, size_t nhosts, bool arrange)
{
    put_dataz(out, name);
    if (arrange)
        put_dataz(out, " arrange=yes");
    for (size_t i = 0; i < nhosts; i++) {
        put_byte(out, ' ');
        put_dataz(out, hosts[i]);
    }
    put_byte(out, '\n');
}

char *cluster_groups_edit(const char *text, const char *name,
                          char *const *hosts, size_t nhosts, bool arrange)
{
    strbuf *out = strbuf_new();
    bool written = false;

    if (!text || !*text)
        put_dataz(out, cluster_groups_file_header);

    const char *p = text ? text : "";
    while (*p) {
        const char *eol = p + strcspn(p, "\n");
        if (line_defines_group(p, eol, name)) {
            /* Replace the first line for this group; drop any others */
            if (!written && nhosts > 0)
                put_group_line(out, name, hosts, nhosts, arrange);
            written = true;
        } else {
            const char *lineend = eol;
            if (lineend > p && lineend[-1] == '\r')
                lineend--;             /* normalise CRLF to LF */
            put_data(out, p, lineend - p);
            put_byte(out, '\n');
        }
        p = *eol ? eol + 1 : eol;
    }

    if (!written && nhosts > 0)
        put_group_line(out, name, hosts, nhosts, arrange);

    return strbuf_to_str(out);
}

void cluster_host_spec_split(const char *spec, char **host_out,
                             int *port_out)
{
    const char *at = strrchr(spec, '@');
    const char *hostpart = at ? at + 1 : spec;
    const char *colon = strrchr(hostpart, ':');

    *port_out = -1;
    if (colon && colon[1]) {
        size_t ndigits = strlen(colon + 1);
        bool bracketed = (hostpart[0] == '[' && colon - hostpart >= 3 &&
                          colon[-1] == ']');
        bool another_colon = memchr(hostpart, ':', colon - hostpart) != NULL;

        if (ndigits <= 5 && strspn(colon + 1, "0123456789") == ndigits &&
            (bracketed || (!another_colon && hostpart[0] != '['))) {
            int port = atoi(colon + 1);
            if (port >= 1 && port <= 65535) {
                if (bracketed) {
                    /* user@[v6addr]:port -> user@v6addr */
                    *host_out = dupprintf(
                        "%.*s%.*s", (int)(hostpart - spec), spec,
                        (int)(colon - hostpart - 2), hostpart + 1);
                } else {
                    *host_out = dupprintf("%.*s", (int)(colon - spec), spec);
                }
                *port_out = port;
                return;
            }
        }
    }
    *host_out = dupstr(spec);
}

char *cluster_host_spec_make(const char *user, const char *host, int port,
                             int default_port)
{
    strbuf *sb = strbuf_new();

    /* The host may already have a user@ prefix of its own */
    const char *at = strrchr(host, '@');
    if (at) {
        put_data(sb, host, at + 1 - host);
        host = at + 1;
    } else if (user && *user) {
        put_fmt(sb, "%s@", user);
    }

    if (port > 0 && port != default_port) {
        if (strchr(host, ':') && host[0] != '[')
            put_fmt(sb, "[%s]:%d", host, port);   /* IPv6 literal */
        else
            put_fmt(sb, "%s:%d", host, port);
    } else {
        put_dataz(sb, host);
    }
    return strbuf_to_str(sb);
}

ClusterGroups *cluster_groups_load(char **errmsg)
{
    *errmsg = NULL;
    char *text = cluster_groups_read(errmsg);
    ClusterGroups *cg = cluster_groups_parse(text);
    sfree(text);
    return cg;
}

bool cluster_groups_store(const char *name, char *const *hosts,
                          size_t nhosts, bool arrange, char **errmsg)
{
    *errmsg = NULL;
    char *text = cluster_groups_read(errmsg);
    if (*errmsg)
        return false;          /* don't overwrite a file we can't read */

    char *newtext = cluster_groups_edit(text, name, hosts, nhosts, arrange);
    bool ok = cluster_groups_write(newtext, errmsg);
    sfree(text);
    sfree(newtext);
    return ok;
}
