/*
 * pds-nx: minimal overlay drawing (filled boxes and 8x8 bitmap text) on top
 * of the game picture. Coordinates are in a virtual 1280x720 screen.
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#pragma once
#include <stdint.h>

#define UI_W 1280
#define UI_H 720

bool ui_init(void);
/* Queue drawing; nothing reaches the screen until ui_flush(). */
void ui_rect(float x, float y, float w, float h, uint32_t rgba);
/* scale 1 = 8 px per character in the 1280x720 space. Returns the width. */
float ui_text(float x, float y, float scale, uint32_t rgba, const char *s);
float ui_text_width(float scale, const char *s);
/* Draw everything queued onto the current draw framebuffer (sw x sh pixels),
 * preserving the GL state the emulator relies on. */
void ui_flush(int sw, int sh);
