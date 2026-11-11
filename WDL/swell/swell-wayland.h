/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.


  */


#pragma once

#include "swell.h"
#include <stdint.h>

static const int SWELL_WAYLAND_SCALE_ONE = 120;
static const int SWELL_WAYLAND_MAX_INTEGER_SCALE = 16;

static inline int swell_wayland_pixels(int logical, int scale)
{
  const int64_t product = (int64_t)logical * scale;
  const int half = SWELL_WAYLAND_SCALE_ONE / 2;
  return (int)((product + (product < 0 ? -half : half)) / SWELL_WAYLAND_SCALE_ONE);
}

static inline int swell_wayland_output_scale(int physical, int logical, int integer_scale)
{
  if (physical > 0 && logical > 0)
  {
    const int64_t scale = ((int64_t)physical * SWELL_WAYLAND_SCALE_ONE + logical / 2) / logical;
    if (scale > 0 && scale <= SWELL_WAYLAND_MAX_INTEGER_SCALE * SWELL_WAYLAND_SCALE_ONE) return (int)scale;
  }
  return integer_scale * SWELL_WAYLAND_SCALE_ONE;
}

struct wl_surface;
struct _cairo;
typedef struct _cairo cairo_t;

enum SWELL_WaylandSurfaceRole { SWELL_WAYLAND_TOPLEVEL, SWELL_WAYLAND_POPUP, SWELL_WAYLAND_SUBSURFACE, SWELL_WAYLAND_DRAG_ICON };
enum { SWELL_WAYLAND_MOUSE_ENTER = 0x2a1, SWELL_WAYLAND_MOUSE_LEAVE = 0x2a3 };
struct SWELL_WaylandView;
struct SWELL_WaylandViewCallbacks
{
  bool (*draw)(SWELL_WaylandView *, void *, cairo_t *);
  bool (*mouse)(SWELL_WaylandView *, void *, UINT, int, int, int, unsigned);
  bool (*key)(SWELL_WaylandView *, void *, bool, uint32_t, uint32_t, unsigned);
  void (*close)(SWELL_WaylandView *, void *);
  void (*focus)(SWELL_WaylandView *, void *, bool);
  void (*gl_reset)(SWELL_WaylandView *, void *);
  void (*resize)(SWELL_WaylandView *, void *, int, int);
};
SWELL_WaylandView *swell_wayland_view_create(HWND owner, SWELL_WaylandView *parent, const RECT *rect, SWELL_WaylandSurfaceRole role, const SWELL_WaylandViewCallbacks *callbacks, void *data);
void swell_wayland_view_destroy(SWELL_WaylandView *view);
void swell_wayland_view_reset_gl(SWELL_WaylandView *view);
void swell_wayland_view_cancel_input(SWELL_WaylandView *view);
void swell_wayland_view_move(SWELL_WaylandView *view, const RECT *rect);
void swell_wayland_view_show(SWELL_WaylandView *view, bool visible);
void swell_wayland_view_invalidate(SWELL_WaylandView *view, const RECT *rect = NULL);
void swell_wayland_view_focus(SWELL_WaylandView *view);
void swell_wayland_view_present(SWELL_WaylandView *view);
bool swell_wayland_view_has_pointer(SWELL_WaylandView *view, POINT *point = NULL);
void swell_wayland_view_set_cursor(SWELL_WaylandView *view, const char *name, const uint32_t *pixels, int width, int height, int hotspot_x, int hotspot_y);
void swell_wayland_view_return_key(SWELL_WaylandView *view, uint32_t code, unsigned state, bool down);
void swell_wayland_view_set_title(SWELL_WaylandView *view, const char *title);
void swell_wayland_view_fullscreen(SWELL_WaylandView *view);
void swell_wayland_view_set_decorated(SWELL_WaylandView *view, bool decorated);
void swell_wayland_view_set_size_limits(SWELL_WaylandView *view, int min_width, int min_height, int max_width, int max_height);
void swell_wayland_view_set_input(SWELL_WaylandView *view, bool enabled);
void swell_wayland_view_set_input_region(SWELL_WaylandView *view, const RECT *rects, int count);
void swell_wayland_view_rect(SWELL_WaylandView *view, RECT *rect);
void swell_wayland_view_output(SWELL_WaylandView *view, RECT *rect);
wl_surface *swell_wayland_view_surface(SWELL_WaylandView *view);
bool swell_wayland_first_uri_path(const char *data, size_t size, char *out, size_t capacity);
