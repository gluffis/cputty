/*
 * cluster-layout.c: tiling the screen for cputty's "Arrange windows".
 * See cluster-layout.h.
 */

#include "putty.h"
#include "cluster-layout.h"

/* Width:height ratio we aim for in each tile: about that of an 80x24
 * terminal in a typical font */
#define TARGET_ASPECT 1.6

/* How far a tile's shape is from the target, as a ratio >= 1 */
static double shape_badness(double w, double h)
{
    double aspect = w / h;
    return aspect > TARGET_ASPECT ? aspect / TARGET_ASPECT :
        TARGET_ASPECT / aspect;
}

/*
 * Number of tiles in row r of 'rows': spread as evenly as possible,
 * with any extra ones in the last rows.
 */
static size_t tiles_in_row(size_t n, size_t rows, size_t r)
{
    size_t base = n / rows, extra = n % rows;
    return base + (r >= rows - extra ? 1 : 0);
}

/* Choose the number of rows whose worst-shaped tile is the best */
static size_t choose_rows(size_t n, int w, int h)
{
    size_t best = 1;
    double best_score = 1e300;

    for (size_t rows = 1; rows <= n; rows++) {
        double worst = 0;
        for (size_t r = 0; r < rows; r++) {
            double b = shape_badness((double)w / tiles_in_row(n, rows, r),
                                     (double)h / rows);
            if (b > worst)
                worst = b;
        }
        if (worst < best_score) {      /* on a tie, prefer fewer rows */
            best_score = worst;
            best = rows;
        }
    }
    return best;
}

void cluster_layout_tiles(size_t n, ClusterRect area, ClusterRect *tiles)
{
    size_t rows = choose_rows(n, area.w, area.h);
    size_t i = 0;

    for (size_t r = 0; r < rows; r++) {
        size_t in_row = tiles_in_row(n, rows, r);
        int y0 = area.y + (int)((long long)area.h * r / rows);
        int y1 = area.y + (int)((long long)area.h * (r + 1) / rows);

        for (size_t c = 0; c < in_row; c++, i++) {
            /* Compute both edges from the area, so that rounding
             * leaves no gaps or overlaps between neighbours */
            int x0 = area.x + (int)((long long)area.w * c / in_row);
            int x1 = area.x + (int)((long long)area.w * (c + 1) / in_row);
            tiles[i].x = x0;
            tiles[i].y = y0;
            tiles[i].w = x1 - x0;
            tiles[i].h = y1 - y0;
        }
    }
}
