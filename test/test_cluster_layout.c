/*
 * Unit tests for cluster-layout.c (cputty's "Arrange windows" tiling).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "cluster-layout.h"

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

/* Tiles must cover the area exactly once: check every pixel row of a
 * coarse grid, plus that the last tile is at the bottom right. */
static void check_cover(size_t n, ClusterRect a)
{
    ClusterRect *t = snewn(n, ClusterRect);
    cluster_layout_tiles(n, a, t);

    long long total = 0;
    bool inside = true, positive = true;
    for (size_t i = 0; i < n; i++) {
        total += (long long)t[i].w * t[i].h;
        if (t[i].w <= 0 || t[i].h <= 0)
            positive = false;
        if (t[i].x < a.x || t[i].y < a.y || t[i].x + t[i].w > a.x + a.w ||
            t[i].y + t[i].h > a.y + a.h)
            inside = false;
    }
    bool overlap = false;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (t[i].x < t[j].x + t[j].w && t[j].x < t[i].x + t[i].w &&
                t[i].y < t[j].y + t[j].h && t[j].y < t[i].y + t[i].h)
                overlap = true;

    CHECK(positive);
    CHECK(inside);
    CHECK(!overlap);
    CHECK(total == (long long)a.w * a.h);   /* no gaps, given no overlap */
    CHECK(t[n - 1].x + t[n - 1].w == a.x + a.w);
    CHECK(t[n - 1].y + t[n - 1].h == a.y + a.h);
    sfree(t);
}

int main(void)
{
    ClusterRect wide = { 0, 27, 2560, 1413 };   /* 16:9-ish, below a panel */
    ClusterRect odd = { 5, 3, 1001, 767 };

    for (size_t n = 1; n <= 40; n++) {
        check_cover(n, wide);
        check_cover(n, odd);
    }

    ClusterRect t[5];

    /* One window fills the area */
    cluster_layout_tiles(1, wide, t);
    CHECK(t[0].x == 0 && t[0].y == 27 && t[0].w == 2560 && t[0].h == 1413);

    /* Two side by side */
    cluster_layout_tiles(2, wide, t);
    CHECK(t[0].y == t[1].y && t[0].w == 1280 && t[1].x == 1280);

    /* Four in a 2x2 grid */
    cluster_layout_tiles(4, wide, t);
    CHECK(t[0].w == 1280 && t[0].h == 706 && t[3].x == 1280 && t[3].y == 733);

    /* Five: two on top, three below (the extra one in the last row) */
    cluster_layout_tiles(5, wide, t);
    CHECK(t[0].y == t[1].y && t[2].y > t[0].y);
    CHECK(t[0].w == 1280 && t[1].w == 1280);
    CHECK(t[2].y == t[4].y && t[2].w == 853 && t[4].w == 854);

    /* Nine: a 3x3 grid, not a sparse last row */
    ClusterRect t9[9];
    cluster_layout_tiles(9, wide, t9);
    CHECK(t9[0].w == 853 && t9[3].y > t9[0].y && t9[6].y > t9[3].y);
    CHECK(t9[8].w == 854 && t9[6].y == t9[8].y);

    /* Seven: rows of 2, 2 and 3, so the last tile isn't stretched */
    ClusterRect t7[7];
    cluster_layout_tiles(7, wide, t7);
    CHECK(t7[0].w == 1280 && t7[2].w == 1280 && t7[4].w == 853);
    CHECK(t7[4].y == t7[6].y && t7[6].w == 854);

    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
