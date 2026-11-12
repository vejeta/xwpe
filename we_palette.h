/* we_palette.h -- the nearest entry of the editor's 16-colour palette */
/* Copyright (C) 2026 Juan Manuel Méndez Rey                           */
/* This is free software; see the file COPYING.                         */

#ifndef WE_PALETTE_H
#define WE_PALETTE_H

/* Entries of the editor's palette. */
#define WPE_PALETTE_SIZE 16

/**
 * wpe_palette_nearest - The palette entry closest to a colour.
 * @pal: the red, green and blue the display shows for each entry, 8 bits
 * @r: red of the colour wanted, 0..255
 * @g: green of the colour wanted, 0..255
 * @b: blue of the colour wanted, 0..255
 *
 * Wherever the editor must show a colour it has no cell for, it shows the
 * entry nearest to it by squared distance: a program's colours on the User
 * Screen, or a semantic-token colour and the dimmed scrollbar track on a
 * palette display.  The first of two equally near entries wins, so the
 * choice is the same every time.
 *
 * Return: the entry's index, 0..WPE_PALETTE_SIZE-1.
 */
static inline int wpe_palette_nearest(unsigned char pal[WPE_PALETTE_SIZE][3],
                                      int r, int g, int b)
{
 int best = 0, best_d = 1 << 30, i;

 for (i = 0; i < WPE_PALETTE_SIZE; i++)
 {
  int dr = r - pal[i][0];
  int dg = g - pal[i][1];
  int db = b - pal[i][2];
  int d = dr * dr + dg * dg + db * db;

  if (d < best_d)
  {
   best_d = d;
   best = i;
  }
 }
 return best;
}

#endif /* WE_PALETTE_H */
