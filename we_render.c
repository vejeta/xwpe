/* we_render.c -- Global rendering backend instance                */
/* Copyright (C) 2026 Juan Manuel Mendez Rey                      */
/* This is free software; see the file COPYING.                    */

#include "we_render.h"
#include <stddef.h>

WpeRenderBackend WpeRender = {
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
 0, 0, 0
};

int wpe_chrome_suppress = 0;
int wpe_modal_active = 0;     /* a transient modal box (dialog / picker / pulldown
                                 / Alt-Q menu) owns the screen: gate the fluid
                                 scrollbar overlay so it does not paint over the
                                 box, which is not a window the occlusion clip
                                 can see.  Kept in sync by e_lsp_modal_enter. */
int wpe_scroll_dragging = 0;
