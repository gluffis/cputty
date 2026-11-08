/*
 * cluster-layout.h: tiling the screen for cputty's "Arrange windows".
 */

#ifndef PUTTY_CLUSTER_LAYOUT_H
#define PUTTY_CLUSTER_LAYOUT_H

typedef struct ClusterRect {
    int x, y, w, h;
} ClusterRect;

/*
 * Divide 'area' into n tiles (n >= 1), written to tiles[0..n-1].
 *
 * The tiles are laid out in rows, each tile as wide as its row allows,
 * so there are no gaps. The tiles are spread over the rows as evenly
 * as possible (any extra ones going in the last rows), and the number
 * of rows is chosen so that the tiles come out roughly the shape of a
 * terminal window (a little wider than tall). Tiles are numbered left
 * to right and top to bottom, so the last one is at the bottom right.
 * Together they cover the area exactly, without overlapping.
 */
void cluster_layout_tiles(size_t n, ClusterRect area, ClusterRect *tiles);

#endif /* PUTTY_CLUSTER_LAYOUT_H */
