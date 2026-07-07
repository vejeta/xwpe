#ifndef WE_RENDER_GLYPHS_H
#define WE_RENDER_GLYPHS_H
/*-------------------------------------------------------------------------*\
  <we_render_glyphs.h> -- shared Cairo glyph geometry for xwpe

  The X11 (we_render_cairo.c) and Wayland (we_render_wayland.c) backends are
  deliberately self-contained -- each owns its cairo_t, surface and colour
  table -- but they must paint the SAME box-drawing / scrollbar glyphs and the
  SAME read-only padlock.  That geometry used to be duplicated by hand in both
  files with "keep identical" comments, which invites silent drift.

  These helpers are the single source of that geometry.  They are pure: each
  takes the target cairo_t and the foreground/background RGB triples, so they
  depend on neither backend's colour table nor its surface.  They are `static`
  (internal linkage) so including this header in both renderers adds no link
  dependency between them -- the self-containment is preserved at the object
  level, only the source is shared.
\*-------------------------------------------------------------------------*/
#include <cairo.h>
#include <math.h>

/* Paint box-drawing / scrollbar glyph `sc` (1..12): fill the cell with bg, then
   draw the frame/scrollbar strokes in fg.  Same rectangles the window frames and
   scrollbars have always used. */
static void wpe_glyph_acs(cairo_t *cr, int sc, int px, int py, int fw, int fh,
                          const double fg[3], const double bg[3])
{
 int lw = fw > 8 ? 2 : 1;
 int mx = px + fw / 2;
 int my = py + fh / 2;

 cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
 cairo_rectangle(cr, px, py, fw, fh);
 cairo_fill(cr);

 cairo_set_source_rgb(cr, fg[0], fg[1], fg[2]);
 switch (sc)
 {
 case 1: /* upper-left corner */
  cairo_rectangle(cr, mx, my, fw - fw/2, lw); cairo_fill(cr);
  cairo_rectangle(cr, mx, my, lw, fh - fh/2); cairo_fill(cr);
  break;
 case 2: /* upper-right corner */
  cairo_rectangle(cr, px, my, fw/2 + lw, lw); cairo_fill(cr);
  cairo_rectangle(cr, mx, my, lw, fh - fh/2); cairo_fill(cr);
  break;
 case 3: /* lower-left corner */
  cairo_rectangle(cr, mx, my, fw - fw/2, lw); cairo_fill(cr);
  cairo_rectangle(cr, mx, py, lw, fh/2 + lw); cairo_fill(cr);
  break;
 case 4: /* lower-right corner */
  cairo_rectangle(cr, px, my, fw/2 + lw, lw); cairo_fill(cr);
  cairo_rectangle(cr, mx, py, lw, fh/2 + lw); cairo_fill(cr);
  break;
 case 5: /* horizontal line */
  cairo_rectangle(cr, px, my, fw, lw); cairo_fill(cr);
  break;
 case 6: case 8: case 9: /* vertical line */
  cairo_rectangle(cr, mx, py, lw, fh); cairo_fill(cr);
  break;
 case 7: case 10: /* scrollbar track (stipple) */
  { int tx, ty;
    for (ty = py; ty < py + fh; ty += 2)
     for (tx = px; tx < px + fw; tx += 2)
      cairo_rectangle(cr, tx, ty, 1, 1);
    cairo_fill(cr);
  }
  break;
 case 11: /* scrollbar thumb */
  { int m = fw > 8 ? 2 : 1;
    cairo_rectangle(cr, px + m, py + m, fw - 2*m, fh - 2*m);
    cairo_fill(cr);
  }
  break;
 }
}

/* Read-only padlock as a crisp vector icon filling a cell_w x cell_h box: a
   rounded body, a stroked inverted-U shackle, and a keyhole punched out in bg.
   Used instead of the colour-emoji U+1F512, which renders at its own bitmap
   size and cannot be fit to a text cell. */
static void wpe_glyph_lock(cairo_t *cr, int px, int py, int cell_w, int cell_h,
                           const double fg[3], const double bg[3])
{
 double bw = cell_w * 0.60;               /* body width               */
 double bh = cell_h * 0.42;               /* body height              */
 double bx = px + (cell_w - bw) / 2.0;
 double by = py + cell_h - bh - cell_h * 0.10;
 double r  = bh * 0.24;                    /* body corner radius       */
 double sr = bw * 0.30;                    /* shackle radius           */
 double scx = px + cell_w / 2.0;           /* shackle / keyhole centre */
 double lw = cell_h * 0.11;                /* shackle thickness        */
 double kr = bh * 0.16;                    /* keyhole radius           */
 double ky = by + bh * 0.40;

 if (lw < 1.5) lw = 1.5;

 cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
 cairo_rectangle(cr, px, py, cell_w, cell_h);
 cairo_fill(cr);

 /* save/restore so the line width + round cap do not leak into later glyphs */
 cairo_save(cr);
 cairo_set_source_rgb(cr, fg[0], fg[1], fg[2]);

 cairo_set_line_width(cr, lw);
 cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
 cairo_new_path(cr);
 cairo_arc(cr, scx, by, sr, M_PI, 2.0 * M_PI);
 cairo_stroke(cr);

 cairo_new_path(cr);
 cairo_arc(cr, bx + r,      by + r,      r, M_PI,       1.5 * M_PI);
 cairo_arc(cr, bx + bw - r, by + r,      r, 1.5 * M_PI, 2.0 * M_PI);
 cairo_arc(cr, bx + bw - r, by + bh - r, r, 0.0,        0.5 * M_PI);
 cairo_arc(cr, bx + r,      by + bh - r, r, 0.5 * M_PI, M_PI);
 cairo_close_path(cr);
 cairo_fill(cr);

 cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
 cairo_new_path(cr);
 cairo_arc(cr, scx, ky, kr, 0.0, 2.0 * M_PI);
 cairo_fill(cr);
 cairo_rectangle(cr, scx - kr * 0.55, ky, kr * 1.1, bh * 0.32);
 cairo_fill(cr);

 cairo_restore(cr);
}

#endif /* WE_RENDER_GLYPHS_H */
