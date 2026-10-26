/* Redstream: the rdsmultistreamtiler layout (plugin-internal, not installed): the grid for a number of sources, the
 * cell rects and the scaling of object and display meta into a cell. Plain C with GLib only (the meta structs are
 * read from the libgstrds headers, nothing is linked): no GStreamer, no GPU.
 *
 * DeepStream 8.0 behaviour throughout. The output width and height are rounded up to a multiple of 4. A cell is
 * W / columns by H / rows (integer division), each rounded down to even; cell (r, c) sits at (c·cell_width,
 * r·cell_height), so a strip at the right and bottom may belong to no cell. Cells are indexed row-major by the
 * frame's pad_index.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TILER_LAYOUT_H
#define RDS_TILER_LAYOUT_H

#include <glib.h>
#include <rds/rds-meta.h>

G_BEGIN_DECLS

typedef struct
{
  guint left, top, width, height;
} RdsTilerRect;

typedef struct
{
  guint rows, columns;
  guint width, height;          /* the output, rounded up to a multiple of 4 */
  guint cell_width, cell_height;
} RdsTilerLayout;

/* @v rounded up to a multiple of 4 (saturating at the largest multiple of 4 a guint holds). */
guint rds_tiler_round_output_size (guint v);

/* The grid for @n sources (n = highest pad index + 1). With @square_seq_grid: ⌈√n⌉ × ⌈√n⌉, whatever @rows and
 * @columns say. Otherwise @rows × @columns while n fits, else ⌊√n⌋ rows of ⌈n / rows⌉ columns. n 0 counts as 1. */
void rds_tiler_grid (guint rows, guint columns, gboolean square_seq_grid, guint n, guint * out_rows,
    guint * out_columns);

/* Fills @layout for @n sources on a @width × @height output (the property values: rounded here). FALSE, with
 * @layout untouched, when a cell would be empty (narrower or lower than 2 pixels). */
gboolean rds_tiler_layout_init (RdsTilerLayout * layout, guint rows, guint columns, gboolean square_seq_grid,
    guint n, guint width, guint height);

/* The rect of cell @index (row-major). FALSE if the grid has no such cell. */
gboolean rds_tiler_layout_cell (const RdsTilerLayout * layout, guint index, RdsTilerRect * cell);

/* Maps a @frame_width × @frame_height frame onto @dst: x' = dst.left + x·sx, y' = dst.top + y·sy, with
 * sx = dst.width / frame_width and sy = dst.height / frame_height (each frame's own size). */
typedef struct
{
  gdouble sx, sy;
  gdouble ox, oy;
} RdsTilerScale;

void rds_tiler_scale_init (RdsTilerScale * scale, guint frame_width, guint frame_height, const RdsTilerRect * dst);

/* rect_params mapped as floats; text_params x/y_offset truncated to integers. Everything else is left as it is:
 * font_size, border_width, detector_bbox_info, tracker_bbox_info, mask_params, ids, class, confidence, labels. */
void rds_tiler_scale_object (const RdsTilerScale * scale, RdsObjectMeta * obj);

/* The first num_* entries (at most RDS_MAX_ELEMENTS_IN_DISPLAY_META) of each kind: rects as floats; text offsets,
 * line and arrow end points and circle centres truncated to integers; circle radius × min (sx, sy), truncated.
 * Widths and font sizes are left as they are. */
void rds_tiler_scale_display (const RdsTilerScale * scale, RdsDisplayMeta * display);

G_END_DECLS

#endif /* RDS_TILER_LAYOUT_H */
