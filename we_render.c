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
int wpe_scroll_dragging = 0;
