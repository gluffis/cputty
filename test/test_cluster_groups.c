/*
 * Unit tests for cluster-groups.c (cputty's host groups file).
 *
 * The storage functions are replaced by an in-memory "file", so the
 * read-modify-write in cluster_groups_store is tested too.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "cluster-groups.h"

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int fails, passes;

#define CHECK(cond) do {                                                \
        if (cond) {                                                     \
            passes++;                                                   \
        } else {                                                        \
            fails++;                                                    \
            printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_STR(got, want) do {                                       \
        const char *got_ = (got), *want_ = (want);                      \
        if (got_ && !strcmp(got_, want_)) {                             \
            passes++;                                                   \
        } else {                                                        \
            fails++;                                                    \
            printf("%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
                   got_ ? got_ : "(null)", want_);                      \
        }                                                               \
    } while (0)

/* In-memory stand-in for the groups file */
static char *fake_file;                /* NULL = no file */
static bool fake_read_fails, fake_write_fails;

char *cluster_groups_path(void) { return dupstr("(test)"); }

char *cluster_groups_read(char **errmsg)
{
    *errmsg = NULL;
    if (fake_read_fails) {
        *errmsg = dupstr("read failed");
        return NULL;
    }
    return fake_file ? dupstr(fake_file) : NULL;
}

bool cluster_groups_write(const char *text, char **errmsg)
{
    if (fake_write_fails) {
        *errmsg = dupstr("write failed");
        return false;
    }
    sfree(fake_file);
    fake_file = dupstr(text);
    return true;
}

static void test_parse(void)
{
    ClusterGroups *cg = cluster_groups_parse(
        "# comment\n"
        "\n"
        "web web1 web2\troot@web3:2222   # trailing comment\r\n"
        "  db   db1  \n"
        "empty\n"                      /* no hosts: ignored */
        "web other1\n"                 /* repeat: first wins */
        "-bad host\n"                  /* name looks like an option */
        "#notagroup host\n"
        "opts -pw secret host9\n"      /* option-like hosts dropped */
        "last lasthost");              /* no final newline */

    CHECK(cg->ngroups == 4);
    if (cg->ngroups == 4) {
        CHECK_STR(cg->groups[0].name, "web");
        CHECK(cg->groups[0].nhosts == 3);
        CHECK_STR(cg->groups[0].hosts[1], "web2");
        CHECK_STR(cg->groups[0].hosts[2], "root@web3:2222");
        CHECK_STR(cg->groups[1].name, "db");
        CHECK(cg->groups[1].nhosts == 1);
        CHECK_STR(cg->groups[2].name, "opts");
        CHECK(cg->groups[2].nhosts == 2);
        CHECK_STR(cg->groups[2].hosts[0], "secret");
        CHECK_STR(cg->groups[2].hosts[1], "host9");
        CHECK_STR(cg->groups[3].name, "last");
    }
    CHECK(cluster_groups_find(cg, "db") == &cg->groups[1]);
    CHECK(cluster_groups_find(cg, "empty") == NULL);
    CHECK(cluster_groups_find(cg, "nosuch") == NULL);
    cluster_groups_free(cg);

    cg = cluster_groups_parse(NULL);
    CHECK(cg->ngroups == 0);
    cluster_groups_free(cg);
}

static void test_validity(void)
{
    CHECK(cluster_group_name_valid("web"));
    CHECK(cluster_group_name_valid("prod.eu-1"));
    CHECK(!cluster_group_name_valid(""));
    CHECK(!cluster_group_name_valid("two words"));
    CHECK(!cluster_group_name_valid("-x"));
    CHECK(!cluster_group_name_valid("#x"));
    CHECK(!cluster_group_name_valid("tab\there"));
    CHECK(cluster_host_spec_valid("root@[::1]:22"));
    CHECK(!cluster_host_spec_valid("-proxycmd"));

    char long_name[300];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(!cluster_group_name_valid(long_name));

    char *t = cluster_group_name_trim("  web \t");
    CHECK_STR(t, "web");
    sfree(t);
}

static void test_edit(void)
{
    char *hosts[] = { "h1", "user@h2:2200" };
    char *text;

    /* New file: header comment, then the group */
    text = cluster_groups_edit(NULL, "web", hosts, 2, false);
    CHECK(text[0] == '#');
    CHECK(strstr(text, "\nweb h1 user@h2:2200\n") != NULL);
    ClusterGroups *cg = cluster_groups_parse(text);
    CHECK(cg->ngroups == 1);       /* the example in the header is commented */
    cluster_groups_free(cg);
    sfree(text);

    /* Replace keeps comments and other groups in place; drops repeats;
     * normalises CRLF */
    text = cluster_groups_edit(
        "# mine\r\nweb old1\r\ndb d1\r\nweb old2\r\n", "web", hosts, 2, false);
    CHECK_STR(text, "# mine\nweb h1 user@h2:2200\ndb d1\n");
    sfree(text);

    /* Append when absent */
    text = cluster_groups_edit("db d1", "web", hosts, 1, false);
    CHECK_STR(text, "db d1\nweb h1\n");
    sfree(text);

    /* Remove */
    text = cluster_groups_edit("db d1\nweb w1\n# end\n", "web", NULL, 0, false);
    CHECK_STR(text, "db d1\n# end\n");
    sfree(text);

    /* A commented-out line with the same name is not the group */
    text = cluster_groups_edit("#web w1\n", "web", hosts, 1, false);
    CHECK_STR(text, "#web w1\nweb h1\n");
    sfree(text);
}

static void test_settings(void)
{
    ClusterGroups *cg = cluster_groups_parse(
        "web arrange=yes w1 w2\n"
        "db d1 ARRANGE=On\n"              /* anywhere, any case */
        "plain p1 arrange=no\n"
        "odd arrange=maybe o1 colour=red\n" /* bad value, unknown key */
        "only arrange=yes\n");             /* settings but no hosts */
    CHECK(cg->ngroups == 4);
    if (cg->ngroups == 4) {
        CHECK(cg->groups[0].arrange && cg->groups[0].nhosts == 2);
        CHECK(cg->groups[1].arrange && cg->groups[1].nhosts == 1);
        CHECK(!cg->groups[2].arrange && cg->groups[2].nhosts == 1);
        CHECK(!cg->groups[3].arrange && cg->groups[3].nhosts == 1);
    }
    cluster_groups_free(cg);

    /* A host containing '=' can't be saved (it would read back as a
     * setting) */
    CHECK(!cluster_host_spec_valid("a=b"));

    char *hosts[] = { "h1", "h2" };
    char *text = cluster_groups_edit("db d1\n", "web", hosts, 2, true);
    CHECK_STR(text, "db d1\nweb arrange=yes h1 h2\n");
    cg = cluster_groups_parse(text);
    CHECK(cg->ngroups == 2 && cg->groups[1].arrange &&
          cg->groups[1].nhosts == 2);
    cluster_groups_free(cg);
    sfree(text);

    /* Replacing without the setting drops it */
    text = cluster_groups_edit("web arrange=yes h1\n", "web", hosts, 1,
                               false);
    CHECK_STR(text, "web h1\n");
    sfree(text);

    /* The header for a new file explains the setting, but its example
     * is commented out */
    text = cluster_groups_edit(NULL, "x", hosts, 1, false);
    CHECK(strstr(text, "arrange=yes") != NULL);
    cg = cluster_groups_parse(text);
    CHECK(cg->ngroups == 1 && !cg->groups[0].arrange);
    cluster_groups_free(cg);
    sfree(text);
}

static void test_host_specs(void)
{
    static const struct {
        const char *spec, *host;
        int port;
    } cases[] = {
        { "web1", "web1", -1 },
        { "web1:2222", "web1", 2222 },
        { "root@web1:22", "root@web1", 22 },
        { "root@web1", "root@web1", -1 },
        { "::1", "::1", -1 },          /* bare IPv6: no port */
        { "[::1]:2200", "::1", 2200 },
        { "me@[fe80::1]:22", "me@fe80::1", 22 },
        { "[]:22", "[]:22", -1 },
        { "web1:0", "web1:0", -1 },
        { "web1:70000", "web1:70000", -1 },
        { "web1:12x", "web1:12x", -1 },
        { "mysession", "mysession", -1 },
    };

    for (size_t i = 0; i < lenof(cases); i++) {
        char *host;
        int port;
        cluster_host_spec_split(cases[i].spec, &host, &port);
        CHECK_STR(host, cases[i].host);
        CHECK(port == cases[i].port);
        sfree(host);
    }

    char *s;
    s = cluster_host_spec_make("root", "web1", 22, 22);
    CHECK_STR(s, "root@web1");
    sfree(s);
    s = cluster_host_spec_make("", "web1", 2222, 22);
    CHECK_STR(s, "web1:2222");
    sfree(s);
    s = cluster_host_spec_make("root", "admin@web1", 22, 22);
    CHECK_STR(s, "admin@web1");    /* the host's own user@ wins */
    sfree(s);
    s = cluster_host_spec_make(NULL, "fe80::1", 2200, 22);
    CHECK_STR(s, "[fe80::1]:2200");
    sfree(s);

    /* make and split agree */
    char *host;
    int port;
    s = cluster_host_spec_make("u", "fe80::1", 2200, 22);
    cluster_host_spec_split(s, &host, &port);
    CHECK_STR(host, "u@fe80::1");
    CHECK(port == 2200);
    sfree(host);
    sfree(s);
}

static void test_store(void)
{
    char *err;
    char *hosts[] = { "a1", "a2" };

    sfree(fake_file);
    fake_file = NULL;
    CHECK(cluster_groups_store("alpha", hosts, 2, false, &err));
    CHECK(err == NULL);
    ClusterGroups *cg = cluster_groups_load(&err);
    CHECK(err == NULL);
    CHECK(cg->ngroups == 1);
    cluster_groups_free(cg);

    /* A file that can't be read must not be overwritten */
    char *before = dupstr(fake_file);
    fake_read_fails = true;
    CHECK(!cluster_groups_store("beta", hosts, 1, false, &err));
    CHECK_STR(err, "read failed");
    sfree(err);
    CHECK_STR(fake_file, before);
    cg = cluster_groups_load(&err);  /* unreadable: empty set + error */
    CHECK(cg->ngroups == 0);
    CHECK(err != NULL);
    sfree(err);
    cluster_groups_free(cg);
    fake_read_fails = false;
    sfree(before);

    fake_write_fails = true;
    CHECK(!cluster_groups_store("beta", hosts, 1, false, &err));
    CHECK_STR(err, "write failed");
    sfree(err);
    fake_write_fails = false;

    CHECK(cluster_groups_store("alpha", NULL, 0, false, &err));
    cg = cluster_groups_load(&err);
    CHECK(cg->ngroups == 0);
    cluster_groups_free(cg);
    sfree(fake_file);
    fake_file = NULL;
}

int main(void)
{
    test_parse();
    test_validity();
    test_edit();
    test_settings();
    test_host_specs();
    test_store();
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
