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

#if defined(SWELL_TARGET_WAYLAND) && !defined(SWELL_PROVIDED_BY_APP)
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <libdecor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <cairo.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "swell.h"
#include "swell-internal.h"
#include "swell-dlggen.h"
#include "swell-wayland.h"
#include "swell-platform.h"
#include "swell-xwayland-bridge.h"
#include "../assocarray.h"
#include "../wdlstring.h"
#include "../wdlcstring.h"
#include "../wdlutf8.h"
extern LRESULT SWELL_SendMouseMessage(HWND, UINT, WPARAM, LPARAM, int, int);
extern void SWELL_internalLICEpaint(HWND, LICE_IBitmap *, int, int, bool);
extern HWND GetFocusIncludeMenus();
extern bool PopupMenuIsActive();
extern HWND swell_window_wants_all_input();
extern int g_swell_ui_scale;
extern const char *g_swell_appname;
extern bool swell_is_likely_capslock;
struct SWELL_WaylandWindow;
static void transfer_registry(uint32_t, const char *, uint32_t);
static void transfer_seat_changed();
static void transfer_destroy_window(SWELL_WaylandWindow *);
static void transfer_dispatch();
static void transfer_init();
static bool transfer_from_point(POINT, HWND *);
static bool transfer_cursor_pos(POINT *);
static bool transfer_async_key_state(int, WORD *);
static bool transfer_drag_active();
static const char *transfer_cursor_name();
static void transfer_begin_drag(HWND);
static wl_display *display;
static wl_registry *registry;
static wl_compositor *compositor;
static wl_subcompositor *subcompositor;
static wl_shm *shm;
static xdg_wm_base *wm_base;
static wl_seat *seat;
static uint32_t seat_name;
static WDL_FastString seat_label;
static xdg_activation_v1 *activation;
static wp_viewporter *viewporter;
static zxdg_output_manager_v1 *output_manager;
static uint32_t output_manager_name;
static WDL_FastString application_id;
static WDL_FastString activation_token;
static libdecor *decor;
static wl_pointer *pointer;
static wp_cursor_shape_manager_v1 *cursor_manager;
static wp_cursor_shape_device_v1 *cursor_device;
static wl_keyboard *keyboard;
static wl_touch *touch;
static int touch_id = -1;
static SWELL_WaylandWindow *touch_window, *click_window;
static SWELL_WaylandWindow *pointer_window, *keyboard_window, *button_window;
static uint32_t input_serial, popup_serial, button_serial;
static unsigned modifiers;
static double pointer_x, pointer_y;
static bool pointer_motion_pending;
static double touch_x, touch_y;
static uint32_t pointer_serial;
static POINT cursor_position;
static SWELL_WaylandWindow *cursor_window;
static DWORD message_position;
static unsigned mouse_buttons, touch_buttons;
static bool initialized, disconnected, unsupported_protocol;
static int repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_key;
static guint repeat_source;
static xkb_context *key_context;
static xkb_keymap *keymap;
static xkb_state *key_state;
static xkb_compose_table *compose_table;
static xkb_compose_state *compose_state;
static RECT rect_union(RECT a, const RECT &b)
{
  if (b.left >= b.right || b.top >= b.bottom) return a;
  if (a.left >= a.right || a.top >= a.bottom) return b;
  a.left = min(a.left, b.left);
  a.top = min(a.top, b.top);
  a.right = max(a.right, b.right);
  a.bottom = max(a.bottom, b.bottom);
  return a;
}

static bool rect_empty(const RECT &r)
{
  return r.left >= r.right || r.top >= r.bottom;
}

static int ceil_scale(int v, int s)
{
  return v / s + (v % s > 0);
}

static int floor_scale(int v, int s)
{
  return v / s - (v % s < 0);
}

static const int max_window_size_pixels = 32768;
static const unsigned constrained_states =
    LIBDECOR_WINDOW_STATE_MAXIMIZED | LIBDECOR_WINDOW_STATE_FULLSCREEN | LIBDECOR_WINDOW_STATE_TILED_LEFT |
    LIBDECOR_WINDOW_STATE_TILED_RIGHT | LIBDECOR_WINDOW_STATE_TILED_TOP | LIBDECOR_WINDOW_STATE_TILED_BOTTOM;
class SWELL_WaylandBitmap : public LICE_IBitmap
{
  cairo_surface_t *surface = NULL;
  cairo_format_t format;

public:
  explicit SWELL_WaylandBitmap(cairo_format_t f = CAIRO_FORMAT_RGB24) : format(f) {}
  ~SWELL_WaylandBitmap()
  {
    if (surface) cairo_surface_destroy(surface);
  }
  LICE_pixel *getBits() { return surface ? (LICE_pixel *)cairo_image_surface_get_data(surface) : NULL; }
  int getWidth() { return surface ? cairo_image_surface_get_width(surface) : 0; }
  int getHeight() { return surface ? cairo_image_surface_get_height(surface) : 0; }
  int getRowSpan() { return surface ? cairo_image_surface_get_stride(surface) / 4 : 0; }
  bool isFlipped() { return false; }
  bool resize(int w, int h)
  {
    if (w <= 0 || h <= 0) w = h = 0;
    if (w == getWidth() && h == getHeight()) return false;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
    if (stride < 0 || (int64_t)stride * h > INT_MAX) return false;
    cairo_surface_t *next = cairo_image_surface_create(format, w, h);
    if (cairo_surface_status(next) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy(next);
      return false;
    }
    if (surface) cairo_surface_destroy(surface);
    surface = next;
    return true;
  }
  INT_PTR Extended(int id, void *data) { return id == 0xca140 && !data ? (INT_PTR)surface : 0; }
};

class SWELL_WaylandBitmapView : public LICE_WrapperBitmap
{
  cairo_surface_t *surface;

public:
  SWELL_WaylandBitmapView(LICE_IBitmap &bitmap)
      : LICE_WrapperBitmap(bitmap.getWidth() > 0 && bitmap.getHeight() > 0 ? bitmap.getBits() : NULL,
            bitmap.getWidth(), bitmap.getHeight(), bitmap.getRowSpan(), bitmap.isFlipped()),
        surface((cairo_surface_t *)bitmap.Extended(0xca140, NULL))
  {
    if (surface)
    {
      cairo_surface_reference(surface);
      cairo_surface_flush(surface);
    }
  }
  ~SWELL_WaylandBitmapView()
  {
    if (surface) cairo_surface_destroy(surface);
  }
  INT_PTR Extended(int id, void *data) { return id == 0xca140 && !data ? (INT_PTR)surface : 0; }
};

struct SWELL_WaylandBuffer
{
  wl_buffer *object = NULL;
  void *data = MAP_FAILED;
  size_t length = 0;
  int width = 0, height = 0, stride = 0;
  SWELL_WaylandWindow *owner = NULL;
  bool busy = false, retired = false;
  RECT stale = {};
  ~SWELL_WaylandBuffer()
  {
    if (object) wl_buffer_destroy(object);
    if (data != MAP_FAILED) munmap(data, length);
  }
};

struct SWELL_WaylandOutput
{
  wl_output *object;
  zxdg_output_v1 *xdg = NULL;
  uint32_t name;
  int x = 0, y = 0, width = 1024, height = 768, scale = 1, transform = WL_OUTPUT_TRANSFORM_NORMAL;
  RECT logical = {}, pending_logical = {};
  WDL_FastString label;
};

struct SWELL_WaylandWindow
{
  unsigned refs = 1;
  HWND hwnd = NULL;
  wl_surface *surface = NULL;
  xdg_surface *xdg = NULL;
  xdg_popup *popup = NULL;
  bool popup_grabbed = false;
  libdecor_frame *frame = NULL;
  wl_subsurface *subsurface = NULL;
  wp_viewport *viewport = NULL;
  wl_callback *callback = NULL;
  SWELL_WaylandWindow *parent = NULL;
  WDL_PtrList<SWELL_WaylandWindow> children;
  WDL_PtrList<SWELL_WaylandOutput> outputs;
  WDL_PtrList<SWELL_WaylandBuffer> buffers;
  int scale = 1, width = 1, height = 1, x = 0, y = 0;
  int normal_width = 1, normal_height = 1;
  int suggested_width = 0, suggested_height = 0;
  int frame_width = 0, frame_height = 0;
  int clip_x = 0, clip_y = 0;
  unsigned frame_state = 0;
  bool resize_pending = false, scale_known = false;
  SWELL_WaylandSurfaceRole role = SWELL_WAYLAND_TOPLEVEL;
  bool mapped = false, configured = false, dead = false, destroying = false, painting = false, pending = true,
       is_view = false;
  bool gl_failed = false, gl_external = false, focused = false;
  unsigned char opacity = 255;
  RECT invalid = {}, changed = {};
  SWELL_WaylandBitmap *view_bitmap = NULL;
  SWELL_WaylandViewCallbacks callbacks = {};
  void *data = NULL;
  wl_egl_window *egl_window = NULL;
  EGLSurface egl_surface = EGL_NO_SURFACE;
  EGLContext gl_context = EGL_NO_CONTEXT;
  GLuint texture = 0, framebuffer = 0;
  int texture_width = 0, texture_height = 0;
  double wheel[2] = {};
  unsigned wheel_source = 0, wheel_modifiers = 0;
  void retain() { ++refs; }
  void release()
  {
    if (!--refs) delete this;
  }
  virtual ~SWELL_WaylandWindow() { delete view_bitmap; }
};

struct SWELL_WaylandCursor
{
  WDL_FastString name;
  uint32_t shape = 0;
  SWELL_WaylandBuffer *buffer = NULL;
  int hotspot_x = 0, hotspot_y = 0;
};

struct SWELL_WaylandView : SWELL_WaylandWindow
{
  RECT size_limits = {}, gl_rect = {}, gl_clip = {};
  int gl_scale = 0;
  SWELL_WaylandCursor cursor;
  ~SWELL_WaylandView()
  {
    if (cursor.buffer)
    {
      cursor.buffer->retired = true;
      if (!cursor.buffer->busy) delete cursor.buffer;
    }
  }
};

static WDL_PtrList<SWELL_WaylandWindow> windows;
static WDL_PtrList<SWELL_WaylandOutput> outputs;
static int initial_scale()
{
  const int scale = !outputs.GetSize() ? 1 : outputs.Get(0)->scale;
  for (SWELL_WaylandOutput *output : outputs)
    if (output->scale != scale) return 1;
  return scale;
}

static guint paint_source;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLConfig egl_config;
static EGLContext egl_context = EGL_NO_CONTEXT;
static bool egl_attempted;
static void schedule(SWELL_WaylandWindow *);
static void destroy_native(SWELL_WaylandWindow *);
static void configure_window(SWELL_WaylandWindow *, int, int);
static void update_cursor();
static void cancel_read();
static void forget_gl_window(SWELL_WaylandWindow *);
static void destroy_gl_view(HWND);
static void restore_gl_views(HWND);
static void update_gl_visibility(HWND);
static bool refresh_gl_view(SWELL_WaylandView *);
static void request_size(SWELL_WaylandWindow *, int, int);
static SWELL_WaylandWindow *drag_icon;
static POINT drag_icon_offset;
static wl_surface *native_drag_icon();
struct SWELL_WaylandHWNDGuard
{
  HWND hwnd;
  SWELL_WaylandHWNDGuard(HWND h) : hwnd(h)
  {
    if (hwnd) hwnd->Retain();
  }
  ~SWELL_WaylandHWNDGuard()
  {
    if (hwnd) hwnd->Release();
  }
  bool valid() const { return hwnd && !hwnd->m_hashaddestroy; }
};

struct SWELL_WaylandWindowGuard
{
  SWELL_WaylandWindow *window;
  HWND hwnd;
  SWELL_WaylandWindowGuard(SWELL_WaylandWindow *w) : window(w), hwnd(w ? w->hwnd : NULL)
  {
    if (window) window->retain();
    if (hwnd) hwnd->Retain();
  }
  ~SWELL_WaylandWindowGuard()
  {
    if (hwnd) hwnd->Release();
    if (window) window->release();
  }
  bool valid() const { return window && !window->dead && (!hwnd || !hwnd->m_hashaddestroy); }
};

static SWELL_WaylandWindow *window_from_surface(wl_surface *surface)
{
  for (SWELL_WaylandWindow *window : windows)
    if (window->surface == surface) return window;
  return NULL;
}

static SWELL_WaylandWindow *window_from_hwnd(HWND hwnd)
{
  while (hwnd && !hwnd->m_oswindow) hwnd = hwnd->m_parent;
  return hwnd ? (SWELL_WaylandWindow *)hwnd->m_oswindow : NULL;
}

static RECT bounds(SWELL_WaylandWindow *w)
{
  RECT r = {0, 0, w->width * w->scale, w->height * w->scale};
  return r;
}

static void add_change(SWELL_WaylandWindow *w, const RECT &rect)
{
  RECT r, b = bounds(w);
  if (!IntersectRect(&r, &rect, &b)) return;
  w->changed = rect_union(w->changed, r);
  for (SWELL_WaylandBuffer *buffer : w->buffers) buffer->stale = rect_union(buffer->stale, r);
  w->pending = true;
  schedule(w);
}

static void retire_buffers(SWELL_WaylandWindow *w)
{
  for (SWELL_WaylandBuffer *buffer : w->buffers)
  {
    buffer->owner = NULL;
    buffer->retired = true;
    if (!buffer->busy) delete buffer;
  }
  w->buffers.Empty();
}

static void buffer_release_cb(void *data, wl_buffer *)
{
  SWELL_WaylandBuffer *released = (SWELL_WaylandBuffer *)data;
  released->busy = false;
  if (released->retired)
    delete released;
  else if (released->owner)
    schedule(released->owner);
}

static SWELL_WaylandBuffer *create_buffer(SWELL_WaylandWindow *w)
{
  const RECT b = bounds(w);
  if (b.right <= 0 || b.bottom <= 0 || (int64_t)b.right * 4 * b.bottom > INT_MAX) return NULL;
  int fd = memfd_create("swell", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return NULL;
  SWELL_WaylandBuffer *buffer = new SWELL_WaylandBuffer;
  buffer->width = b.right;
  buffer->height = b.bottom;
  buffer->stride = b.right * 4;
  buffer->length = (size_t)buffer->stride * buffer->height;
  if (ftruncate(fd, buffer->length) < 0)
  {
    close(fd);
    delete buffer;
    return NULL;
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
  buffer->data = mmap(NULL, buffer->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  wl_shm_pool *pool = buffer->data == MAP_FAILED ? NULL : wl_shm_create_pool(shm, fd, buffer->length);
  close(fd);
  if (!pool)
  {
    delete buffer;
    return NULL;
  }
  buffer->object = wl_shm_pool_create_buffer(pool, 0, buffer->width, buffer->height, buffer->stride,
      w->opacity == 255 && !w->callbacks.draw ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  if (!buffer->object)
  {
    delete buffer;
    return NULL;
  }
  static const wl_buffer_listener listener = {buffer_release_cb};
  wl_buffer_add_listener(buffer->object, &listener, buffer);
  buffer->owner = w;
  buffer->stale = b;
  w->buffers.Add(buffer);
  return buffer;
}

static SWELL_WaylandBuffer *available_buffer(SWELL_WaylandWindow *w)
{
  for (SWELL_WaylandBuffer *buffer : w->buffers)
    if (!buffer->busy) return buffer;
  return w->buffers.GetSize() < 2 ? create_buffer(w) : NULL;
}

static void clear_gl()
{
  cancel_read();
  Display *xdisplay = glXGetCurrentDisplay();
  if (xdisplay) glXMakeCurrent(xdisplay, None, NULL);
  EGLDisplay current = eglGetCurrentDisplay();
  if (current != EGL_NO_DISPLAY) eglMakeCurrent(current, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

struct SWELL_WaylandGLBinding
{
  EGLDisplay display = eglGetCurrentDisplay();
  EGLContext context = eglGetCurrentContext();
  EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
  EGLenum api = eglQueryAPI();
  Display *xdisplay = glXGetCurrentDisplay();
  GLXContext xcontext = glXGetCurrentContext();
  GLXDrawable xdraw = glXGetCurrentDrawable(), xread = glXGetCurrentReadDrawable();
  SWELL_WaylandWindow *window = NULL;
  SWELL_WaylandGLBinding()
  {
    for (SWELL_WaylandWindow *w : windows)
      if (w->egl_surface != EGL_NO_SURFACE && (w->egl_surface == draw || w->egl_surface == read))
      {
        window = w;
        w->retain();
        break;
      }
  }
  ~SWELL_WaylandGLBinding()
  {
    if (window && window->dead) context = EGL_NO_CONTEXT;
    if (eglGetCurrentContext() != context || glXGetCurrentContext() != xcontext ||
        eglGetCurrentSurface(EGL_DRAW) != draw || eglGetCurrentSurface(EGL_READ) != read ||
        eglQueryAPI() != api)
    {
      clear_gl();
      eglBindAPI(api);
      if (context != EGL_NO_CONTEXT) eglMakeCurrent(display, draw, read, context);
      if (xcontext) glXMakeContextCurrent(xdisplay, xdraw, xread, xcontext);
    }
    if (window) window->release();
  }
};

static bool init_egl()
{
  cancel_read();
  if (egl_attempted) return egl_context != EGL_NO_CONTEXT;
  egl_attempted = true;
  const char *mode = getenv("SWELL_WAYLAND_RENDERER");
  if (mode && !strcmp(mode, "shm")) return false;
  egl_display = eglGetDisplay((EGLNativeDisplayType)display);
  EGLint major, minor, count;
  if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &major, &minor) ||
      !eglBindAPI(EGL_OPENGL_API))
    return false;
  const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  if (!eglChooseConfig(egl_display, attributes, &egl_config, 1, &count) || !count) return false;
  const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
  return egl_context != EGL_NO_CONTEXT;
}

static bool make_current(SWELL_WaylandWindow *w)
{
  cancel_read();
  if (w->dead || w->gl_failed || !init_egl()) return false;
  if (!w->egl_window)
  {
    w->egl_window = wl_egl_window_create(w->surface, w->width * w->scale, w->height * w->scale);
    if (!w->egl_window) return false;
    w->egl_surface =
        eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)w->egl_window, NULL);
    if (w->egl_surface == EGL_NO_SURFACE) return false;
  }
  clear_gl();
  eglBindAPI(EGL_OPENGL_API);
  if (!eglMakeCurrent(egl_display, w->egl_surface, w->egl_surface,
          w->gl_context != EGL_NO_CONTEXT ? w->gl_context : egl_context))
    return false;
  eglSwapInterval(egl_display, 0);
  return true;
}

static void reset_gl(SWELL_WaylandWindow *w)
{
  SWELL_WaylandGLBinding binding;
  if (binding.context == w->gl_context && w->gl_context != EGL_NO_CONTEXT) binding.context = EGL_NO_CONTEXT;
  if (w->egl_surface != EGL_NO_SURFACE && make_current(w))
  {
    if (w->callbacks.gl_reset) w->callbacks.gl_reset((SWELL_WaylandView *)w, w->data);
    glDeleteFramebuffers(1, &w->framebuffer);
    glDeleteTextures(1, &w->texture);
  }
  clear_gl();
  if (w->gl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, w->gl_context);
  if (w->egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, w->egl_surface);
  if (w->egl_window) wl_egl_window_destroy(w->egl_window);
  w->gl_context = EGL_NO_CONTEXT;
  w->egl_surface = EGL_NO_SURFACE;
  w->egl_window = NULL;
  w->texture = w->framebuffer = 0;
  w->texture_width = w->texture_height = 0;
}

static cairo_surface_t *paint_cpu(SWELL_WaylandWindow *w)
{
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid()) return NULL;
  LICE_IBitmap *bitmap;
  if (w->callbacks.draw)
  {
    if (!w->view_bitmap) w->view_bitmap = new SWELL_WaylandBitmap(CAIRO_FORMAT_ARGB32);
    bitmap = w->view_bitmap;
  }
  else
  {
    if (!w->hwnd) return NULL;
    if (!w->hwnd->m_backingstore) w->hwnd->m_backingstore = new SWELL_WaylandBitmap;
    bitmap = w->hwnd->m_backingstore;
  }
  const RECT b = bounds(w);
  const bool resized = bitmap->resize(b.right, b.bottom);
  cairo_surface_t *surface = (cairo_surface_t *)bitmap->Extended(0xca140, NULL);
  if (!surface || bitmap->getWidth() != b.right || bitmap->getHeight() != b.bottom) return NULL;
  cairo_surface_reference(surface);
  RECT dirty = resized ? b : w->invalid;
  w->invalid = {};
  if (!rect_empty(dirty))
  {
    RECT clipped;
    if (IntersectRect(&clipped, &dirty, &b))
    {
      cairo_surface_flush(surface);
      if (w->callbacks.draw)
      {
        cairo_t *cr = cairo_create(surface);
        cairo_rectangle(
            cr, clipped.left, clipped.top, clipped.right - clipped.left, clipped.bottom - clipped.top);
        cairo_clip(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        w->callbacks.draw((SWELL_WaylandView *)w, w->data, cr);
        cairo_destroy(cr);
      }
      else
      {
        LICE_WrapperBitmap retained((LICE_pixel *)cairo_image_surface_get_data(surface), b.right, b.bottom,
            cairo_image_surface_get_stride(surface) / 4, false);
        LICE_SubBitmap part(
            &retained, clipped.left, clipped.top, clipped.right - clipped.left, clipped.bottom - clipped.top);
        SWELL_internalLICEpaint(w->hwnd, &part, clipped.left, clipped.top, resized);
      }
      cairo_surface_mark_dirty(surface);
      if (guard.valid()) add_change(w, clipped);
    }
  }
  if (!guard.valid() ||
      (!w->callbacks.draw &&
          (!w->hwnd->m_backingstore || w->hwnd->m_backingstore->Extended(0xca140, NULL) != (INT_PTR)surface)))
  {
    cairo_surface_destroy(surface);
    return NULL;
  }
  return surface;
}

static bool upload_gl(SWELL_WaylandWindow *w, cairo_surface_t *surface)
{
  const int width = cairo_image_surface_get_width(surface), height = cairo_image_surface_get_height(surface);
  if (!w->texture)
  {
    glGenTextures(1, &w->texture);
    glGenFramebuffers(1, &w->framebuffer);
  }
  glBindTexture(GL_TEXTURE_2D, w->texture);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  RECT dirty = w->changed;
  if (w->texture_width != width || w->texture_height != height)
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    w->texture_width = width;
    w->texture_height = height;
    dirty = bounds(w);
  }
  if (!rect_empty(dirty))
  {
    cairo_surface_flush(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, dirty.left, dirty.top, dirty.right - dirty.left,
        dirty.bottom - dirty.top, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
        cairo_image_surface_get_data(surface) + dirty.top * stride + dirty.left * 4);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, w->framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->texture, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glDrawBuffer(GL_BACK);
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  return glGetError() == GL_NO_ERROR;
}

static void finish_resize(SWELL_WaylandWindow *w);
static void frame_done_cb(void *data, wl_callback *callback, uint32_t)
{
  SWELL_WaylandWindow *window = (SWELL_WaylandWindow *)data;
  wl_callback_destroy(callback);
  window->callback = NULL;
  SWELL_WaylandWindowGuard guard(window);
  finish_resize(window);
  if (guard.valid()) schedule(window);
}

static void request_frame(SWELL_WaylandWindow *w)
{
  if (w->callback) return;
  static const wl_callback_listener listener = {frame_done_cb};
  w->callback = wl_surface_frame(w->surface);
  wl_callback_add_listener(w->callback, &listener, w);
}

static bool present_shm(SWELL_WaylandWindow *w, cairo_surface_t *surface)
{
  if (w->egl_window)
  {
    reset_gl(w);
    w->gl_failed = true;
    add_change(w, bounds(w));
  }
  SWELL_WaylandBuffer *buffer = available_buffer(w);
  if (!buffer) return false;
  cairo_surface_flush(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const unsigned char *source = cairo_image_surface_get_data(surface);
  const RECT dirty = buffer->stale;
  const bool alpha = cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32;
  for (int y = dirty.top; y < dirty.bottom; ++y)
  {
    const uint32_t *src = (const uint32_t *)(source + y * stride) + dirty.left;
    uint32_t *dest = (uint32_t *)((unsigned char *)buffer->data + y * buffer->stride) + dirty.left;
    if (w->opacity == 255)
      memcpy(dest, src, (dirty.right - dirty.left) * 4);
    else
      for (int x = dirty.left; x < dirty.right; ++x)
      {
        const uint32_t pixel = *src++, opacity = w->opacity;
        const unsigned a = alpha ? pixel >> 24 : 255;
        *dest++ =
            (((a * opacity + 127) / 255) << 24) | (((((pixel >> 16) & 255) * opacity + 127) / 255) << 16) |
            (((((pixel >> 8) & 255) * opacity + 127) / 255) << 8) | (((pixel & 255) * opacity + 127) / 255);
      }
  }
  buffer->stale = {};
  int x = 0, y = 0;
  if (w == drag_icon)
  {
    const POINT offset = {floor_scale(w->x, w->scale), floor_scale(w->y, w->scale)};
    x = offset.x - drag_icon_offset.x;
    y = offset.y - drag_icon_offset.y;
    drag_icon_offset = offset;
    if (wl_surface_get_version(w->surface) >= WL_SURFACE_OFFSET_SINCE_VERSION)
    {
      wl_surface_offset(w->surface, x, y);
      x = y = 0;
    }
  }
  wl_surface_attach(w->surface, buffer->object, x, y);
  const RECT damage = w->changed;
  if (!rect_empty(damage))
    wl_surface_damage_buffer(
        w->surface, damage.left, damage.top, damage.right - damage.left, damage.bottom - damage.top);
  buffer->busy = true;
  request_frame(w);
  wl_surface_commit(w->surface);
  return true;
}

static void commit_frame(SWELL_WaylandWindow *w, int width, int height,
    libdecor_configuration *configuration = NULL)
{
  w->frame_width = width;
  w->frame_height = height;
  libdecor_state *state = libdecor_state_new(width, height);
  libdecor_frame_commit(w->frame, state, configuration);
  libdecor_state_free(state);
}

static void commit_parent(SWELL_WaylandWindow *w)
{
  if (w->frame && w->pending)
    schedule(w);
  else if (!w->painting)
    wl_surface_commit(w->surface);
}

static void render(SWELL_WaylandWindow *w)
{
  if (w->dead || !w->mapped || !w->configured || w->callback || w->painting || !w->pending) return;
  SWELL_WaylandWindowGuard guard(w);
  SWELL_WaylandGLBinding binding;
  w->painting = true;
  if (w->frame && (w->frame_width != w->width || w->frame_height != w->height))
    commit_frame(w, w->width, w->height);
  bool presented = false, retry = false;
  cairo_surface_t *surface = NULL;
  if (w->callbacks.draw && !w->gl_failed && make_current(w))
  {
    RECT invalid = w->invalid;
    w->invalid = {};
    glViewport(0, 0, w->width * w->scale, w->height * w->scale);
    if (w->callbacks.draw((SWELL_WaylandView *)w, w->data, NULL) && guard.valid())
    {
      request_frame(w);
      presented = eglSwapBuffers(egl_display, w->egl_surface);
    }
    else if (guard.valid())
      w->invalid = rect_union(w->invalid, invalid);
    clear_gl();
    if (guard.valid() && !presented)
    {
      if (w->callback)
      {
        wl_callback_destroy(w->callback);
        w->callback = NULL;
      }
      reset_gl(w);
      w->gl_failed = true;
    }
  }
  if (guard.valid() && !presented && !w->gl_external)
  {
    surface = paint_cpu(w);
    if (surface && guard.valid() &&
        (cairo_image_surface_get_width(surface) != w->width * w->scale ||
            cairo_image_surface_get_height(surface) != w->height * w->scale))
    {
      cairo_surface_destroy(surface);
      surface = NULL;
      retry = true;
    }
    if (surface && guard.valid() && w != drag_icon && !w->callbacks.draw && w->opacity == 255 &&
        make_current(w))
    {
      if (upload_gl(w, surface))
      {
        request_frame(w);
        presented = eglSwapBuffers(egl_display, w->egl_surface);
      }
      clear_gl();
      if (!presented)
      {
        if (w->callback)
        {
          wl_callback_destroy(w->callback);
          w->callback = NULL;
        }
        reset_gl(w);
        w->gl_failed = true;
        add_change(w, bounds(w));
      }
    }
    if (surface && guard.valid() && !presented) presented = present_shm(w, surface);
  }
  if (surface) cairo_surface_destroy(surface);
  if (guard.valid() && presented)
  {
    w->changed = {};
    w->pending = !rect_empty(w->invalid);
    if (w->subsurface && w->parent && !w->parent->dead) commit_parent(w->parent);
  }
  w->painting = false;
  if (retry && guard.valid()) schedule(w);
  if (display) wl_display_flush(display);
}

static gboolean paint_pending(gpointer)
{
  paint_source = 0;
  WDL_PtrList<SWELL_WaylandWindow> pending = windows;
  for (SWELL_WaylandWindow *window : pending) window->retain();
  for (SWELL_WaylandWindow *window : pending)
  {
    render(window);
    window->release();
  }
  return G_SOURCE_REMOVE;
}

static void schedule(SWELL_WaylandWindow *w)
{
  if (disconnected || !w || w->dead || !w->mapped || !w->configured || w->callback || w->painting ||
      !w->pending || paint_source)
    return;
  paint_source = g_idle_add_full(G_PRIORITY_DEFAULT, paint_pending, NULL, NULL);
}

LICE_IBitmap *swell_oswindow_acquire_bitmap_wayland(LICE_IBitmap *bitmap, int x, int y, int width, int height)
{
  LICE_SubBitmap clipped(bitmap, x, y, width, height);
  return new SWELL_WaylandBitmapView(clipped);
}

void swell_oswindow_release_bitmap_wayland(HWND hwnd, LICE_IBitmap *bitmap, const RECT *dirty)
{
  cairo_surface_t *surface = bitmap ? (cairo_surface_t *)bitmap->Extended(0xca140, NULL) : NULL;
  if (surface && dirty)
  {
    cairo_surface_mark_dirty(surface);
    SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
    if (w && !w->dead && hwnd->m_backingstore &&
        hwnd->m_backingstore->Extended(0xca140, NULL) == (INT_PTR)surface)
      add_change(w, *dirty);
  }
  delete bitmap;
}

static void apply_view_size_limits(SWELL_WaylandView *view)
{
  if (!view->frame || view->resize_pending) return;
  const RECT &r = view->size_limits;
  if (r.left > 0 && r.top > 0 && r.right == r.left && r.bottom == r.top)
    libdecor_frame_unset_capabilities(view->frame, LIBDECOR_ACTION_RESIZE);
  else
    libdecor_frame_set_capabilities(view->frame, LIBDECOR_ACTION_RESIZE);
  libdecor_frame_set_min_content_size(
      view->frame, ceil_scale(r.left, view->scale), ceil_scale(r.top, view->scale));
  libdecor_frame_set_max_content_size(
      view->frame, ceil_scale(r.right, view->scale), ceil_scale(r.bottom, view->scale));
}

static void configure_window(SWELL_WaylandWindow *w, int width, int height)
{
  if (w->dead) return;
  SWELL_WaylandWindowGuard guard(w);
  width = max(1, min(width, max_window_size_pixels / w->scale));
  height = max(1, min(height, max_window_size_pixels / w->scale));
  const bool changed = w->width != width || w->height != height;
  if (changed && w->callback)
  {
    wl_callback_destroy(w->callback);
    w->callback = NULL;
  }
  w->width = width;
  w->height = height;
  w->configured = true;
  if (!w->is_view && w->opacity == 255)
  {
    wl_region *opaque = wl_compositor_create_region(compositor);
    wl_region_add(opaque, 0, 0, width, height);
    wl_surface_set_opaque_region(w->surface, opaque);
    wl_region_destroy(opaque);
  }
  if (changed || (w->buffers.GetSize() && (w->buffers.Get(0)->width != width * w->scale ||
                                             w->buffers.Get(0)->height != height * w->scale)))
    retire_buffers(w);
  if (w->egl_window) wl_egl_window_resize(w->egl_window, width * w->scale, height * w->scale, 0, 0);
  w->invalid = w->changed = bounds(w);
  add_change(w, w->invalid);
  if (w->hwnd && !w->is_view)
  {
    HWND hwnd = w->hwnd;
    const int physical_w = width * w->scale, physical_h = height * w->scale;
    bool resized = physical_w != hwnd->m_position.right - hwnd->m_position.left ||
                   physical_h != hwnd->m_position.bottom - hwnd->m_position.top;
    hwnd->m_position.right = hwnd->m_position.left + physical_w;
    hwnd->m_position.bottom = hwnd->m_position.top + physical_h;
    if (resized) SendMessage(hwnd, WM_SIZE, hwnd->m_is_maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, 0);
  }
  if (guard.valid() && w->hwnd && !w->is_view) update_gl_visibility(w->hwnd);
  if (guard.valid()) schedule(w);
}

static void set_scale(SWELL_WaylandWindow *w, int scale, bool first = false)
{
  scale = max(1, min(scale, 16));
  if (w->dead || w->scale == scale) return;
  SWELL_WaylandWindowGuard guard(w);
  const bool configured = w->configured;
  const bool initial = first || !w->scale_known;
  int width = w->is_view || initial ? ceil_scale(w->width * w->scale, scale) : w->width;
  int height = w->is_view || initial ? ceil_scale(w->height * w->scale, scale) : w->height;
  if (w->is_view)
  {
    const RECT &limits = ((SWELL_WaylandView *)w)->size_limits;
    width = max(width, ceil_scale(limits.left, scale));
    height = max(height, ceil_scale(limits.top, scale));
    if (limits.right) width = min(width, ceil_scale(limits.right, scale));
    if (limits.bottom) height = min(height, ceil_scale(limits.bottom, scale));
  }
  if (w->is_view || initial)
  {
    w->normal_width = ceil_scale(w->normal_width * w->scale, scale);
    w->normal_height = ceil_scale(w->normal_height * w->scale, scale);
  }
  w->scale = scale;
  if (w->is_view) apply_view_size_limits((SWELL_WaylandView *)w);
  wl_surface_set_buffer_scale(w->surface, scale);
  if (w->subsurface && w->parent)
    wl_subsurface_set_position(
        w->subsurface, floor_scale(w->x, w->parent->scale), floor_scale(w->y, w->parent->scale));
  retire_buffers(w);
  if (w->is_view || initial) request_size(w, width, height);
  configure_window(w, width, height);
  if (!guard.valid()) return;
  w->configured = configured;
  if (w->gl_external && !refresh_gl_view((SWELL_WaylandView *)w)) return;
  if (w == pointer_window) update_cursor();
  WDL_PtrList<SWELL_WaylandWindow> children = w->children;
  for (SWELL_WaylandWindow *child : children) child->retain();
  for (SWELL_WaylandWindow *child : children)
  {
    if (!child->dead && child->role != SWELL_WAYLAND_TOPLEVEL) set_scale(child, scale);
    child->release();
  }
}

static void update_scale(SWELL_WaylandWindow *w, bool entered = false)
{
  int scale = w->parent && w->role != SWELL_WAYLAND_TOPLEVEL ? w->parent->scale :
    !w->outputs.GetSize() ? w->scale : 1;
  for (SWELL_WaylandOutput *output : w->outputs) scale = max(scale, output->scale);
  const bool first = entered && !w->scale_known;
  if (entered) w->scale_known = true;
  set_scale(w, scale, first);
}

static void surface_enter_cb(void *data, wl_surface *, wl_output *output)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  for (SWELL_WaylandOutput *o : outputs)
    if (o->object == output && w->outputs.Find(o) < 0)
      w->outputs.Add(o);
  update_scale(w, true);
}

static void surface_leave_cb(void *data, wl_surface *, wl_output *output)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  for (int i = 0; i < w->outputs.GetSize(); ++i)
    if (w->outputs.Get(i)->object == output)
    {
      w->outputs.Delete(i);
      break;
    }
  update_scale(w);
}

static void surface_preferred_buffer_scale_cb(void *data, wl_surface *, int32_t scale)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  const bool first = !w->scale_known;
  w->scale_known = true;
  set_scale(w, scale, first);
}

static void surface_preferred_buffer_transform_cb(void *, wl_surface *, uint32_t) {}

static const wl_surface_listener surface_listener = {surface_enter_cb, surface_leave_cb,
    surface_preferred_buffer_scale_cb, surface_preferred_buffer_transform_cb};
static void close_native(SWELL_WaylandWindow *w)
{
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid()) return;
  if (w->hwnd && !w->is_view && xw_bridge_popup_blocks_close(w->hwnd)) return;
  if (w->callbacks.close)
    w->callbacks.close((SWELL_WaylandView *)w, w->data);
  else if (w->hwnd && IsWindowEnabled(w->hwnd) && !DestroyPopupMenus() &&
           !SendMessage(w->hwnd, WM_CLOSE, 0, 0))
    SendMessage(w->hwnd, WM_COMMAND, IDCANCEL, 0);
}

static void commit_size(SWELL_WaylandWindow *w)
{
  commit_frame(w, w->frame_width, w->frame_height);
  commit_parent(w);
}

static void finish_resize(SWELL_WaylandWindow *w)
{
  if (!w->resize_pending || !w->frame) return;
  SWELL_WaylandWindowGuard guard(w);
  w->resize_pending = false;
  if (w->is_view)
    apply_view_size_limits((SWELL_WaylandView *)w);
  else
    swell_recalcMinMaxInfo_wayland(w->hwnd);
  if (guard.valid() && w->is_view) commit_size(w);
}

static void constrain_size(SWELL_WaylandWindow *w, bool fixed = false)
{
  w->resize_pending = true;
  if (w->callback)
  {
    wl_callback_destroy(w->callback);
    w->callback = NULL;
  }
  int min_width = 1, min_height = 1;
  int max_width = max_window_size_pixels / w->scale, max_height = max_width;
  if (fixed)
  {
    min_width = max_width = w->normal_width;
    min_height = max_height = w->normal_height;
  }
  else
  {
    if (w->normal_width > w->width)
      min_width = w->normal_width;
    else if (w->normal_width < w->width)
      max_width = w->normal_width;
    if (w->normal_height > w->height)
      min_height = w->normal_height;
    else if (w->normal_height < w->height)
      max_height = w->normal_height;
  }
  libdecor_frame_set_min_content_size(w->frame, min_width, min_height);
  libdecor_frame_set_max_content_size(w->frame, max_width, max_height);
}

static void request_size(SWELL_WaylandWindow *w, int width, int height)
{
  w->normal_width = width;
  w->normal_height = height;
  if (!w->frame || !w->configured || (w->width == width && w->height == height)) return;
  constrain_size(w, (w->frame_state & constrained_states) != 0);
  commit_frame(w, width, height);
  if ((w->frame_state & constrained_states) &&
      !(w->frame_state & (LIBDECOR_WINDOW_STATE_MAXIMIZED | LIBDECOR_WINDOW_STATE_FULLSCREEN)) &&
      w->height > 1)
  {
    int x, y;
    libdecor_frame_translate_coordinate(w->frame, 0, 0, &x, &y);
    if (!x && !y)
    {
      xdg_surface_set_window_geometry(libdecor_frame_get_xdg_surface(w->frame), 0, 1, w->width, w->height - 1);
      wl_surface_commit(w->surface);
      commit_frame(w, width, height);
    }
  }
}

static void frame_configure_cb(libdecor_frame *frame, libdecor_configuration *configuration, void *data)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  SWELL_WaylandWindowGuard guard(w);
  if (!w->configured && !libdecor_frame_is_visible(frame)) libdecor_frame_set_visibility(frame, false);
  int width = w->width, height = w->height;
  int min_width = 0, min_height = 0, max_width = 0, max_height = 0;
  if (w->resize_pending)
  {
    libdecor_frame_get_min_content_size(frame, &min_width, &min_height);
    libdecor_frame_get_max_content_size(frame, &max_width, &max_height);
    libdecor_frame_set_min_content_size(frame, 0, 0);
    libdecor_frame_set_max_content_size(frame, 0, 0);
  }
  const bool has_size = libdecor_configuration_get_content_size(configuration, frame, &width, &height);
  if (w->resize_pending)
  {
    libdecor_frame_set_min_content_size(frame, min_width, min_height);
    libdecor_frame_set_max_content_size(frame, max_width, max_height);
  }
  const bool same_size = has_size && width == w->suggested_width && height == w->suggested_height;
  if (has_size)
  {
    w->suggested_width = width;
    w->suggested_height = height;
  }
  libdecor_window_state state = LIBDECOR_WINDOW_STATE_NONE;
  libdecor_configuration_get_window_state(configuration, &state);
  if (same_size && (w->frame_state ^ state) == LIBDECOR_WINDOW_STATE_ACTIVE)
  {
    width = w->width;
    height = w->height;
  }
  const bool constrained = (state & constrained_states) != 0;
  const bool restoring = !constrained && (w->frame_state & constrained_states);
  if (restoring && (width != w->normal_width || height != w->normal_height)) constrain_size(w);
  if (w->resize_pending || restoring ||
      (!(state & (LIBDECOR_WINDOW_STATE_MAXIMIZED | LIBDECOR_WINDOW_STATE_FULLSCREEN)) &&
          w->hwnd && !w->is_view && !(w->hwnd->m_style & WS_THICKFRAME)))
  {
    width = w->normal_width;
    height = w->normal_height;
  }
  else if (!constrained)
  {
    w->normal_width = width;
    w->normal_height = height;
  }
  const bool defer = w->configured && w->frame_state == (unsigned)state;
  w->frame_state = state;
  w->pending = true;
  if (w->hwnd && !w->is_view) w->hwnd->m_is_maximized = !!(state & LIBDECOR_WINDOW_STATE_MAXIMIZED);
  commit_frame(w, defer ? w->frame_width : max(1, width),
      defer ? w->frame_height : max(1, height), configuration);
  const bool resized = w->width != width || w->height != height;
  if (guard.valid()) configure_window(w, width, height);
  if (resized && guard.valid() && w->callbacks.resize)
  {
    const RECT &limits = ((SWELL_WaylandView *)w)->size_limits;
    int physical_width = w->width * w->scale, physical_height = w->height * w->scale;
    if (limits.right) physical_width = min(physical_width, limits.right);
    if (limits.bottom) physical_height = min(physical_height, limits.bottom);
    w->callbacks.resize((SWELL_WaylandView *)w, w->data, physical_width, physical_height);
  }
}

static gboolean frame_close_idle_cb(gpointer data)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  if (!w->dead) close_native(w);
  w->release();
  return G_SOURCE_REMOVE;
}

static void frame_close_cb(libdecor_frame *, void *data)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  w->retain();
  g_idle_add(frame_close_idle_cb, w);
}

static void frame_commit_cb(libdecor_frame *, void *data)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  if (!w->dead && w->surface) commit_parent(w);
}

static void frame_dismiss_popup_cb(libdecor_frame *, const char *, void *)
{
  DestroyPopupMenus();
}

static const libdecor_frame_interface frame_interface = {frame_configure_cb, frame_close_cb, frame_commit_cb,
    frame_dismiss_popup_cb, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static void xdg_surface_configure_cb(void *data, xdg_surface *surface, uint32_t serial)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  xdg_surface_ack_configure(surface, serial);
  configure_window(w, w->width, w->height);
}

static const xdg_surface_listener xdg_listener = {xdg_surface_configure_cb};
static void popup_configure_cb(void *data, xdg_popup *, int32_t x, int32_t y, int32_t width, int32_t height)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  if (w->parent && w->parent->frame)
  {
    int frame_x, frame_y;
    libdecor_frame_translate_coordinate(w->parent->frame, 0, 0, &frame_x, &frame_y);
    x -= frame_x;
    y -= frame_y;
  }
  w->x = x * w->scale;
  w->y = y * w->scale;
  w->width = max(1, width);
  w->height = max(1, height);
  if (w->hwnd && !w->is_view && w->parent)
  {
    const int parent_x = w->parent->hwnd ? w->parent->hwnd->m_position.left : 0;
    const int parent_y = w->parent->hwnd ? w->parent->hwnd->m_position.top : 0;
    w->hwnd->m_position.left = parent_x + w->x;
    w->hwnd->m_position.top = parent_y + w->y;
  }
}

static void popup_done_cb(void *data, xdg_popup *)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)data;
  SWELL_WaylandWindowGuard guard(w);
  if (w->callbacks.close)
    w->callbacks.close((SWELL_WaylandView *)w, w->data);
  else if (w->hwnd)
  {
    if (!DestroyPopupMenus()) ShowWindow(w->hwnd, SW_HIDE);
  }
}

static void popup_repositioned_cb(void *, xdg_popup *, uint32_t) {}

static const xdg_popup_listener popup_listener = {popup_configure_cb, popup_done_cb, popup_repositioned_cb};
static xdg_surface *role_surface(SWELL_WaylandWindow *w)
{
  return w ? (w->frame ? libdecor_frame_get_xdg_surface(w->frame) : w->xdg) : NULL;
}

static void link_parent(SWELL_WaylandWindow *w, SWELL_WaylandWindow *parent)
{
  w->parent = parent;
  if (parent)
  {
    parent->children.Add(w);
    w->scale = parent->scale;
  }
}

static POINT window_origin(SWELL_WaylandWindow *w)
{
  POINT point = {};
  if (w->parent && w->is_view)
  {
    point = window_origin(w->parent);
    point.x += w->x;
    point.y += w->y;
  }
  else if (w->hwnd)
  {
    point.x = w->hwnd->m_position.left;
    point.y = w->hwnd->m_position.top;
  }
  return point;
}

static xdg_positioner *popup_positioner(SWELL_WaylandWindow *w)
{
  RECT anchor = {w->x, w->y, w->x, w->y};
  uint32_t alignment = XDG_POSITIONER_ANCHOR_TOP_LEFT;
  uint32_t constraints = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                         XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                         XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
  if (w->is_view)
  {
    anchor = {};
    constraints &= ~XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
  }
  else if (swell_menu_get_anchor(w->hwnd, &anchor))
  {
    const POINT origin = window_origin(w->parent);
    OffsetRect(&anchor, -origin.x, -origin.y);
    if (anchor.right > anchor.left)
      alignment = XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    else if (anchor.bottom > anchor.top)
      alignment = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    constraints |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
  }
  int x = floor_scale(anchor.left, w->scale), y = floor_scale(anchor.top, w->scale);
  const int width = max(1, ceil_scale(anchor.right, w->scale) - x);
  const int height = max(1, ceil_scale(anchor.bottom, w->scale) - y);
  if (w->parent->frame) libdecor_frame_translate_coordinate(w->parent->frame, x, y, &x, &y);
  xdg_positioner *positioner = xdg_wm_base_create_positioner(wm_base);
  xdg_positioner_set_size(positioner, w->width, w->height);
  xdg_positioner_set_anchor_rect(positioner, x, y, width, height);
  xdg_positioner_set_anchor(positioner, alignment);
  xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  xdg_positioner_set_constraint_adjustment(positioner, constraints);
  if (w->is_view && xdg_positioner_get_version(positioner) >= XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION)
    xdg_positioner_set_reactive(positioner);
  if (w->is_view)
    xdg_positioner_set_offset(positioner, floor_scale(w->x, w->scale), floor_scale(w->y, w->scale));
  return positioner;
}

static void init_application_id()
{
  if (application_id.GetLength()) return;
  application_id.Set(g_swell_appname && *g_swell_appname ? g_swell_appname : "swell");
  if (!g_swell_appname) return;
  GList *apps = g_app_info_get_all();
  for (GList *p = apps; p; p = p->next)
  {
    if (!G_IS_DESKTOP_APP_INFO(p->data)) continue;
    const char *wmclass = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(p->data));
    const char *id = g_app_info_get_id(G_APP_INFO(p->data));
    if (wmclass && !strcmp(wmclass, g_swell_appname) && id && strlen(id) > 8 &&
        g_str_has_suffix(id, ".desktop"))
    {
      application_id.SetRaw(id, strlen(id) - 8);
      break;
    }
  }
  g_list_free_full(apps, g_object_unref);
}

static bool create_surface(SWELL_WaylandWindow *w, SWELL_WaylandWindow *parent, bool grab)
{
  w->normal_width = w->width;
  w->normal_height = w->height;
  w->surface = wl_compositor_create_surface(compositor);
  if (!w->surface) return false;
  wl_surface_add_listener(w->surface, &surface_listener, w);
  link_parent(w, parent);
  wl_surface_set_buffer_scale(w->surface, w->scale);
  windows.Add(w);
  if (w->role == SWELL_WAYLAND_SUBSURFACE)
  {
    if (!parent || !subcompositor) return false;
    w->subsurface = wl_subcompositor_get_subsurface(subcompositor, w->surface, parent->surface);
    wl_subsurface_set_position(
        w->subsurface, floor_scale(w->x, parent->scale), floor_scale(w->y, parent->scale));
    wl_subsurface_set_desync(w->subsurface);
    w->configured = true;
  }
  else if (w->role == SWELL_WAYLAND_POPUP)
  {
    xdg_surface *parent_xdg = role_surface(parent);
    if (!parent_xdg) return false;
    w->xdg = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
    xdg_surface_add_listener(w->xdg, &xdg_listener, w);
    xdg_positioner *positioner = popup_positioner(w);
    w->popup = xdg_surface_get_popup(w->xdg, parent_xdg, positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(w->popup, &popup_listener, w);
    if (grab && seat && popup_serial && (!parent->popup || parent->popup_grabbed))
    {
      if (parent->frame && seat_label.GetLength())
        libdecor_frame_popup_grab(parent->frame, seat_label.Get());
      xdg_popup_grab(w->popup, seat, popup_serial);
      w->popup_grabbed = true;
    }
    wl_surface_commit(w->surface);
  }
  else if (w->role == SWELL_WAYLAND_DRAG_ICON)
    w->configured = true;
  else
  {
    w->frame =
        libdecor_decorate(decor, w->surface, const_cast<libdecor_frame_interface *>(&frame_interface), w);
    if (!w->frame) return false;
    init_application_id();
    libdecor_frame_set_app_id(w->frame, application_id.Get());
    libdecor_frame_set_title(w->frame, w->hwnd ? w->hwnd->m_title.Get() : "");
    if (parent && parent->frame) libdecor_frame_set_parent(w->frame, parent->frame);
    if (w->hwnd && !w->is_view && !(w->hwnd->m_style & WS_CAPTION))
      libdecor_frame_set_visibility(w->frame, false);
    libdecor_frame_map(w->frame);
  }
  w->mapped = true;
  w->invalid = bounds(w);
  add_change(w, w->invalid);
  return true;
}

static void destroy_native(SWELL_WaylandWindow *w)
{
  if (!w || w->dead || w->destroying) return;
  SWELL_WaylandWindowGuard guard(w);
  w->destroying = true;
  if (w->hwnd && !w->is_view)
  {
    if (!w->hwnd->m_hashaddestroy) SetProp(w->hwnd, "swell-native-scale", (HANDLE)(INT_PTR)w->scale);
    xw_bridge_close_owned(w->hwnd);
    xw_bridge_detach_children(w->hwnd);
  }
  if (SWELL_focused_oswindow == w) SWELL_focused_oswindow = NULL;
  if (w->callback)
  {
    wl_callback_destroy(w->callback);
    w->callback = NULL;
  }
  transfer_destroy_window(w);
  if (pointer_window == w) pointer_window = NULL;
  if (click_window == w) click_window = NULL;
  if (cursor_window == w) cursor_window = NULL;
  if (button_window == w)
  {
    button_window = NULL;
    button_serial = 0;
    if (touch_window != w) mouse_buttons = 0;
  }
  if (touch_window == w)
  {
    touch_window = NULL;
    touch_id = -1;
    touch_buttons = 0;
  }
  if (keyboard_window == w)
  {
    keyboard_window = w->popup_grabbed && w->parent && !w->parent->destroying ? w->parent : NULL;
    if (repeat_source)
    {
      g_source_remove(repeat_source);
      repeat_source = 0;
    }
  }
  WDL_PtrList<SWELL_WaylandWindow> children = w->children;
  for (SWELL_WaylandWindow *child : children) child->retain();
  for (SWELL_WaylandWindow *child : children)
  {
    destroy_native(child);
    child->release();
  }
  forget_gl_window(w);
  reset_gl(w);
  w->dead = true;
  w->mapped = false;
  retire_buffers(w);
  if (w->parent)
  {
    auto &siblings = w->parent->children;
    siblings.DeletePtr(w);
    if (w->popup_grabbed && w->parent->frame && seat_label.GetLength())
      libdecor_frame_popup_ungrab(w->parent->frame, seat_label.Get());
  }
  if (w->viewport) wp_viewport_destroy(w->viewport);
  if (w->subsurface) wl_subsurface_destroy(w->subsurface);
  if (w->popup) xdg_popup_destroy(w->popup);
  if (w->xdg) xdg_surface_destroy(w->xdg);
  if (w->frame) libdecor_frame_unref(w->frame);
  if (w->surface) wl_surface_destroy(w->surface);
  windows.DeletePtr(w);
  if (w->hwnd && w->hwnd->m_oswindow == w) w->hwnd->m_oswindow = NULL;
  w->surface = NULL;
  w->parent = NULL;
  w->release();
}

static void dispatch_mouse(SWELL_WaylandWindow *w, UINT message, int x, int y, WPARAM param)
{
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid()) return;
  POINT global = window_origin(w);
  global.x += x;
  global.y += y;
  cursor_position = global;
  cursor_window = w;
  message_position = MAKELONG(global.x & 65535, global.y & 65535);
  if (w->callbacks.mouse &&
      w->callbacks.mouse((SWELL_WaylandView *)w, w->data, message, x, y,
          (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ? (short)HIWORD(param) : 0,
          modifiers | mouse_buttons | touch_buttons))
    return;
  if (!guard.valid() || !w->hwnd ||
      message == SWELL_WAYLAND_MOUSE_ENTER || message == SWELL_WAYLAND_MOUSE_LEAVE) return;
  if (message == WM_MOUSEHWHEEL) param = MAKEWPARAM(LOWORD(param), -(short)HIWORD(param));
  const bool pressed = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN;
  const bool released = message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP;
  if (pressed && xw_bridge_swell_on_button_event_escape()) return;
  HWND target = GetCapture();
  if (target && message == WM_MOUSEMOVE && (mouse_buttons & 256) &&
      (x < 0 || y < 0 || x >= w->width * w->scale || y >= w->height * w->scale) &&
      target->m_classname && !strcmp(target->m_classname, "WDLTabCtrl") &&
      target->m_parent && !strcmp(target->m_parent->m_title.Get(), "REAPER_dock"))
  {
    transfer_begin_drag(NULL);
    if (transfer_drag_active()) return;
  }
  if (!target)
  {
    POINT local = {x + w->clip_x, y + w->clip_y};
    target = swell_window_wants_all_input() == w->hwnd ? w->hwnd : ChildWindowFromPoint(w->hwnd, local);
  }
  if (!target) return;
  target->Retain();
  POINT local = global;
  if (message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) ScreenToClient(target, &local);
  if (pressed && IsWindowEnabled(target)) SendMessage(target, WM_MOUSEACTIVATE, 0, 0);
  if (guard.valid() && (pressed || (released && PopupMenuIsActive())))
    SWELL_focused_oswindow = window_from_hwnd(w->hwnd);
  if (!target->m_hashaddestroy && guard.valid())
    SWELL_SendMouseMessage(target, message, param, MAKELPARAM(local.x, local.y), global.x, global.y);
  if (guard.valid() && !target->m_hashaddestroy && message >= WM_LBUTTONDOWN && message <= WM_MBUTTONDBLCLK)
    swell_im_update_candidates_location();
  target->Release();
}

static void notify_view_focus(SWELL_WaylandWindow *w, bool focused)
{
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid() || w->focused == focused) return;
  w->focused = focused;
  if (w->callbacks.focus) w->callbacks.focus((SWELL_WaylandView *)w, w->data, focused);
  if (!guard.valid()) return;
  WDL_PtrList<SWELL_WaylandWindow> children = w->children;
  for (SWELL_WaylandWindow *child : children) child->retain();
  for (SWELL_WaylandWindow *child : children)
  {
    if (child->subsurface && child->mapped) notify_view_focus(child, focused);
    child->release();
  }
}

static bool app_inactive;
static UINT_PTR app_deactivate_timer;

static void set_app_active(bool active)
{
  if (app_deactivate_timer)
  {
    KillTimer(NULL, app_deactivate_timer);
    app_deactivate_timer = 0;
  }
  if (app_inactive == !active) return;
  app_inactive = !active;
  for (HWND h = SWELL_topwindows; h; h = h->m_next) PostMessage(h, WM_ACTIVATEAPP, active, 0);
  if (!active) swell_on_toplevel_raise(NULL);
}

static void app_deactivate_cb(HWND, UINT, UINT_PTR, DWORD)
{
  set_app_active(keyboard_window != NULL);
}

static void update_app_focus()
{
  if (keyboard_window) set_app_active(true);
  else if (!app_inactive && !app_deactivate_timer)
    app_deactivate_timer = SetTimer(NULL, 0, 200, app_deactivate_cb);
}

static void set_focus(SWELL_WaylandWindow *w, bool focused)
{
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid()) return;
  update_app_focus();
  notify_view_focus(w, focused);
  if (!guard.valid()) return;
  SWELL_WaylandWindow *root = w;
  while (root->is_view && root->parent) root = root->parent;
  if (focused)
  {
    SWELL_WaylandWindow *focus = (SWELL_WaylandWindow *)SWELL_focused_oswindow;
    while (focus && focus != root && focus->popup) focus = focus->parent;
    if (focus != root) SWELL_focused_oswindow = root;
    swell_on_toplevel_raise(root);
  }
  else if (SWELL_focused_oswindow == root && !root->popup)
    SWELL_focused_oswindow = NULL;
}

static unsigned key_modifiers()
{
  if (!key_state) return 0;
  unsigned result = 0;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 1;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 2;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 4;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 8;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 16;
  if (xkb_state_mod_name_is_active(key_state, "Mod3", XKB_STATE_MODS_EFFECTIVE) > 0) result |= 32;
  if (xkb_state_mod_name_is_active(key_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) result |= 64;
  if (xkb_state_mod_name_is_active(key_state, "Mod5", XKB_STATE_MODS_EFFECTIVE) > 0) result |= 128;
  const xkb_layout_index_t group = xkb_state_serialize_layout(key_state, XKB_STATE_LAYOUT_EFFECTIVE);
  if (group != XKB_LAYOUT_INVALID) result |= (group & 3) << 13;
  return result;
}

static UINT convert_key(xkb_keysym_t symbol, bool &extended)
{
  if (symbol >= XKB_KEY_F1 && symbol <= XKB_KEY_F24) return VK_F1 + symbol - XKB_KEY_F1;
  if (symbol >= XKB_KEY_KP_0 && symbol <= XKB_KEY_KP_9) return VK_NUMPAD0 + symbol - XKB_KEY_KP_0;
  extended = true;
  switch (symbol)
  {
    case XKB_KEY_Left:
      return VK_LEFT;
    case XKB_KEY_Right:
      return VK_RIGHT;
    case XKB_KEY_Up:
      return VK_UP;
    case XKB_KEY_Down:
      return VK_DOWN;
    case XKB_KEY_Home:
      return VK_HOME;
    case XKB_KEY_End:
      return VK_END;
    case XKB_KEY_Page_Up:
      return VK_PRIOR;
    case XKB_KEY_Page_Down:
      return VK_NEXT;
    case XKB_KEY_Insert:
      return VK_INSERT;
    case XKB_KEY_Delete:
      return VK_DELETE;
    case XKB_KEY_KP_Enter:
      return VK_RETURN;
  }
  extended = false;
  switch (symbol)
  {
    case XKB_KEY_BackSpace:
      return VK_BACK;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
      return VK_TAB;
    case XKB_KEY_Return:
      return VK_RETURN;
    case XKB_KEY_Escape:
      return VK_ESCAPE;
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
      return VK_SHIFT;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
      return VK_CONTROL;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
      return VK_MENU;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
      return VK_LWIN;
    case XKB_KEY_KP_Left:
      return VK_LEFT;
    case XKB_KEY_KP_Right:
      return VK_RIGHT;
    case XKB_KEY_KP_Up:
      return VK_UP;
    case XKB_KEY_KP_Down:
      return VK_DOWN;
    case XKB_KEY_KP_Home:
      return VK_HOME;
    case XKB_KEY_KP_End:
      return VK_END;
    case XKB_KEY_KP_Page_Up:
      return VK_PRIOR;
    case XKB_KEY_KP_Page_Down:
      return VK_NEXT;
    case XKB_KEY_KP_Insert:
      return VK_INSERT;
    case XKB_KEY_KP_Delete:
      return VK_DELETE;
    case XKB_KEY_KP_Begin:
      return VK_SELECT;
    case XKB_KEY_KP_Decimal:
      return VK_DECIMAL;
    case XKB_KEY_KP_Separator:
      return VK_SEPARATOR;
    case XKB_KEY_KP_Add:
      return VK_ADD;
    case XKB_KEY_KP_Subtract:
      return VK_SUBTRACT;
    case XKB_KEY_KP_Multiply:
      return VK_MULTIPLY;
    case XKB_KEY_KP_Divide:
      return VK_DIVIDE;
    case XKB_KEY_Num_Lock:
      return VK_NUMLOCK;
    case XKB_KEY_Caps_Lock:
      return VK_CAPITAL;
    case XKB_KEY_Scroll_Lock:
      return VK_SCROLL;
    case XKB_KEY_Pause:
      return VK_PAUSE;
    case XKB_KEY_Print:
      return VK_SNAPSHOT;
  }
  return 0;
}

static void send_key_message(SWELL_WaylandWindow *w, HWND target, UINT message, UINT character, int flags)
{
  SWELL_WaylandWindowGuard guard(w);
  MSG msg = {target, message, character, flags};
  if (SWELLAppMain(SWELLAPP_PROCESSMESSAGE, (INT_PTR)&msg, DialogBoxIsActive() ? 1 : 0) <= 0 &&
      guard.valid() && !target->m_hashaddestroy)
    SendMessage(target, msg.message, msg.wParam, msg.lParam);
}

static void dispatch_key_event(SWELL_WaylandWindow *w, HWND target,
    xkb_keysym_t symbol, unsigned state, bool down)
{
  SWELL_WaylandWindowGuard guard(w);
  HWND focus = GetFocusIncludeMenus();
  UINT message = down ? WM_KEYDOWN : WM_KEYUP;
  int flags = (state & 1 ? FSHIFT : 0) | (state & 4 ? FCONTROL : 0) | (state & 8 ? FALT : 0) |
      (state & 64 ? FLWIN : 0);
  bool extended = false;
  UINT key = convert_key(symbol, extended);
  if (key)
  {
    flags |= FVIRTKEY;
    if ((flags & FSHIFT) && (state & 16 ? !extended && key >= VK_PRIOR && key <= VK_SELECT
                                      : key >= VK_NUMPAD0 && key <= VK_NUMPAD9))
      flags &= ~FSHIFT;
  }
  else if (symbol >= 'a' && symbol <= 'z')
  {
    key = symbol - 'a' + 'A';
    flags |= FVIRTKEY;
  }
  else if ((symbol >= 'A' && symbol <= 'Z') || (symbol >= '0' && symbol <= '9'))
  {
    key = symbol;
    flags |= FVIRTKEY;
  }
  else if (symbol == XKB_KEY_space)
    key = VK_SPACE;
  else
  {
    key = xkb_keysym_to_utf32(symbol);
    if (symbol <= 255) flags &= ~FSHIFT;
  }
  swell_is_likely_capslock = !!(state & 2);
  if (extended) flags |= 1 << 24;
  if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN)
    message = down ? WM_SYSKEYDOWN : WM_SYSKEYUP;
  if (down && compose_state && !(flags & (FCONTROL | FALT | FLWIN)))
  {
    xkb_compose_state_feed(compose_state, symbol);
    const xkb_compose_status status = xkb_compose_state_get_status(compose_state);
    if (status == XKB_COMPOSE_COMPOSING || status == XKB_COMPOSE_CANCELLED)
    {
      if (status == XKB_COMPOSE_CANCELLED) xkb_compose_state_reset(compose_state);
      return;
    }
    if (status == XKB_COMPOSE_COMPOSED)
    {
      const int length = xkb_compose_state_get_utf8(compose_state, NULL, 0);
      WDL_TypedBuf<char> text;
      if (!text.ResizeOK(max(0, length) + 1)) return;
      xkb_compose_state_get_utf8(compose_state, text.Get(), text.GetSize());
      xkb_compose_state_reset(compose_state);
      for (const char *p = text.Get();
          *p && guard.valid() && !target->m_hashaddestroy && GetFocusIncludeMenus() == focus;)
      {
        int character;
        p += wdl_utf8_parsechar(p, &character);
        send_key_message(w, target, message, character, 0);
      }
      if (guard.valid() && !target->m_hashaddestroy) swell_im_update_candidates_location();
      return;
    }
  }
  if (key) send_key_message(w, target, message, key, flags);
  if (guard.valid() && !target->m_hashaddestroy) swell_im_update_candidates_location();
}

static void key_event(SWELL_WaylandWindow *w, uint32_t code, bool down)
{
  if (!w || !key_state) return;
  SWELL_WaylandWindowGuard guard(w);
  if (!guard.valid()) return;
  const xkb_keysym_t symbol = xkb_state_key_get_one_sym(key_state, code + 8);
  const bool host_key = (!w->callbacks.key || w->role == SWELL_WAYLAND_SUBSURFACE) &&
      xw_bridge_key_wanted_by_host(GetFocusIncludeMenus(), code + 8, modifiers);
  if (!guard.valid()) return;
  if (!host_key && w->callbacks.key &&
      w->callbacks.key((SWELL_WaylandView *)w, w->data, down, code + 8, symbol, modifiers))
    return;
  if (!guard.valid() || !w->hwnd) return;
  HWND focus = GetFocusIncludeMenus();
  HWND target = focus;
  if (!target || !IsWindow(target)) target = w->hwnd;
  SWELL_WaylandHWNDGuard hold(target);
  if (!host_key && xw_bridge_forward_key(target, code + 8, symbol, modifiers, down))
    return;
  if (xw_bridge_raise_modals())
    return;
  if (guard.valid() && hold.valid())
    dispatch_key_event(w, target, symbol, modifiers, down);
}

void swell_wayland_view_return_key(SWELL_WaylandView *view, uint32_t code, unsigned state, bool down)
{
  if (!view || view->dead || !view->hwnd || !key_state) return;
  xkb_state *event_state = xkb_state_new(xkb_state_get_keymap(key_state));
  if (!event_state) return;
  static const char *names[] = {XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL,
      XKB_MOD_NAME_ALT, XKB_MOD_NAME_NUM, "Mod3", XKB_MOD_NAME_LOGO, "Mod5"};
  xkb_mod_mask_t mask = 0;
  for (unsigned i = 0; i < 8; ++i)
  {
    const xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap, names[i]);
    if ((state & (1U << i)) && index < sizeof(mask) * 8) mask |= 1U << index;
  }
  xkb_state_update_mask(event_state, mask, 0, 0, 0, 0, (state >> 13) & 3);
  const xkb_keysym_t symbol = xkb_state_key_get_one_sym(event_state, code);
  xkb_state_unref(event_state);
  SWELL_WaylandHWNDGuard hold(view->hwnd);
  dispatch_key_event(view, hold.hwnd, symbol, state, down);
}

static double axis_values[2];
static int axis_steps[2];
static bool axis_precise[2], axis_stopped[2];
static unsigned axis_source;
static void flush_axis()
{
  SWELL_WaylandWindow *w = pointer_window;
  if (w)
  {
    SWELL_WaylandWindowGuard guard(w);
    if (w->wheel_source != axis_source || w->wheel_modifiers != modifiers) w->wheel[0] = w->wheel[1] = 0;
    w->wheel_source = axis_source;
    w->wheel_modifiers = modifiers;
    for (int axis = 0; axis < 2 && guard.valid(); ++axis)
    {
      const double value = axis_precise[axis] ? axis_steps[axis] : axis_values[axis] * 12.0;
      int amount;
      if (w->callbacks.mouse)
      {
        w->wheel[axis] += value;
        amount = (int)(w->wheel[axis] / 120.0) * 120;
        w->wheel[axis] -= amount;
      }
      else
        amount = (int)round(value);
      if (amount)
        dispatch_mouse(w, axis == 0 ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL, (int)pointer_x, (int)pointer_y,
            (WPARAM)(uint32_t)((uint32_t)(uint16_t)(axis == 0 ? -amount : amount) << 16));
      if (axis_stopped[axis]) w->wheel[axis] = 0;
    }
  }
  memset(axis_values, 0, sizeof(axis_values));
  memset(axis_steps, 0, sizeof(axis_steps));
  memset(axis_precise, 0, sizeof(axis_precise));
  memset(axis_stopped, 0, sizeof(axis_stopped));
}

static SWELL_WaylandWindow *pointer_target(SWELL_WaylandWindow *w, double &x, double &y)
{
  if (w && button_window && button_window->is_view && mouse_buttons && w != button_window)
  {
    const POINT origin = window_origin(w), target = window_origin(button_window);
    x += origin.x - target.x;
    y += origin.y - target.y;
    return button_window;
  }
  return w;
}

static void flush_pointer_motion()
{
  if (!pointer_motion_pending) return;
  pointer_motion_pending = false;
  double x = pointer_x, y = pointer_y;
  SWELL_WaylandWindow *w = pointer_target(pointer_window, x, y);
  if (w) dispatch_mouse(w, WM_MOUSEMOVE, (int)x, (int)y, 0);
}

static void pointer_button(SWELL_WaylandWindow *w, double x, double y, uint32_t serial, uint32_t time,
    uint32_t button, uint32_t state, bool from_touch = false)
{
  if (!w) return;
  const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
  SWELL_WaylandWindowGuard guard(w);
  if (down && !w->popup)
  {
    DestroyPopupMenus();
    if (!guard.valid()) return;
  }
  unsigned mask = button == BTN_LEFT ? 256 : button == BTN_MIDDLE ? 512 : button == BTN_RIGHT ? 1024 : 0;
  UINT message = button == BTN_LEFT ? WM_LBUTTONDOWN
                 : button == BTN_MIDDLE ? WM_MBUTTONDOWN
                 : button == BTN_RIGHT ? WM_RBUTTONDOWN
                                        : 0;
  if (!message) return;
  input_serial = serial;
  unsigned &buttons = from_touch ? touch_buttons : mouse_buttons;
  if (down)
  {
    popup_serial = serial;
    buttons |= mask;
    button_window = w;
    button_serial = serial;
  }
  else
  {
    buttons &= ~mask;
    ++message;
  }
  static uint32_t click_time, click_button;
  static POINT click_point;
  if (down && !w->callbacks.mouse)
  {
    if (click_window == w && click_button == button && time - click_time <= 500 &&
        abs((int)x - click_point.x) <= 4 * w->scale && abs((int)y - click_point.y) <= 4 * w->scale)
    {
      message += 2;
      click_time = 0;
    }
    else
    {
      click_time = time;
      click_button = button;
      click_point = {(int)x, (int)y};
      click_window = w;
    }
  }
  dispatch_mouse(w, message, (int)x, (int)y, 0);
  if (!down && !(mouse_buttons | touch_buttons))
  {
    button_window = NULL;
    button_serial = 0;
  }
}

static void pointer_enter_cb(
    void *, wl_pointer *, uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
  SWELL_WaylandWindowGuard target(window_from_surface(surface));
  flush_pointer_motion();
  pointer_window = target.valid() ? target.window : NULL;
  pointer_serial = serial;
  if (!pointer_window) return;
  pointer_x = wl_fixed_to_double(x) * pointer_window->scale;
  pointer_y = wl_fixed_to_double(y) * pointer_window->scale;
  if (pointer_window->is_view) SetCursor(LoadCursor(NULL, IDC_ARROW));
  update_cursor();
  double local_x = pointer_x, local_y = pointer_y;
  SWELL_WaylandWindow *w = pointer_target(pointer_window, local_x, local_y);
  dispatch_mouse(w, SWELL_WAYLAND_MOUSE_ENTER, (int)local_x, (int)local_y, 0);
}

static void pointer_leave_cb(void *, wl_pointer *, uint32_t, wl_surface *surface)
{
  SWELL_WaylandWindowGuard target(window_from_surface(surface));
  flush_pointer_motion();
  SWELL_WaylandWindow *w = target.valid() ? target.window : NULL;
  if (w)
  {
    w->wheel[0] = w->wheel[1] = 0;
    dispatch_mouse(w, SWELL_WAYLAND_MOUSE_LEAVE, (int)pointer_x, (int)pointer_y, 0);
  }
  if (pointer_window == w) pointer_window = NULL;
  if (cursor_window == w) cursor_window = NULL;
}

static void pointer_motion_cb(void *, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
  if (!pointer_window) return;
  pointer_x = wl_fixed_to_double(x) * pointer_window->scale;
  pointer_y = wl_fixed_to_double(y) * pointer_window->scale;
  pointer_motion_pending = true;
}

static void pointer_button_cb(
    void *, wl_pointer *, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
  flush_pointer_motion();
  double x = pointer_x, y = pointer_y;
  SWELL_WaylandWindow *w = pointer_target(pointer_window, x, y);
  pointer_button(w, x, y, serial, time, button, state);
}

static void pointer_axis_cb(void *, wl_pointer *p, uint32_t, uint32_t axis, wl_fixed_t value)
{
  flush_pointer_motion();
  if (axis < 2) axis_values[axis] += wl_fixed_to_double(value);
  if (wl_pointer_get_version(p) < 5) flush_axis();
}

static void pointer_frame_cb(void *, wl_pointer *)
{
  flush_axis();
}

static void pointer_axis_source_cb(void *, wl_pointer *, uint32_t source)
{
  axis_source = source;
}

static void pointer_axis_stop_cb(void *, wl_pointer *, uint32_t, uint32_t axis)
{
  if (axis < 2) axis_stopped[axis] = true;
}

static void pointer_axis_discrete_cb(void *, wl_pointer *, uint32_t axis, int32_t discrete)
{
  if (axis < 2)
  {
    axis_steps[axis] += discrete * 120;
    axis_precise[axis] = true;
  }
}

static void pointer_axis_value120_cb(void *, wl_pointer *, uint32_t axis, int32_t value)
{
  if (axis < 2)
  {
    axis_steps[axis] += value;
    axis_precise[axis] = true;
  }
}

static void pointer_axis_relative_direction_cb(void *, wl_pointer *, uint32_t, uint32_t) {}

static const wl_pointer_listener pointer_listener = {pointer_enter_cb, pointer_leave_cb, pointer_motion_cb,
    pointer_button_cb, pointer_axis_cb, pointer_frame_cb, pointer_axis_source_cb, pointer_axis_stop_cb,
    pointer_axis_discrete_cb, pointer_axis_value120_cb, pointer_axis_relative_direction_cb};
static void stop_repeat()
{
  if (repeat_source) g_source_remove(repeat_source);
  repeat_source = 0;
  repeat_key = 0;
}

static gboolean repeat_key_cb(gpointer)
{
  if (!keyboard_window)
  {
    repeat_source = 0;
    return G_SOURCE_REMOVE;
  }
  key_event(keyboard_window, repeat_key, true);
  return repeat_source ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean start_repeat_cb(gpointer)
{
  repeat_source = 0;
  SWELL_WaylandWindow *window = keyboard_window;
  const uint32_t key = repeat_key;
  if (!window || !key || repeat_rate <= 0) return G_SOURCE_REMOVE;
  key_event(window, key, true);
  if (keyboard_window == window && repeat_key == key && !repeat_source && repeat_rate > 0)
    repeat_source = g_timeout_add(max(1, 1000 / repeat_rate), repeat_key_cb, NULL);
  return G_SOURCE_REMOVE;
}

static void keyboard_keymap_cb(void *, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size)
{
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size)
  {
    close(fd);
    return;
  }
  char *text = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (text == MAP_FAILED) return;
  xkb_keymap *next =
      xkb_keymap_new_from_string(key_context, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(text, size);
  if (!next) return;
  xkb_state *state = xkb_state_new(next);
  if (!state)
  {
    xkb_keymap_unref(next);
    return;
  }
  if (key_state) xkb_state_unref(key_state);
  if (keymap) xkb_keymap_unref(keymap);
  keymap = next;
  key_state = state;
}

static void keyboard_enter_cb(void *, wl_keyboard *, uint32_t serial, wl_surface *surface, wl_array *)
{
  SWELL_WaylandWindowGuard target(window_from_surface(surface));
  flush_pointer_motion();
  input_serial = serial;
  keyboard_window = target.valid() ? target.window : NULL;
  if (keyboard_window) set_focus(keyboard_window, true);
}

static void keyboard_leave_cb(void *, wl_keyboard *, uint32_t, wl_surface *surface)
{
  SWELL_WaylandWindowGuard target(window_from_surface(surface));
  flush_pointer_motion();
  stop_repeat();
  SWELL_WaylandWindow *w = target.valid() ? target.window : NULL;
  if (keyboard_window == w) keyboard_window = NULL;
  if (compose_state) xkb_compose_state_reset(compose_state);
  if (w) set_focus(w, false);
}

static void keyboard_key_cb(void *, wl_keyboard *, uint32_t serial, uint32_t, uint32_t code, uint32_t state)
{
  flush_pointer_motion();
  input_serial = popup_serial = serial;
  const bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
  if (!down && code == repeat_key) stop_repeat();
  if (down && keyboard_window && keymap && repeat_rate > 0 && xkb_keymap_key_repeats(keymap, code + 8))
  {
    stop_repeat();
    repeat_key = code;
    repeat_source = g_timeout_add(max(1, repeat_delay), start_repeat_cb, NULL);
  }
  key_event(keyboard_window, code, down);
}

static void keyboard_modifiers_cb(
    void *, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
  flush_pointer_motion();
  const unsigned previous = modifiers;
  if (key_state) xkb_state_update_mask(key_state, depressed, latched, locked, 0, 0, group);
  modifiers = key_modifiers();
  SWELL_WaylandWindow *w = pointer_window;
  if (previous == modifiers || !w || w != cursor_window || !w->hwnd || w->is_view ||
      GetCapture() || transfer_drag_active()) return;

  const POINT p = cursor_position;
  SWELL_WaylandHWNDGuard target(WindowFromPoint(p));
  if (!target.valid() || !IsWindowEnabled(target.hwnd)) return;
  const LRESULT hit = SendMessage(target.hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
  if (hit != HTCLIENT || !target.valid() || pointer_window != w || GetCapture()) return;
  if (!SendMessage(target.hwnd, WM_SETCURSOR, (WPARAM)target.hwnd, hit) &&
      pointer_window == w && !GetCapture())
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

static void keyboard_repeat_info_cb(void *, wl_keyboard *, int32_t rate, int32_t delay)
{
  repeat_rate = max(0, rate);
  repeat_delay = max(1, delay);
  if (!repeat_rate) stop_repeat();
}

static const wl_keyboard_listener keyboard_listener = {keyboard_keymap_cb, keyboard_enter_cb,
    keyboard_leave_cb, keyboard_key_cb, keyboard_modifiers_cb, keyboard_repeat_info_cb};
static void touch_down_cb(void *, wl_touch *, uint32_t serial, uint32_t time, wl_surface *surface, int32_t id,
    wl_fixed_t x, wl_fixed_t y)
{
  if (touch_id >= 0) return;
  touch_window = window_from_surface(surface);
  if (!touch_window) return;
  touch_id = id;
  touch_x = wl_fixed_to_double(x) * touch_window->scale;
  touch_y = wl_fixed_to_double(y) * touch_window->scale;
  pointer_button(
      touch_window, touch_x, touch_y, serial, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED, true);
}

static void touch_up_cb(void *, wl_touch *, uint32_t serial, uint32_t time, int32_t id)
{
  if (id != touch_id) return;
  SWELL_WaylandWindow *window = touch_window;
  touch_id = -1;
  touch_window = NULL;
  pointer_button(window, touch_x, touch_y, serial, time, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, true);
}

static void touch_motion_cb(void *, wl_touch *, uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
  if (id != touch_id || !touch_window) return;
  touch_x = wl_fixed_to_double(x) * touch_window->scale;
  touch_y = wl_fixed_to_double(y) * touch_window->scale;
  dispatch_mouse(touch_window, WM_MOUSEMOVE, (int)touch_x, (int)touch_y, 0);
}

static void touch_frame_cb(void *, wl_touch *) {}

static void touch_cancel_cb(void *, wl_touch *)
{
  SWELL_WaylandWindow *window = touch_window;
  touch_id = -1;
  touch_window = NULL;
  pointer_button(window, touch_x, touch_y, input_serial, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED, true);
}

static void touch_shape_cb(void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) {}

static void touch_orientation_cb(void *, wl_touch *, int32_t, wl_fixed_t) {}

static const wl_touch_listener touch_listener = {touch_down_cb, touch_up_cb, touch_motion_cb, touch_frame_cb,
    touch_cancel_cb, touch_shape_cb, touch_orientation_cb};
static void seat_capabilities_cb(void *, wl_seat *s, uint32_t capabilities)
{
  const bool pointer_lost = pointer && !(capabilities & WL_SEAT_CAPABILITY_POINTER);
  const bool keyboard_lost = keyboard && !(capabilities & WL_SEAT_CAPABILITY_KEYBOARD);
  const bool touch_lost = touch && !(capabilities & WL_SEAT_CAPABILITY_TOUCH);
  SWELL_WaylandHWNDGuard capture(
      (pointer_lost && mouse_buttons) || (touch_lost && touch_buttons) ? GetCapture() : NULL);
  SWELL_WaylandWindowGuard old_keyboard(keyboard_lost ? keyboard_window : NULL);
  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer)
  {
    pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  }
  else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer)
  {
    if (cursor_device)
    {
      wp_cursor_shape_device_v1_destroy(cursor_device);
      cursor_device = NULL;
    }
    wl_pointer_release(pointer);
    pointer = NULL;
    if (pointer_window) pointer_window->wheel[0] = pointer_window->wheel[1] = 0;
    if (cursor_window == pointer_window) cursor_window = NULL;
    pointer_window = NULL;
    mouse_buttons = 0;
    pointer_serial = 0;
    click_window = NULL;
    memset(axis_values, 0, sizeof(axis_values));
    memset(axis_steps, 0, sizeof(axis_steps));
    memset(axis_precise, 0, sizeof(axis_precise));
    memset(axis_stopped, 0, sizeof(axis_stopped));
  }
  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard)
  {
    keyboard = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
  }
  else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard)
  {
    stop_repeat();
    wl_keyboard_release(keyboard);
    keyboard = NULL;
    keyboard_window = NULL;
    modifiers = 0;
    if (key_state) xkb_state_update_mask(key_state, 0, 0, 0, 0, 0, 0);
    if (compose_state) xkb_compose_state_reset(compose_state);
  }
  if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !touch)
  {
    touch = wl_seat_get_touch(s);
    wl_touch_add_listener(touch, &touch_listener, NULL);
  }
  else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH) && touch)
  {
    wl_touch_release(touch);
    touch = NULL;
  }
  if (touch_lost) touch_listener.cancel(NULL, NULL);
  if (old_keyboard.valid() && !keyboard_window) set_focus(old_keyboard.window, false);
  if (!(mouse_buttons | touch_buttons))
  {
    button_window = NULL;
    button_serial = 0;
  }
  if (capture.valid() && GetCapture() == capture.hwnd && !(mouse_buttons | touch_buttons)) ReleaseCapture();
  transfer_seat_changed();
}

static void seat_name_cb(void *, wl_seat *, const char *name)
{
  seat_label.Set(name);
}

static const wl_seat_listener seat_listener = {seat_capabilities_cb, seat_name_cb};
static SWELL_CursorResourceIndex *SWELL_curmodule_cursorresource_head;
static HCURSOR current_cursor;
static int cursor_visible;
static wl_surface *cursor_surface;
static POINT cursor_hotspot;
static WDL_IntKeyedArray<wl_cursor_theme *> cursor_themes;
static WDL_StringKeyedArray<SWELL_WaylandCursor *> named_cursors;

static const struct { const char *name; uint32_t shape; } cursor_shapes[] = {
  {"default", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
  {"context-menu", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU},
  {"help", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
  {"pointer", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
  {"progress", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
  {"wait", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
  {"cell", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL},
  {"crosshair", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
  {"text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
  {"vertical-text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT},
  {"alias", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
  {"copy", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
  {"move", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
  {"no-drop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
  {"not-allowed", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
  {"grab", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB},
  {"grabbing", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING},
  {"e-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
  {"n-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
  {"ne-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
  {"nw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
  {"s-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
  {"se-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
  {"sw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
  {"w-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
  {"ew-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
  {"ns-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
  {"nesw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
  {"nwse-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
  {"col-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
  {"row-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
  {"all-scroll", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
  {"zoom-in", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN},
  {"zoom-out", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT},
  {"left_ptr", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
  {"arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
  {"top_left_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
  {"hand1", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
  {"hand2", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
  {"xterm", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
  {"watch", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
  {"left_ptr_watch", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
  {"cross", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
  {"fleur", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
  {"question_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
  {"sb_h_double_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
  {"sb_v_double_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
  {"bd_double_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
  {"fd_double_arrow", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
  {"top_side", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
  {"bottom_side", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
  {"left_side", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
  {"right_side", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
  {"top_left_corner", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
  {"top_right_corner", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
  {"bottom_left_corner", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
  {"bottom_right_corner", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE}
};

static void update_cursor()
{
  const bool dragging = transfer_drag_active();
  SWELL_WaylandWindow *window = pointer_window ? pointer_window :
    dragging ? button_window : NULL;
  if (!pointer || !window || !pointer_serial) return;
  if (cursor_visible < 0)
  {
    wl_pointer_set_cursor(pointer, pointer_serial, NULL, 0, 0);
    return;
  }
  SWELL_WaylandCursor *cursor = (SWELL_WaylandCursor *)current_cursor;
  if (!dragging && window->is_view)
  {
    SWELL_WaylandCursor *view_cursor = &((SWELL_WaylandView *)window)->cursor;
    if (view_cursor->buffer || view_cursor->name.GetLength()) cursor = view_cursor;
  }
  if (cursor_manager && !dragging && (!cursor || cursor->shape))
  {
    if (!cursor_device) cursor_device = wp_cursor_shape_manager_v1_get_pointer(cursor_manager, pointer);
    wp_cursor_shape_device_v1_set_shape(
        cursor_device, pointer_serial, cursor ? cursor->shape : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    return;
  }
  const int scale = window->scale;
  if (!cursor_surface) cursor_surface = wl_compositor_create_surface(compositor);
  wl_buffer *buffer = NULL;
  int hotspot_x = 0, hotspot_y = 0, width = 0, height = 0, buffer_scale = scale;
  wl_cursor *image = NULL;
  if (!cursor || cursor->name.GetLength())
  {
    wl_cursor_theme *theme = cursor_themes.Get(scale);
    if (!theme)
    {
      const char *value = getenv("XCURSOR_SIZE");
      const long size = value ? strtol(value, NULL, 10) : 24;
      const int pixels = size > 0 && size <= 256 ? size : 24;
      theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), pixels * scale, shm);
      cursor_themes.Insert(scale, theme);
    }
    const char *name = cursor ? cursor->name.Get() : "left_ptr";
    if (!strcmp(name, "left_ptr"))
    {
      const char *drag_name = transfer_cursor_name();
      if (drag_name) name = drag_name;
    }
    if (theme)
    {
      image = wl_cursor_theme_get_cursor(theme, name);
      if (!image && cursor && cursor->shape)
        for (const auto &entry : cursor_shapes)
        {
          if (entry.shape != cursor->shape) continue;
          image = wl_cursor_theme_get_cursor(theme, entry.name);
          if (image) break;
        }
      if (!image && (!cursor || !cursor->buffer)) image = wl_cursor_theme_get_cursor(theme, "left_ptr");
    }
  }
  if (image && image->image_count)
  {
    wl_cursor_image *frame = image->images[0];
    buffer = wl_cursor_image_get_buffer(frame);
    width = frame->width;
    height = frame->height;
    hotspot_x = frame->hotspot_x / scale;
    hotspot_y = frame->hotspot_y / scale;
  }
  else if (cursor && cursor->buffer)
  {
    cursor->buffer->busy = true;
    buffer = cursor->buffer->object;
    width = cursor->buffer->width;
    height = cursor->buffer->height;
    hotspot_x = cursor->hotspot_x;
    hotspot_y = cursor->hotspot_y;
    buffer_scale = 1;
  }
  if (!buffer) return;
  wl_pointer_set_cursor(pointer, pointer_serial, cursor_surface,
      dragging ? cursor_hotspot.x : hotspot_x, dragging ? cursor_hotspot.y : hotspot_y);
  int x = dragging ? cursor_hotspot.x - hotspot_x : 0;
  int y = dragging ? cursor_hotspot.y - hotspot_y : 0;
  cursor_hotspot = {hotspot_x, hotspot_y};
  if ((x || y) && wl_surface_get_version(cursor_surface) >= 5)
  {
    wl_surface_offset(cursor_surface, x, y);
    x = y = 0;
  }
  wl_surface_set_buffer_scale(cursor_surface, buffer_scale);
  wl_surface_attach(cursor_surface, buffer, x, y);
  wl_surface_damage_buffer(cursor_surface, 0, 0, width, height);
  wl_surface_commit(cursor_surface);
}

bool swell_wayland_view_has_pointer(SWELL_WaylandView *view, POINT *point)
{
  if (!view || view != pointer_window) return false;
  if (point) *point = {(int)pointer_x, (int)pointer_y};
  return true;
}

void swell_wayland_view_set_cursor(SWELL_WaylandView *view, const char *name, const uint32_t *pixels,
    int width, int height, int hotspot_x, int hotspot_y)
{
  if (!view || view->dead) return;
  uint32_t shape = 0;
  for (const auto &entry : cursor_shapes)
    if (!strcmp(name, entry.name))
    {
      shape = entry.shape;
      break;
    }
  SWELL_WaylandBuffer *buffer = NULL;
  if (pixels && (!cursor_manager || !shape))
  {
    SWELL_WaylandWindow temporary;
    temporary.width = width;
    temporary.height = height;
    temporary.opacity = 254;
    buffer = create_buffer(&temporary);
    if (!buffer) return;
    buffer->owner = NULL;
    memcpy(buffer->data, pixels, buffer->length);
  }
  SWELL_WaylandCursor &cursor = view->cursor;
  if (cursor.buffer)
  {
    cursor.buffer->retired = true;
    if (!cursor.buffer->busy) delete cursor.buffer;
  }
  cursor.name.Set(name);
  cursor.shape = shape;
  cursor.buffer = buffer;
  cursor.hotspot_x = hotspot_x;
  cursor.hotspot_y = hotspot_y;
  if (view == pointer_window) update_cursor();
}

void SWELL_SetCursor_wayland(HCURSOR cursor)
{
  if (cursor != current_cursor)
  {
    current_cursor = cursor;
    update_cursor();
  }
}

HCURSOR SWELL_GetCursor_wayland()
{
  return current_cursor;
}

HCURSOR SWELL_GetLastSetCursor_wayland()
{
  return current_cursor;
}

bool SWELL_IsCursorVisible_wayland()
{
  return cursor_visible >= 0;
}

int SWELL_ShowCursor_wayland(BOOL visible)
{
  const bool was_visible = cursor_visible >= 0;
  cursor_visible += visible ? 1 : -1;
  if (was_visible != (cursor_visible >= 0)) update_cursor();
  return cursor_visible;
}

BOOL SWELL_SetCursorPos_wayland(int, int)
{
  return FALSE;
}

static HCURSOR load_cursor(const char *file, int hotspot_x, int hotspot_y)
{
  if (!swell_wayland_init()) return NULL;
  LICE_IBitmap *image = LICE_LoadIcon(file, 32);
  if (!image) return NULL;
  SWELL_WaylandCursor *cursor = new SWELL_WaylandCursor;
  cursor->hotspot_x = hotspot_x;
  cursor->hotspot_y = hotspot_y;
  FILE *input = fopen(file, "rb");
  if (input)
  {
    unsigned char header[22];
    if (fread(header, 1, sizeof(header), input) == sizeof(header) && header[2] == 2)
    {
      cursor->hotspot_x = header[10] | header[11] << 8;
      cursor->hotspot_y = header[12] | header[13] << 8;
    }
    fclose(input);
  }
  SWELL_WaylandWindow temporary;
  temporary.width = image->getWidth();
  temporary.height = image->getHeight();
  temporary.opacity = 254;
  cursor->buffer = create_buffer(&temporary);
  if (cursor->buffer)
  {
    cursor->buffer->owner = NULL;
    for (int y = 0; y < image->getHeight(); ++y)
    {
      const LICE_pixel *src = image->getBits() + y * image->getRowSpan();
      uint32_t *dest = (uint32_t *)((unsigned char *)cursor->buffer->data + y * cursor->buffer->stride);
      for (int x = 0; x < image->getWidth(); ++x)
      {
        unsigned a = LICE_GETA(src[x]);
        dest[x] = (a << 24) | ((LICE_GETR(src[x]) * a / 255) << 16) | ((LICE_GETG(src[x]) * a / 255) << 8) |
                  (LICE_GETB(src[x]) * a / 255);
      }
    }
  }
  delete image;
  if (!cursor->buffer)
  {
    delete cursor;
    return NULL;
  }
  return (HCURSOR)cursor;
}

HCURSOR SWELL_LoadCursorFromFile_wayland(const char *file)
{
  return load_cursor(file, 0, 0);
}

HCURSOR SWELL_LoadCursor_wayland(const char *id)
{
  const char *name = "left_ptr";
  uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
  if (id == IDC_NO)
  {
    name = "not-allowed";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
  }
  else if (id == IDC_SIZENWSE)
  {
    name = "nwse-resize";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE;
  }
  else if (id == IDC_SIZENESW)
  {
    name = "nesw-resize";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
  }
  else if (id == IDC_SIZEALL)
  {
    name = "all-scroll";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
  }
  else if (id == IDC_SIZEWE)
  {
    name = "ew-resize";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
  }
  else if (id == IDC_SIZENS)
  {
    name = "ns-resize";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
  }
  else if (id == IDC_HAND)
  {
    name = "pointer";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
  }
  else if (id == IDC_UPARROW)
  {
    name = "up-arrow";
    shape = 0;
  }
  else if (id == IDC_IBEAM)
  {
    name = "text";
    shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
  }
  else if (id != IDC_ARROW)
    for (SWELL_CursorResourceIndex *resource = SWELL_curmodule_cursorresource_head; resource;
        resource = resource->_next)
      if (resource->resid == id)
      {
        if (resource->cachedCursor) return resource->cachedCursor;
        char path[4096];
        GetModuleFileName(NULL, path, sizeof(path));
        WDL_remove_filepart(path);
        snprintf_append(path, sizeof(path), "/Resources/%s.cur", resource->resname);
        return resource->cachedCursor = load_cursor(path, resource->hotspot.x, resource->hotspot.y);
      }
  SWELL_WaylandCursor *cursor = named_cursors.Get(name);
  if (!cursor)
  {
    cursor = new SWELL_WaylandCursor;
    cursor->name.Set(name);
    cursor->shape = shape;
    named_cursors.Insert(name, cursor);
  }
  return (HCURSOR)cursor;
}

void SWELL_Register_Cursor_Resource_wayland(const char *id, const char *name, int x, int y)
{
  SWELL_CursorResourceIndex *resource = (SWELL_CursorResourceIndex *)calloc(1, sizeof(*resource));
  resource->resid = id;
  resource->resname = name;
  resource->hotspot.x = x;
  resource->hotspot.y = y;
  resource->_next = SWELL_curmodule_cursorresource_head;
  SWELL_curmodule_cursorresource_head = resource;
}

static void xdg_output_logical_position_cb(void *data, zxdg_output_v1 *, int32_t x, int32_t y)
{
  RECT &r = ((SWELL_WaylandOutput *)data)->pending_logical;
  r.right += x - r.left;
  r.bottom += y - r.top;
  r.left = x;
  r.top = y;
}

static void xdg_output_logical_size_cb(void *data, zxdg_output_v1 *, int32_t width, int32_t height)
{
  RECT &r = ((SWELL_WaylandOutput *)data)->pending_logical;
  r.right = r.left + width;
  r.bottom = r.top + height;
}

static void xdg_output_done_cb(void *data, zxdg_output_v1 *)
{
  SWELL_WaylandOutput *value = (SWELL_WaylandOutput *)data;
  value->logical = value->pending_logical;
}

static void xdg_output_name_cb(void *data, zxdg_output_v1 *, const char *name)
{
  ((SWELL_WaylandOutput *)data)->label.Set(name);
}

static void xdg_output_description_cb(void *, zxdg_output_v1 *, const char *) {}

static void create_xdg_output(SWELL_WaylandOutput *output)
{
  if (!output_manager || output->xdg || wl_output_get_version(output->object) < 2) return;
  output->xdg = zxdg_output_manager_v1_get_xdg_output(output_manager, output->object);
  static const zxdg_output_v1_listener listener = {xdg_output_logical_position_cb, xdg_output_logical_size_cb,
      xdg_output_done_cb, xdg_output_name_cb, xdg_output_description_cb};
  zxdg_output_v1_add_listener(output->xdg, &listener, output);
}

static void output_geometry_cb(void *data, wl_output *, int32_t x, int32_t y, int32_t, int32_t, int32_t,
    const char *, const char *, int32_t transform)
{
  SWELL_WaylandOutput *output = (SWELL_WaylandOutput *)data;
  output->x = x;
  output->y = y;
  output->transform = transform;
}

static void output_mode_cb(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t)
{
  if (flags & WL_OUTPUT_MODE_CURRENT)
  {
    SWELL_WaylandOutput *output = (SWELL_WaylandOutput *)data;
    output->width = width;
    output->height = height;
  }
}

static void output_done_cb(void *data, wl_output *)
{
  SWELL_WaylandOutput *output = (SWELL_WaylandOutput *)data;
  if (output->xdg && zxdg_output_v1_get_version(output->xdg) >= 3) output->logical = output->pending_logical;
  WDL_PtrList<SWELL_WaylandWindow> pending = windows;
  for (SWELL_WaylandWindow *w : pending) w->retain();
  for (SWELL_WaylandWindow *w : pending)
  {
    if (!w->dead) update_scale(w);
    w->release();
  }
}

static void output_scale_cb(void *data, wl_output *, int32_t scale)
{
  ((SWELL_WaylandOutput *)data)->scale = max(1, scale);
}

static void output_name_cb(void *data, wl_output *, const char *name)
{
  ((SWELL_WaylandOutput *)data)->label.Set(name);
}

static void output_description_cb(void *, wl_output *, const char *) {}

static const wl_output_listener output_listener = {output_geometry_cb, output_mode_cb, output_done_cb,
    output_scale_cb, output_name_cb, output_description_cb};
static void wm_base_ping_cb(void *, xdg_wm_base *base, uint32_t serial)
{
  xdg_wm_base_pong(base, serial);
}

static void registry_global_cb(void *, wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
  if (!strcmp(interface, wl_compositor_interface.name))
  {
    if (version < 4)
    {
      unsupported_protocol = true;
      return;
    }
    compositor = (wl_compositor *)wl_registry_bind(r, name, &wl_compositor_interface, min(version, 6U));
  }
  else if (!strcmp(interface, wl_subcompositor_interface.name))
    subcompositor = (wl_subcompositor *)wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  else if (!strcmp(interface, wl_shm_interface.name))
    shm = (wl_shm *)wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(interface, xdg_wm_base_interface.name))
  {
    wm_base = (xdg_wm_base *)wl_registry_bind(r, name, &xdg_wm_base_interface, min(version, 6U));
    static const xdg_wm_base_listener listener = {wm_base_ping_cb};
    xdg_wm_base_add_listener(wm_base, &listener, NULL);
  }
  else if (!strcmp(interface, wl_seat_interface.name) && !seat)
  {
    if (version < 3)
    {
      unsupported_protocol = true;
      return;
    }
    seat = (wl_seat *)wl_registry_bind(r, name, &wl_seat_interface, min(version, 9U));
    seat_name = name;
    wl_seat_add_listener(seat, &seat_listener, NULL);
  }
  else if (!strcmp(interface, wl_output_interface.name))
  {
    SWELL_WaylandOutput *output = new SWELL_WaylandOutput;
    output->object = (wl_output *)wl_registry_bind(r, name, &wl_output_interface, min(version, 4U));
    output->name = name;
    outputs.Add(output);
    wl_output_add_listener(output->object, &output_listener, output);
    create_xdg_output(output);
  }
  else if (!strcmp(interface, zxdg_output_manager_v1_interface.name) && !output_manager)
  {
    output_manager = (zxdg_output_manager_v1 *)wl_registry_bind(
        r, name, &zxdg_output_manager_v1_interface, min(version, 3U));
    output_manager_name = name;
    for (SWELL_WaylandOutput *output : outputs) create_xdg_output(output);
  }
  else if (!strcmp(interface, xdg_activation_v1_interface.name))
    activation = (xdg_activation_v1 *)wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
  else if (!strcmp(interface, wp_viewporter_interface.name))
    viewporter = (wp_viewporter *)wl_registry_bind(r, name, &wp_viewporter_interface, 1);
  else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name))
    cursor_manager =
        (wp_cursor_shape_manager_v1 *)wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
  transfer_registry(name, interface, version);
}

static void registry_global_remove_cb(void *, wl_registry *, uint32_t name)
{
  if (seat && name == seat_name)
  {
    wl_seat *old = seat;
    seat = NULL;
    seat_name = 0;
    input_serial = popup_serial = 0;
    seat_label.Set("");
    for (SWELL_WaylandWindow *w : windows) w->popup_grabbed = false;
    seat_listener.capabilities(NULL, old, 0);
    if (wl_seat_get_version(old) >= 5)
      wl_seat_release(old);
    else
      wl_seat_destroy(old);
  }
  if (output_manager && name == output_manager_name)
  {
    for (SWELL_WaylandOutput *output : outputs)
      if (output->xdg)
      {
        zxdg_output_v1_destroy(output->xdg);
        output->xdg = NULL;
        output->logical = output->pending_logical = RECT{};
      }
    zxdg_output_manager_v1_destroy(output_manager);
    output_manager = NULL;
  }
  for (int i = 0; i < outputs.GetSize(); ++i)
    if (outputs.Get(i)->name == name)
    {
      SWELL_WaylandOutput *output = outputs.Get(i);
      for (SWELL_WaylandWindow *w : windows)
        w->outputs.DeletePtr(output);
      if (output->xdg) zxdg_output_v1_destroy(output->xdg);
      if (wl_output_get_version(output->object) >= 3)
        wl_output_release(output->object);
      else
        wl_output_destroy(output->object);
      delete output;
      outputs.Delete(i);
      break;
    }
}

static const wl_registry_listener registry_listener = {registry_global_cb, registry_global_remove_cb};
struct SWELL_WaylandDisplaySource
{
  GSource source;
  GPollFD poll;
  bool reading;
};

static SWELL_WaylandDisplaySource *display_source;
static void cancel_read()
{
  if (display_source && display_source->reading)
  {
    wl_display_cancel_read(display);
    display_source->reading = false;
  }
}

static gboolean display_source_prepare_cb(GSource *base, gint *timeout)
{
  SWELL_WaylandDisplaySource *source = (SWELL_WaylandDisplaySource *)base;
  *timeout = -1;
  if (disconnected) return FALSE;
  cancel_read();
  if (wl_display_prepare_read(display) != 0) return TRUE;
  source->reading = true;
  source->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  if (wl_display_flush(display) < 0)
  {
    if (errno == EAGAIN)
      source->poll.events |= G_IO_OUT;
    else
    {
      cancel_read();
      disconnected = true;
    }
  }
  return FALSE;
}

static gboolean display_source_check_cb(GSource *base)
{
  SWELL_WaylandDisplaySource *source = (SWELL_WaylandDisplaySource *)base;
  if (source->reading)
  {
    if (source->poll.revents & G_IO_IN)
    {
      source->reading = false;
      if (wl_display_read_events(display) < 0) disconnected = true;
    }
    else
      cancel_read();
  }
  return source->poll.revents != 0;
}

static gboolean display_source_dispatch_cb(GSource *base, GSourceFunc, gpointer)
{
  SWELL_WaylandDisplaySource *source = (SWELL_WaylandDisplaySource *)base;
  cancel_read();
  if (source->poll.revents & (G_IO_ERR | G_IO_HUP)) disconnected = true;
  if (disconnected) return G_SOURCE_REMOVE;
  if (wl_display_dispatch_pending(display) < 0)
  {
    disconnected = true;
    return G_SOURCE_REMOVE;
  }
  flush_pointer_motion();
  transfer_dispatch();
  if (wl_display_flush(display) < 0 && errno != EAGAIN)
  {
    disconnected = true;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs source_functions = {
    display_source_prepare_cb, display_source_check_cb, display_source_dispatch_cb, NULL, NULL, NULL};
static void decor_error_cb(libdecor *, libdecor_error, const char *message)
{
  fprintf(stderr, "SWELL: %s\n", message);
}

bool swell_wayland_init()
{
  if (initialized) return display && !disconnected;
  initialized = true;
  key_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  const char *locale = setlocale(LC_CTYPE, NULL);
  if (key_context)
    compose_table =
        xkb_compose_table_new_from_locale(key_context, locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (compose_table) compose_state = xkb_compose_state_new(compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
  display = wl_display_connect(NULL);
  if (!display) return false;
  registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 || unsupported_protocol ||
      !compositor || !shm || !wm_base)
  {
    wl_display_disconnect(display);
    display = NULL;
    return false;
  }
  static libdecor_interface interface = {
      decor_error_cb, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
  decor = libdecor_new(display, &interface);
  if (!decor)
  {
    wl_display_disconnect(display);
    display = NULL;
    return false;
  }
  transfer_init();
  display_source =
      (SWELL_WaylandDisplaySource *)g_source_new(&source_functions, sizeof(SWELL_WaylandDisplaySource));
  display_source->poll.fd = wl_display_get_fd(display);
  display_source->poll.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  g_source_add_poll(&display_source->source, &display_source->poll);
  g_source_set_can_recurse(&display_source->source, TRUE);
  g_source_attach(&display_source->source, g_main_context_default());
  const char *token = getenv("XDG_ACTIVATION_TOKEN");
  if (!token || !*token) token = getenv("DESKTOP_STARTUP_ID");
  if (token) activation_token.Set(token);
  unsetenv("XDG_ACTIVATION_TOKEN");
  unsetenv("DESKTOP_STARTUP_ID");
  init_private_xwayland();
  return true;
}

void SWELL_initargs_wayland(int *, char ***)
{
  swell_wayland_init();
}

void SWELL_RunEvents_wayland()
{
  if (!swell_wayland_init()) return;
  cancel_read();
  GMainContext *context = g_main_context_default();
  const gint64 deadline = g_get_monotonic_time() + 4000;
  do
  {
    if (!g_main_context_iteration(context, FALSE)) break;
  } while (g_get_monotonic_time() < deadline);
  cancel_read();
  update_app_focus();
}

void swell_oswindow_destroy_spare_wayland(SWELL_OSWINDOW) {}
void swell_oswindow_destroy_wayland(HWND hwnd)
{
  destroy_gl_view(hwnd);
  if (!hwnd || !hwnd->m_oswindow) return;
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)hwnd->m_oswindow;
  if (w == drag_icon)
  {
    w->hwnd = NULL;
    w->mapped = w->pending = false;
    if (w->callback)
    {
      wl_callback_destroy(w->callback);
      w->callback = NULL;
    }
    wl_surface_attach(w->surface, NULL, 0, 0);
    wl_surface_commit(w->surface);
  }
  else destroy_native(w);
  hwnd->m_oswindow = NULL;
  delete hwnd->m_backingstore;
  hwnd->m_backingstore = NULL;
}

void swell_oswindow_update_text_wayland(HWND hwnd)
{
  SWELL_WaylandWindow *w = hwnd ? (SWELL_WaylandWindow *)hwnd->m_oswindow : NULL;
  if (w && w->frame) libdecor_frame_set_title(w->frame, hwnd->m_title.Get());
}

void swell_recalcMinMaxInfo_wayland(HWND hwnd)
{
  SWELL_WaylandWindow *w = hwnd ? (SWELL_WaylandWindow *)hwnd->m_oswindow : NULL;
  if (!w || !w->frame || w->resize_pending) return;
  SWELL_WaylandWindowGuard guard(w);
  MINMAXINFO info = {};
  if (hwnd->m_style & WS_THICKFRAME)
  {
    info.ptMinTrackSize.x = info.ptMinTrackSize.y = 20;
    info.ptMaxSize.x = info.ptMaxSize.y = info.ptMaxTrackSize.x = info.ptMaxTrackSize.y = 16384;
    SendMessage(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&info);
  }
  else
  {
    info.ptMinTrackSize.x = info.ptMaxSize.x = hwnd->m_position.right - hwnd->m_position.left;
    info.ptMinTrackSize.y = info.ptMaxSize.y = hwnd->m_position.bottom - hwnd->m_position.top;
  }
  if (!guard.valid() || w->resize_pending) return;
  if (hwnd->m_style & WS_THICKFRAME)
    libdecor_frame_set_capabilities(w->frame, LIBDECOR_ACTION_RESIZE);
  else
    libdecor_frame_unset_capabilities(w->frame, LIBDECOR_ACTION_RESIZE);
  libdecor_frame_set_min_content_size(w->frame, max(1, ceil_scale(info.ptMinTrackSize.x, w->scale)),
      max(1, ceil_scale(info.ptMinTrackSize.y, w->scale)));
  libdecor_frame_set_max_content_size(w->frame, max(1, ceil_scale(info.ptMaxSize.x, w->scale)),
      max(1, ceil_scale(info.ptMaxSize.y, w->scale)));
  if (w->configured) commit_size(w);
}

void swell_oswindow_manage_wayland(HWND hwnd, bool wantfocus)
{
  if (!hwnd) return;
  SWELL_WaylandHWNDGuard hold(hwnd);
  const bool wanted = !hwnd->m_parent && hwnd->m_visible;
  if (!wanted)
  {
    update_gl_visibility(hwnd);
    xw_bridge_attach_children(hwnd);
    if (hwnd->m_oswindow) swell_oswindow_destroy_wayland(hwnd);
    return;
  }
  if (!hwnd->m_oswindow && swell_wayland_init())
  {
    SWELL_WaylandWindow *parent = window_from_hwnd(hwnd->m_owner);
    const bool popup = (hwnd->m_style & WS_CHILD) != 0;
    if (popup)
    {
      if (cursor_window || pointer_window) parent = cursor_window ? cursor_window : pointer_window;
      while (parent && parent->role == SWELL_WAYLAND_SUBSURFACE) parent = parent->parent;
      if (!parent) parent = keyboard_window;
      for (int i = windows.GetSize() - 1; i >= 0; --i)
        if (windows.Get(i)->popup && windows.Get(i)->mapped && !windows.Get(i)->callbacks.draw)
        {
          parent = windows.Get(i);
          break;
        }
    }
    const bool drag_preview = popup && !hwnd->m_owner && transfer_drag_active() && native_drag_icon();
    SWELL_WaylandWindow *w = drag_preview ? drag_icon : new SWELL_WaylandWindow;
    w->hwnd = hwnd;
    w->role = drag_preview ? SWELL_WAYLAND_DRAG_ICON :
      popup && parent ? SWELL_WAYLAND_POPUP : SWELL_WAYLAND_TOPLEVEL;
    const int previous_scale = (int)(INT_PTR)GetProp(hwnd, "swell-native-scale");
    if (!drag_preview)
      w->scale = parent && w->role != SWELL_WAYLAND_TOPLEVEL ? parent->scale :
        previous_scale ? previous_scale : initial_scale();
    w->width = max(1, ceil_scale(hwnd->m_position.right - hwnd->m_position.left, w->scale));
    w->height = max(1, ceil_scale(hwnd->m_position.bottom - hwnd->m_position.top, w->scale));
    POINT origin = parent ? window_origin(parent) : POINT{};
    if (drag_preview) ::GetCursorPos(&origin);
    w->x = hwnd->m_position.left - origin.x;
    w->y = hwnd->m_position.top - origin.y;
    hwnd->m_oswindow = w;
    if (!hwnd->m_backingstore) hwnd->m_backingstore = new SWELL_WaylandBitmap;
    const bool grab = hwnd->m_classname && !strcmp(hwnd->m_classname, "__SWELL_MENU");
    if (!drag_preview && !create_surface(w, parent, grab))
      destroy_native(w);
    else
    {
      w->mapped = true;
      SWELL_WaylandWindowGuard guard(w);
      swell_recalcMinMaxInfo_wayland(hwnd);
      if (guard.valid())
      {
        if (hwnd->m_oswindow_fullscreen && w->frame) libdecor_frame_set_fullscreen(w->frame, NULL);
        if (hwnd->m_is_maximized && w->frame) libdecor_frame_set_maximized(w->frame);
        xw_bridge_attach_children(hwnd);
        restore_gl_views(hwnd);
        WDL_PtrList<HWND__> owned;
        for (HWND child = SWELL_topwindows; child; child = child->m_next)
          if (child->m_visible && !child->m_oswindow && child->m_owner &&
              window_from_hwnd(child->m_owner) == w)
          {
            child->Retain();
            owned.Add(child);
          }
        for (HWND child : owned)
        {
          if (guard.valid() && !child->m_hashaddestroy) swell_oswindow_manage_wayland(child, false);
          child->Release();
        }
        if (guard.valid() && wantfocus) swell_oswindow_focus_wayland(hwnd);
      }
    }
  }
  if (hwnd->m_oswindow) swell_oswindow_update_text_wayland(hwnd);
}

static void activation_done_cb(void *data, xdg_activation_token_v1 *object, const char *value)
{
  SWELL_WaylandWindow *window = (SWELL_WaylandWindow *)data;
  if (!window->dead) xdg_activation_v1_activate(activation, value, window->surface);
  xdg_activation_token_v1_destroy(object);
  window->release();
}

static void activate_window(SWELL_WaylandWindow *w)
{
  if (!w || w->dead) return;
  while (w->parent && w->role == SWELL_WAYLAND_SUBSURFACE) w = w->parent;
  if (activation && activation_token.GetLength())
  {
    xdg_activation_v1_activate(activation, activation_token.Get(), w->surface);
    activation_token.Set("");
    return;
  }
  if (activation && input_serial && seat)
  {
    xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation);
    w->retain();
    static const xdg_activation_token_v1_listener listener = {activation_done_cb};
    xdg_activation_token_v1_add_listener(token, &listener, w);
    xdg_activation_token_v1_set_serial(token, input_serial, seat);
    if (keyboard_window && keyboard_window->surface)
      xdg_activation_token_v1_set_surface(token, keyboard_window->surface);
    xdg_activation_token_v1_commit(token);
  }
}

void swell_oswindow_focus_wayland(HWND hwnd)
{
  if (!hwnd)
  {
    SWELL_focused_oswindow = NULL;
    return;
  }
  SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
  if (w && (w->popup || w == keyboard_window)) SWELL_focused_oswindow = w;
  if (!w || !w->popup) activate_window(w);
}

void swell_oswindow_maximize_wayland(HWND hwnd, bool maximize)
{
  if (!hwnd) return;
  hwnd->m_is_maximized = maximize;
  SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
  if (w && w->frame)
  {
    SWELL_WaylandWindowGuard guard(w);
    finish_resize(w);
    if (!guard.valid()) return;
    if (maximize)
      libdecor_frame_set_maximized(w->frame);
    else
    {
      constrain_size(w);
      commit_size(w);
      libdecor_frame_unset_maximized(w->frame);
    }
  }
}

void swell_oswindow_update_style_wayland(HWND hwnd, LONG)
{
  SWELL_WaylandWindow *w = hwnd ? (SWELL_WaylandWindow *)hwnd->m_oswindow : NULL;
  if (w && w->frame)
  {
    if (!(w->frame_state & constrained_states))
    {
      w->normal_width = w->width;
      w->normal_height = w->height;
    }
    libdecor_frame_set_visibility(w->frame, (hwnd->m_style & WS_CAPTION) != 0);
  }
  swell_recalcMinMaxInfo_wayland(hwnd);
}

void swell_oswindow_update_enable_wayland(HWND) {}
void swell_oswindow_begin_resize_wayland(SWELL_OSWINDOW) {}
void swell_oswindow_resize_wayland(SWELL_OSWINDOW native, int flags, RECT rect)
{
  SWELL_WaylandWindow *w = (SWELL_WaylandWindow *)native;
  if (!w || w->dead) return;
  if (flags & 1)
  {
    if (w == drag_icon)
    {
      POINT origin;
      ::GetCursorPos(&origin);
      w->x = rect.left - origin.x;
      w->y = rect.top - origin.y;
    }
    else
    {
      w->x = rect.left;
      w->y = rect.top;
    }
    if (w->subsurface && w->parent)
    {
      POINT origin = window_origin(w->parent);
      w->x -= origin.x;
      w->y -= origin.y;
      wl_subsurface_set_position(
          w->subsurface, floor_scale(w->x, w->parent->scale), floor_scale(w->y, w->parent->scale));
      commit_parent(w->parent);
    }
  }
  if (!(flags & 2)) return;
  int width = max(1, ceil_scale(rect.right - rect.left, w->scale)),
      height = max(1, ceil_scale(rect.bottom - rect.top, w->scale));
  request_size(w, width, height);
  if (w->configured)
    configure_window(w, width, height);
  else
  {
    w->width = width;
    w->height = height;
  }
}

void swell_oswindow_postresize_wayland(HWND hwnd, RECT)
{
  xw_bridge_attach_children(hwnd);
  update_gl_visibility(hwnd);
  swell_recalcMinMaxInfo_wayland(hwnd);
  SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
  if (w && !w->dead)
  {
    w->invalid = bounds(w);
    w->pending = true;
    schedule(w);
  }
}

void swell_oswindow_invalidate_wayland(HWND hwnd, const RECT *rect)
{
  SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
  if (!w || w->dead) return;
  w->invalid = rect_union(w->invalid, rect ? *rect : bounds(w));
  w->pending = true;
  schedule(w);
}

void UpdateWindow_wayland(HWND) {}

bool GetWindowRect_wayland(HWND hwnd, RECT *rect)
{
  if (!hwnd || !rect) return false;
  hwnd->Retain();
  *rect = hwnd->m_position;
  if (hwnd->m_parent)
  {
    POINT p = {0, 0};
    ClientToScreen(hwnd->m_parent, &p);
    if (!hwnd->m_hashaddestroy) OffsetRect(rect, p.x, p.y);
  }
  const bool valid = !hwnd->m_hashaddestroy;
  hwnd->Release();
  return valid;
}

void GetCursorPos_wayland(POINT *point)
{
  if (point && !transfer_cursor_pos(point)) *point = cursor_position;
}

WORD GetAsyncKeyState_wayland(int key)
{
  WORD value;
  if (transfer_async_key_state(key, &value)) return value;
  if (key == VK_LBUTTON) return (mouse_buttons | touch_buttons) & 256 ? 0x8000 : 0;
  if (key == VK_MBUTTON) return mouse_buttons & 512 ? 0x8000 : 0;
  if (key == VK_RBUTTON) return mouse_buttons & 1024 ? 0x8000 : 0;
  if (key == VK_SHIFT) return modifiers & 1 ? 0x8000 : 0;
  if (key == VK_CONTROL) return modifiers & 4 ? 0x8000 : 0;
  if (key == VK_MENU) return modifiers & 8 ? 0x8000 : 0;
  if (key == VK_LWIN || key == VK_RWIN) return modifiers & 64 ? 0x8000 : 0;
  return 0;
}

DWORD GetMessagePos_wayland()
{
  return message_position;
}

int SWELL_KeyToASCII_wayland(int, int, int *)
{
  return 0;
}

int swell_is_app_inactive_wayland()
{
  return app_inactive ? 1 : 0;
}

int SWELL_SetWindowLevel_wayland(HWND hwnd, int level)
{
  int old = hwnd ? hwnd->m_israised : 0;
  if (hwnd) hwnd->m_israised = level;
  return old;
}

void swell_oswindow_begin_drag_wayland(HWND hwnd)
{
  transfer_begin_drag(hwnd);
}

bool swell_oswindow_from_point_wayland(POINT point, HWND *hwnd)
{
  if (transfer_from_point(point, hwnd)) return true;
  SWELL_WaylandWindow *window = cursor_window ? cursor_window : pointer_window;
  if (!window || !window->hwnd) return false;
  HWND root = window->hwnd;
  SWELL_WaylandHWNDGuard guard(root);
  RECT rect;
  GetWindowContentViewRect(root, &rect);
  if (!guard.valid())
  {
    *hwnd = NULL;
    return true;
  }
  point.x -= rect.left;
  point.y -= rect.top;
  *hwnd = ChildWindowFromPoint(root, point);
  return true;
}

bool swell_oswindow_can_reuse_wayland()
{
  return false;
}

void swell_oswindow_end_modal_wayland(HWND hwnd)
{
  if (hwnd && hwnd->m_owner) swell_oswindow_focus_wayland(hwnd->m_owner);
}

static RECT output_rect(SWELL_WaylandOutput *output)
{
  if (!rect_empty(output->logical))
  {
    const RECT &r = output->logical;
    return {r.left * output->scale, r.top * output->scale, r.right * output->scale, r.bottom * output->scale};
  }
  const bool rotated = (output->transform & 1) != 0;
  RECT rect = {output->x * output->scale, output->y * output->scale,
      output->x * output->scale + (rotated ? output->height : output->width),
      output->y * output->scale + (rotated ? output->width : output->height)};
  return rect;
}

void SWELL_GetViewPort_wayland(RECT *rect, const RECT *source, bool)
{
  *rect = {0, 0, 1024, 768};
  if (!swell_wayland_init() || !outputs.GetSize()) return;
  if (source && source->left == source->right && source->top == source->bottom)
  {
    SWELL_WaylandWindow *w = cursor_window ? cursor_window : keyboard_window;
    while (w && w->role != SWELL_WAYLAND_TOPLEVEL) w = w->parent;
    if (w && w->outputs.GetSize())
    {
      SWELL_WaylandOutput *output = w->outputs.Get(0);
      RECT area = output_rect(output);
      const POINT origin = window_origin(w);
      *rect = {origin.x, origin.y,
          origin.x + (area.right - area.left) / output->scale * w->scale,
          origin.y + (area.bottom - area.top) / output->scale * w->scale};
      return;
    }
  }
  SWELL_WaylandOutput *chosen = outputs.Get(0);
  int64_t best = -1;
  for (SWELL_WaylandOutput *output : outputs)
  {
    RECT area = output_rect(output), intersection;
    if (source && IntersectRect(&intersection, source, &area))
    {
      int64_t size =
          (int64_t)(intersection.right - intersection.left) * (intersection.bottom - intersection.top);
      if (size > best)
      {
        best = size;
        chosen = output;
      }
    }
  }
  *rect = output_rect(chosen);
}

BOOL EnumDisplayMonitors_wayland(HDC dc, const LPRECT clip, MONITORENUMPROC proc, LPARAM data)
{
  if (!swell_wayland_init()) return FALSE;
  for (int i = 0; i < outputs.GetSize(); ++i)
  {
    RECT r = output_rect(outputs.Get(i)), intersection;
    if (clip)
    {
      if (!IntersectRect(&intersection, &r, clip)) continue;
      r = intersection;
    }
    if (!proc((HMONITOR)(uintptr_t)(i + 1), dc, &r, data)) break;
  }
  return TRUE;
}

BOOL GetMonitorInfo_wayland(HMONITOR monitor, void *info)
{
  size_t index = (uintptr_t)monitor - 1;
  MONITORINFOEX *result = (MONITORINFOEX *)info;
  if (!result || index >= (size_t)outputs.GetSize() || result->cbSize < sizeof(MONITORINFO)) return FALSE;
  result->rcMonitor = result->rcWork = output_rect(outputs.Get(index));
  result->dwFlags = index == 0 ? 1 : 0;
  if (result->cbSize > sizeof(MONITORINFO))
    lstrcpyn_safe(result->szDevice, outputs.Get(index)->label.Get(), result->cbSize - sizeof(MONITORINFO));
  return TRUE;
}

void swell_scaling_init_wayland(bool disabled)
{
  if ((!disabled || g_swell_ui_scale != 256) && swell_wayland_init())
    g_swell_ui_scale *= initial_scale();
}

bool swell_oswindow_set_fullscreen_wayland(HWND hwnd, int mode)
{
  if (!hwnd) return false;
  const bool previous = hwnd->m_oswindow_fullscreen;
  if (mode >= 0)
  {
    hwnd->m_oswindow_fullscreen = mode != 0;
    SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
    if (w && w->frame)
    {
      SWELL_WaylandWindowGuard guard(w);
      finish_resize(w);
      if (guard.valid())
      {
        if (mode)
          libdecor_frame_set_fullscreen(w->frame, NULL);
        else
        {
          constrain_size(w);
          commit_size(w);
          libdecor_frame_unset_fullscreen(w->frame);
        }
      }
    }
  }
  return previous;
}

void swell_oswindow_reactivate_app_wayland(const char *token)
{
  if (token && *token) activation_token.Set(token);
  HWND hwnd = GetFocus();
  if (!hwnd) hwnd = SWELL_topwindows;
  swell_oswindow_focus_wayland(hwnd);
}

void swell_oswindow_prevent_screensaver_wayland(bool prevent, const char *description)
{
  static GDBusProxy *proxy;
  static guint cookie;
  if (!proxy)
    proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
        "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", NULL,
        NULL);
  if (!proxy) return;
  if (prevent && !cookie)
  {
    GVariant *reply = g_dbus_proxy_call_sync(proxy, "Inhibit",
        g_variant_new("(ss)", g_swell_appname ? g_swell_appname : "SWELL", description ? description : ""),
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (reply)
    {
      g_variant_get(reply, "(u)", &cookie);
      g_variant_unref(reply);
    }
  }
  else if (!prevent && cookie)
  {
    g_dbus_proxy_call(
        proxy, "UnInhibit", g_variant_new("(u)", cookie), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    cookie = 0;
  }
}

void *SWELL_GetOSWindow_wayland(HWND hwnd, const char *type)
{
  SWELL_WaylandWindow *w = window_from_hwnd(hwnd);
  if (!strcmp(type, "wl_display")) return display;
  if (w && !strcmp(type, "wl_surface")) return w->surface;
  return NULL;
}

void *SWELL_GetOSEvent_wayland(const char *)
{
  return NULL;
}

HWND SWELL_CreateXBridgeWindow_wayland(HWND parent, void **window, const RECT *rect)
{
  return xw_bridge_create(parent, window, rect, "__swell_xbridgewndclass");
}

SWELL_WaylandView *swell_wayland_view_create(HWND owner, SWELL_WaylandView *parent, const RECT *rect,
    SWELL_WaylandSurfaceRole role, const SWELL_WaylandViewCallbacks *callbacks, void *data)
{
  if (!swell_wayland_init() || !rect) return NULL;
  SWELL_WaylandWindow *native_parent = parent ? (SWELL_WaylandWindow *)parent : window_from_hwnd(owner);
  if (role != SWELL_WAYLAND_SUBSURFACE)
    while (native_parent && !role_surface(native_parent)) native_parent = native_parent->parent;
  if (!native_parent) return NULL;
  SWELL_WaylandView *view = new SWELL_WaylandView;
  view->hwnd = owner;
  view->callbacks = *callbacks;
  view->data = data;
  view->is_view = true;
  view->role = role;
  view->scale = native_parent->scale;
  view->x = rect->left;
  view->y = rect->top;
  view->width = max(1, ceil_scale(rect->right - rect->left, view->scale));
  view->height = max(1, ceil_scale(rect->bottom - rect->top, view->scale));
  if (!create_surface(view, native_parent, false))
  {
    destroy_native(view);
    return NULL;
  }
  return view;
}

void swell_wayland_view_set_decorated(SWELL_WaylandView *view, bool decorated)
{
  if (view && !view->dead && view->frame) libdecor_frame_set_visibility(view->frame, decorated);
}

void swell_wayland_view_set_size_limits(
    SWELL_WaylandView *view, int min_width, int min_height, int max_width, int max_height)
{
  if (!view || view->dead) return;
  view->size_limits = {min(max_window_size_pixels, max(0, min_width)),
      min(max_window_size_pixels, max(0, min_height)), min(max_window_size_pixels, max(0, max_width)),
      min(max_window_size_pixels, max(0, max_height))};
  apply_view_size_limits(view);
  if (view->frame && view->configured && !view->resize_pending) commit_size(view);
}

void swell_wayland_view_destroy(SWELL_WaylandView *view)
{
  destroy_native(view);
}

void swell_wayland_view_reset_gl(SWELL_WaylandView *view)
{
  SWELL_WaylandWindowGuard guard(view);
  SWELL_WaylandGLBinding binding;
  if (!guard.valid()) return;
  view->gl_failed = false;
  if (view->egl_surface != EGL_NO_SURFACE && make_current(view) && view->callbacks.gl_reset)
    view->callbacks.gl_reset(view, view->data);
  clear_gl();
}

void swell_wayland_view_cancel_input(SWELL_WaylandView *view)
{
  if (!view || view->dead) return;
  if (button_window == view)
  {
    button_window = NULL;
    button_serial = 0;
    if (touch_window != view) mouse_buttons = 0;
  }
  if (touch_window == view)
  {
    touch_window = NULL;
    touch_id = -1;
    touch_buttons = 0;
  }
  view->wheel[0] = view->wheel[1] = 0;
}

void swell_wayland_view_move(SWELL_WaylandView *view, const RECT *rect)
{
  if (!view || view->dead || !rect) return;
  view->x = rect->left;
  view->y = rect->top;
  if (view->popup && xdg_popup_get_version(view->popup) >= 3)
  {
    view->width = max(1, ceil_scale(rect->right - rect->left, view->scale));
    view->height = max(1, ceil_scale(rect->bottom - rect->top, view->scale));
    xdg_positioner *positioner = popup_positioner(view);
    xdg_popup_reposition(view->popup, positioner, 0);
    xdg_positioner_destroy(positioner);
    view->configured = false;
    return;
  }
  if (view->subsurface && view->parent)
  {
    wl_subsurface_set_position(view->subsurface, floor_scale(view->x, view->parent->scale),
        floor_scale(view->y, view->parent->scale));
    commit_parent(view->parent);
  }
  const int width = max(1, ceil_scale(rect->right - rect->left, view->scale));
  const int height = max(1, ceil_scale(rect->bottom - rect->top, view->scale));
  if (width != view->width || height != view->height) request_size(view, width, height);
  configure_window(view, width, height);
}

void swell_wayland_view_show(SWELL_WaylandView *view, bool visible)
{
  if (!view || view->dead) return;
  SWELL_WaylandWindowGuard guard(view);
  SWELL_WaylandWindow *root = view;
  while (root->subsurface && root->parent) root = root->parent;
  if (view->subsurface) notify_view_focus(view, visible && root == keyboard_window);
  if (!guard.valid() || view->mapped == visible) return;
  view->mapped = visible;
  if (visible)
  {
    view->invalid = bounds(view);
    view->pending = true;
    schedule(view);
  }
  else
  {
    swell_wayland_view_cancel_input(view);
    if (view->callback)
    {
      wl_callback_destroy(view->callback);
      view->callback = NULL;
    }
    wl_surface_attach(view->surface, NULL, 0, 0);
    wl_surface_commit(view->surface);
  }
}

void swell_wayland_view_invalidate(SWELL_WaylandView *view, const RECT *rect)
{
  if (!view || view->dead) return;
  view->invalid = rect_union(view->invalid, rect ? *rect : bounds(view));
  view->pending = true;
  schedule(view);
}

void swell_wayland_view_focus(SWELL_WaylandView *view)
{
  activate_window(view);
}

void swell_wayland_view_present(SWELL_WaylandView *view)
{
  swell_wayland_view_show(view, true);
  swell_wayland_view_focus(view);
}

void swell_wayland_view_set_title(SWELL_WaylandView *view, const char *title)
{
  if (view && !view->dead && view->frame) libdecor_frame_set_title(view->frame, title ? title : "");
}

static wl_region *make_region(SWELL_WaylandWindow *w, const RECT *rects, int count)
{
  wl_region *region = wl_compositor_create_region(compositor);
  for (int i = 0; i < count; ++i)
  {
    const RECT &r = rects[i];
    int x = floor_scale(r.left, w->scale), y = floor_scale(r.top, w->scale);
    wl_region_add(region, x, y, ceil_scale(r.right, w->scale) - x, ceil_scale(r.bottom, w->scale) - y);
  }
  return region;
}

void swell_wayland_view_set_input_region(SWELL_WaylandView *view, const RECT *rects, int count)
{
  if (!view || view->dead) return;
  wl_region *region = make_region(view, rects, count);
  wl_surface_set_input_region(view->surface, region);
  wl_region_destroy(region);
  commit_parent(view);
}

void swell_wayland_view_set_input(SWELL_WaylandView *view, bool enabled)
{
  if (!view || view->dead) return;
  if (!enabled)
  {
    swell_wayland_view_cancel_input(view);
    swell_wayland_view_set_input_region(view, NULL, 0);
  }
  else
  {
    wl_surface_set_input_region(view->surface, NULL);
    commit_parent(view);
  }
}

void swell_wayland_view_rect(SWELL_WaylandView *view, RECT *rect)
{
  if (!rect) return;
  *rect = {};
  if (view && !view->dead)
    *rect = {view->x, view->y, view->x + view->width * view->scale, view->y + view->height * view->scale};
}

void swell_wayland_view_output(SWELL_WaylandView *view, RECT *rect)
{
  if (!rect) return;
  SWELL_WaylandWindow *w = view;
  while (w && !w->outputs.GetSize()) w = w->parent;
  if (w)
    *rect = output_rect(w->outputs.Get(0));
  else
    SWELL_GetViewPort_wayland(rect, NULL, false);
}

wl_surface *swell_wayland_view_surface(SWELL_WaylandView *view)
{
  return view && !view->dead ? view->surface : NULL;
}

static WDL_KeyedArray<HWND, SWELL_WaylandView *> gl_views;
static void update_gl_visibility(HWND hwnd)
{
  WDL_PtrList<SWELL_WaylandView> pending;
  for (const auto &item : gl_views)
    for (HWND ancestor = item.key; ancestor; ancestor = ancestor->m_parent)
      if (ancestor == hwnd)
      {
        item.val->retain();
        pending.Add(item.val);
        break;
      }
  for (SWELL_WaylandView *view : pending)
  {
    if (!view->dead) refresh_gl_view(view);
    view->release();
  }
}

static SWELL_WaylandWindow *current_gl_view;
static void destroy_gl_view(HWND hwnd)
{
  SWELL_WaylandView *view = gl_views.Get(hwnd);
  if (view)
  {
    gl_views.Delete(hwnd);
    swell_wayland_view_destroy(view);
  }
}

static void restore_gl_views(HWND hwnd)
{
  SWELL_WaylandHWNDGuard guard(hwnd);
  if (GetProp(hwnd, "swell-native-gl")) SWELL_SetViewGL_wayland(hwnd, 1);
  if (!guard.valid()) return;
  WDL_PtrList<HWND__> children;
  for (HWND child = hwnd->m_children; child; child = child->m_next)
  {
    child->Retain();
    children.Add(child);
  }
  for (HWND child : children)
  {
    if (!child->m_hashaddestroy && child->m_parent == hwnd) restore_gl_views(child);
    child->Release();
  }
}

static void forget_gl_window(SWELL_WaylandWindow *w)
{
  if (current_gl_view == w) current_gl_view = NULL;
  for (int i = gl_views.GetSize() - 1; i >= 0; --i)
    if (gl_views.Enumerate(i) == w) gl_views.DeleteByIndex(i);
}

static RECT gl_view_rect(HWND hwnd, SWELL_WaylandWindow *parent)
{
  SWELL_WaylandWindowGuard guard(parent);
  RECT rect = {};
  GetClientRect(hwnd, &rect);
  if (hwnd->m_hashaddestroy || !guard.valid()) return rect;
  POINT origin = {0, 0};
  ClientToScreen(hwnd, &origin);
  POINT native = window_origin(parent);
  OffsetRect(&rect, origin.x - native.x, origin.y - native.y);
  return rect;
}

static bool refresh_gl_view(SWELL_WaylandView *view)
{
  SWELL_WaylandWindowGuard guard(view);
  if (!guard.valid() || !view->parent) return false;
  const RECT rect = gl_view_rect(view->hwnd, view->parent);
  if (!guard.valid()) return false;
  RECT clip = rect;
  for (HWND ancestor = view->hwnd->m_parent; ancestor;)
  {
    SWELL_WaylandHWNDGuard hold(ancestor);
    RECT client = gl_view_rect(ancestor, view->parent);
    if (!guard.valid() || !hold.valid()) return false;
    IntersectRect(&clip, &clip, &client);
    if (ancestor == view->parent->hwnd) break;
    ancestor = ancestor->m_parent;
  }
  const bool visible = IsWindowVisible(view->hwnd) && !rect_empty(clip);
  if (!visible)
  {
    swell_wayland_view_show(view, false);
    return true;
  }
  if (view->mapped && view->gl_scale == view->scale && !memcmp(&view->gl_rect, &rect, sizeof(rect)) &&
      !memcmp(&view->gl_clip, &clip, sizeof(clip)))
    return true;
  view->gl_rect = rect;
  view->gl_clip = clip;
  view->gl_scale = view->scale;
  const int width = max(1, ceil_scale(rect.right - rect.left, view->scale));
  const int height = max(1, ceil_scale(rect.bottom - rect.top, view->scale));
  if (width != view->width || height != view->height)
  {
    wl_surface_attach(view->surface, NULL, 0, 0);
    configure_window(view, width, height);
  }
  view->x = clip.left;
  view->y = clip.top;
  view->clip_x = clip.left - rect.left;
  view->clip_y = clip.top - rect.top;
  wl_subsurface_set_position(
      view->subsurface, floor_scale(clip.left, view->scale), floor_scale(clip.top, view->scale));
  if (!view->viewport) view->viewport = wp_viewporter_get_viewport(viewporter, view->surface);
  wp_viewport_set_source(view->viewport, wl_fixed_from_double((double)view->clip_x / view->scale),
      wl_fixed_from_double((double)view->clip_y / view->scale),
      wl_fixed_from_double((double)(clip.right - clip.left) / view->scale),
      wl_fixed_from_double((double)(clip.bottom - clip.top) / view->scale));
  wp_viewport_set_destination(view->viewport, ceil_scale(clip.right - clip.left, view->scale),
      ceil_scale(clip.bottom - clip.top, view->scale));
  swell_wayland_view_show(view, true);
  wl_surface_commit(view->surface);
  commit_parent(view->parent);
  return true;
}

static void cancel_gl_view(HWND hwnd)
{
  if (hwnd->m_hashaddestroy) return;
  RemoveProp(hwnd, "swell-native-gl");
  xw_bridge_set_external_gl(hwnd, false);
}

void SWELL_SetViewGL_wayland(HWND hwnd, char mode)
{
  if (!hwnd) return;
  SWELL_WaylandHWNDGuard hold(hwnd);
  SWELL_WaylandGLBinding binding;
  SWELL_WaylandView *view = gl_views.Get(hwnd);
  if (!mode)
  {
    RemoveProp(hwnd, "swell-native-gl");
    if (view != NULL)
    {
      gl_views.Delete(hwnd);
      swell_wayland_view_destroy(view);
    }
    if (hold.valid()) xw_bridge_set_external_gl(hwnd, false);
    return;
  }
  if (view != NULL || !xw_bridge_set_external_gl(hwnd, true) || !hold.valid()) return;
  SetProp(hwnd, "swell-native-gl", (HANDLE)(INT_PTR)mode);
  SWELL_WaylandWindow *parent = window_from_hwnd(hwnd);
  if (!parent) return;
  if (!viewporter || !init_egl())
  {
    cancel_gl_view(hwnd);
    return;
  }
  RECT rect = gl_view_rect(hwnd, parent);
  if (!hold.valid() || window_from_hwnd(hwnd) != parent)
  {
    cancel_gl_view(hwnd);
    return;
  }
  SWELL_WaylandViewCallbacks callbacks = {};
  view = swell_wayland_view_create(hwnd, NULL, &rect, SWELL_WAYLAND_SUBSURFACE, &callbacks, NULL);
  if (!view)
  {
    cancel_gl_view(hwnd);
    return;
  }
  view->gl_external = true;
  swell_wayland_view_show(view, IsWindowVisible(hwnd));
  const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
  view->gl_context = eglCreateContext(egl_display, egl_config, egl_context, attributes);
  if (view->gl_context == EGL_NO_CONTEXT)
  {
    swell_wayland_view_destroy(view);
    cancel_gl_view(hwnd);
    return;
  }
  gl_views.Insert(hwnd, view);
  refresh_gl_view(view);
}

bool SWELL_GetViewGL_wayland(HWND hwnd)
{
  return gl_views.Exists(hwnd);
}

bool SWELL_SetGLContextToView_wayland(HWND hwnd)
{
  if (!hwnd)
  {
    SWELL_WaylandWindow *view = current_gl_view;
    current_gl_view = NULL;
    if (view && !view->dead && view->mapped && view->configured)
    {
      cancel_read();
      if ((eglGetCurrentContext() == view->gl_context || make_current(view)) &&
          eglSwapBuffers(egl_display, view->egl_surface) && view->parent)
        commit_parent(view->parent);
    }
    clear_gl();
    return true;
  }
  SWELL_WaylandView *view = gl_views.Get(hwnd);
  if (!view) return false;
  SWELL_WaylandWindowGuard guard(view);
  if (!refresh_gl_view(view) || !guard.valid()) return false;
  if (!make_current(view)) return false;
  current_gl_view = view;
  return true;
}

static wl_surface *native_drag_icon()
{
  if (!drag_icon)
  {
    drag_icon = new SWELL_WaylandWindow;
    drag_icon->role = SWELL_WAYLAND_DRAG_ICON;
    if (!create_surface(drag_icon, NULL, false))
    {
      destroy_native(drag_icon);
      drag_icon = NULL;
    }
    else
    {
      drag_icon->scale = button_window ? button_window->scale : 1;
      wl_surface_set_buffer_scale(drag_icon->surface, drag_icon->scale);
      drag_icon->mapped = false;
      drag_icon->pending = false;
    }
  }
  return drag_icon ? drag_icon->surface : NULL;
}

static wl_data_device_manager *data_manager;
static wl_data_device *data_device;

struct SWELL_WaylandTransferOffer
{
  wl_data_offer *proxy;
  WDL_StringKeyedArray<bool> types;
  int refs = 1;
  void retain() { ++refs; }
  void release()
  {
    if (!--refs) delete this;
  }
  uint32_t action = 0;
  explicit SWELL_WaylandTransferOffer(wl_data_offer *p) : proxy(p) {}
  ~SWELL_WaylandTransferOffer() { wl_data_offer_destroy(proxy); }
  bool has(const char *type) const { return types.Exists(type); }
};

struct SWELL_WaylandTransferSource
{
  wl_data_source *proxy = NULL;
  WDL_StringKeyedArray<WDL_FastString *> data{true, wdl_deletefunc<WDL_FastString *>};
  int refs = 1;
  void retain() { ++refs; }
  void release()
  {
    if (!--refs) delete this;
  }
  HWND hwnd = NULL;
  void (*callback)(const char *) = NULL;
  WDL_FastString filename;
  uint32_t action = 0;
  bool generated = false, internal = false, finished = false, released = false;
  bool cancelled = false, outside_release = false;
  ~SWELL_WaylandTransferSource()
  {
    if (proxy) wl_data_source_destroy(proxy);
    if (hwnd) hwnd->Release();
  }
};

static WDL_KeyedArray<wl_data_offer *, SWELL_WaylandTransferOffer *> pending_offers;
static SWELL_WaylandTransferOffer *selection_offer, *drag_offer;
static SWELL_WaylandTransferSource *selection_source, *drag_source;
static HWND drag_destination;
static SWELL_WaylandWindow *drag_window;
static POINT drag_position;
static uint64_t drag_generation;
static bool drag_dropped;
static WDL_PtrList_DeleteOnDestroy<char> clipboard_formats(free);
static WDL_IntKeyedArray<HANDLE> clipboard_contents;
static HANDLE clipboard_read_data;
static UINT clipboard_read_type;
static bool clipboard_written;
static uint64_t clipboard_generation;

static void transfer_destination(HWND hwnd)
{
  if (hwnd == drag_destination) return;
  if (hwnd) hwnd->Retain();
  if (drag_destination) drag_destination->Release();
  drag_destination = hwnd;
}

static HANDLE transfer_dropfiles(const POINT *point, const WDL_FastString &bytes)
{
  gchar **uris = g_uri_list_extract_uris(bytes.Get());
  WDL_FastString files;
  for (gchar **uri = uris; *uri; ++uri)
  {
    gchar *host = NULL;
    gchar *path = g_filename_from_uri(*uri, &host, NULL);
    if (path &&
        (!host || !g_ascii_strcasecmp(host, "localhost") || !g_ascii_strcasecmp(host, g_get_host_name())))
    {
      files.AppendRaw(path, strlen(path) + 1);
    }
    g_free(path);
    g_free(host);
  }
  g_strfreev(uris);
  if (!files.GetLength() || files.GetLength() > INT_MAX - (int)sizeof(DROPFILES) - 1) return NULL;
  HANDLE result = GlobalAlloc(0, sizeof(DROPFILES) + files.GetLength() + 1);
  if (result)
  {
    auto *drop = (DROPFILES *)result;
    memset(drop, 0, sizeof(*drop));
    drop->pFiles = sizeof(*drop);
    if (point) drop->pt = *point;
    memcpy(drop + 1, files.Get(), files.GetLength() + 1);
  }
  return result;
}

bool swell_wayland_first_uri_path(const char *data, size_t size, char *out, size_t capacity)
{
  if (!out || !capacity) return false;
  if (size > INT_MAX - 1u) return false;
  WDL_FastString bytes;
  bytes.SetRaw(data, size);
  HANDLE files = transfer_dropfiles(NULL, bytes);
  if (!files) return false;
  const char *path = (const char *)files + ((DROPFILES *)files)->pFiles;
  const size_t len = strlen(path);
  const bool fits = len < capacity;
  if (fits) memcpy(out, path, len + 1);
  GlobalFree(files);
  return fits;
}

static void transfer_append_uri(WDL_FastString &text, const char *path)
{
  if (!path || !*path) return;
  gchar *uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri) return;
  text.Append(uri);
  text.Append("\r\n");
  g_free(uri);
}

static ssize_t transfer_write(int fd, const void *data, size_t size)
{
  sigset_t blocked, old, pending;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &blocked, &old)) return -1;
  sigpending(&pending);
  const bool had_signal = sigismember(&pending, SIGPIPE);
  const ssize_t result = write(fd, data, size);
  const int error = errno;
  if (result < 0 && error == EPIPE && !had_signal)
  {
    const timespec zero = {};
    while (sigtimedwait(&blocked, NULL, &zero) < 0 && errno == EINTR)
    {
    }
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  errno = error;
  return result;
}

struct SWELL_WaylandTransferWrite
{
  int fd;
  WDL_FastString bytes;
  int offset = 0;
  ~SWELL_WaylandTransferWrite() { close(fd); }
};

static gboolean transfer_write_ready(gint, GIOCondition condition, gpointer data)
{
  auto *out = (SWELL_WaylandTransferWrite *)data;
  if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) return G_SOURCE_REMOVE;
  size_t budget = 256 * 1024;
  while (out->offset < out->bytes.GetLength() && budget)
  {
    const size_t amount = wdl_min(budget, (size_t)(out->bytes.GetLength() - out->offset));
    const ssize_t size = transfer_write(out->fd, out->bytes.Get() + out->offset, amount);
    if (size > 0)
    {
      out->offset += size;
      budget -= size;
    }
    else if (size < 0 && errno == EINTR)
      continue;
    else if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return G_SOURCE_CONTINUE;
    else
      return G_SOURCE_REMOVE;
  }
  return out->offset < out->bytes.GetLength() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void transfer_write_free(gpointer data)
{
  delete (SWELL_WaylandTransferWrite *)data;
}

static void transfer_send(int fd, const WDL_FastString &bytes)
{
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
  {
    close(fd);
    return;
  }
  auto *out = new SWELL_WaylandTransferWrite{fd, bytes};
  g_unix_fd_add_full(G_PRIORITY_DEFAULT, fd, (GIOCondition)(G_IO_OUT | G_IO_ERR | G_IO_HUP),
      transfer_write_ready, out, transfer_write_free);
}

struct SWELL_WaylandTransferRead
{
  int fd = -1;
  guint source = 0, timeout = 0;
  bool finished = false, good = false;
  WDL_FastString bytes;
  void (*done)(SWELL_WaylandTransferRead *) = NULL;
  uint64_t generation = 0;
  HWND target = NULL;
  POINT position = {};
  SWELL_WaylandTransferOffer *offer = NULL;
  int refs = 1;
  void retain() { ++refs; }
  void release()
  {
    if (!--refs) delete this;
  }
  ~SWELL_WaylandTransferRead()
  {
    if (offer) offer->release();
    if (fd >= 0) close(fd);
    if (target) target->Release();
  }
};

static void transfer_read_finish(SWELL_WaylandTransferRead *in, bool good)
{
  if (in->finished) return;
  in->retain();
  in->finished = true;
  in->good = good;
  if (in->fd >= 0)
  {
    close(in->fd);
    in->fd = -1;
  }
  if (in->source)
  {
    g_source_remove(in->source);
    in->source = 0;
  }
  if (in->timeout)
  {
    g_source_remove(in->timeout);
    in->timeout = 0;
  }
  if (in->done) in->done(in);
  in->release();
}

static gboolean transfer_read_ready(gint, GIOCondition condition, gpointer data)
{
  SWELL_WaylandTransferRead *input = (SWELL_WaylandTransferRead *)data;
  if (condition & (G_IO_ERR | G_IO_NVAL))
  {
    transfer_read_finish(input, false);
    return G_SOURCE_REMOVE;
  }
  char buffer[16384];
  for (size_t budget = 256 * 1024; budget;)
  {
    const ssize_t size = read(input->fd, buffer, wdl_min(sizeof(buffer), budget));
    if (size > 0)
    {
      if (input->bytes.GetLength() > INT_MAX - 1 - size)
      {
        transfer_read_finish(input, false);
        return G_SOURCE_REMOVE;
      }
      const int length = input->bytes.GetLength();
      input->bytes.AppendRaw(buffer, size);
      if (input->bytes.GetLength() != length + size)
      {
        transfer_read_finish(input, false);
        return G_SOURCE_REMOVE;
      }
      budget -= size;
    }
    else if (!size)
    {
      transfer_read_finish(input, true);
      return G_SOURCE_REMOVE;
    }
    else if (errno == EINTR)
      continue;
    else if (errno == EAGAIN || errno == EWOULDBLOCK)
      return G_SOURCE_CONTINUE;
    else
    {
      transfer_read_finish(input, false);
      return G_SOURCE_REMOVE;
    }
  }
  return G_SOURCE_CONTINUE;
}

static gboolean transfer_read_timeout(gpointer data)
{
  SWELL_WaylandTransferRead *input = (SWELL_WaylandTransferRead *)data;
  input->timeout = 0;
  transfer_read_finish(input, false);
  return G_SOURCE_REMOVE;
}

static void transfer_read_free(gpointer data)
{
  ((SWELL_WaylandTransferRead *)data)->release();
}

static SWELL_WaylandTransferRead *transfer_receive(
    SWELL_WaylandTransferOffer *offer, const char *type,
    void (*done)(SWELL_WaylandTransferRead *) = NULL, HWND target = NULL, POINT position = {})
{
  SWELL_WaylandTransferRead *in = new SWELL_WaylandTransferRead;
  in->done = done;
  in->generation = drag_generation;
  in->offer = offer;
  if (offer) offer->retain();
  in->target = target;
  in->position = position;
  if (target) target->Retain();
  int fds[2];
  if (!offer || pipe2(fds, O_CLOEXEC) < 0)
  {
    transfer_read_finish(in, false);
    return in;
  }
  in->fd = fds[0];
  if (fcntl(in->fd, F_SETFL, fcntl(in->fd, F_GETFL) | O_NONBLOCK) < 0)
  {
    close(fds[1]);
    transfer_read_finish(in, false);
    return in;
  }
  wl_data_offer_receive(offer->proxy, type, fds[1]);
  close(fds[1]);
  in->retain();
  in->source = g_unix_fd_add_full(G_PRIORITY_DEFAULT, in->fd, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
      transfer_read_ready, in, transfer_read_free);
  in->retain();
  in->timeout = g_timeout_add_full(G_PRIORITY_DEFAULT, 3000, transfer_read_timeout,
      in, transfer_read_free);
  wl_display_flush(display);
  return in;
}

static void transfer_drag_mouse(UINT message)
{
  auto source = drag_source;
  if (!source || !source->internal || !source->hwnd || source->hwnd->m_hashaddestroy ||
      GetCapture() != source->hwnd)
    return;
  source->retain();
  POINT p = drag_position;
  ScreenToClient(source->hwnd, &p);
  message_position = MAKELONG(drag_position.x & 65535, drag_position.y & 65535);
  SWELL_SendMouseMessage(source->hwnd, message, 0, MAKELPARAM(p.x, p.y), drag_position.x, drag_position.y);
  source->release();
}

static void transfer_finish_source(SWELL_WaylandTransferSource *source, bool cancelled)
{
  if (!drag_source || drag_source != source || source->finished) return;
  SWELL_WaylandTransferSource *keep = drag_source;
  keep->retain();
  keep->cancelled = cancelled;
  keep->released = true;
  keep->finished = true;
  if (keep->proxy)
  {
    wl_data_source_destroy(keep->proxy);
    keep->proxy = NULL;
  }
  mouse_buttons = touch_buttons = 0;
  button_window = NULL;
  button_serial = 0;
  if (keep->internal && cancelled)
  {
    if (keep->outside_release)
    {
      keep->cancelled = false;
      transfer_drag_mouse(WM_LBUTTONUP);
    }
    else
      transfer_drag_mouse(WM_MOUSEMOVE);
  }
  if (keep->hwnd && GetCapture() == keep->hwnd) ReleaseCapture();
  transfer_destination(NULL);
  drag_window = NULL;
  if (drag_source) drag_source->release();
  drag_source = NULL;
  update_cursor();
  keep->release();
}

static void data_source_action_cb(void *data, wl_data_source *, uint32_t action)
{
  auto *source = (SWELL_WaylandTransferSource *)data;
  if (source->action == action) return;
  source->action = action;
  if (drag_source == source) update_cursor();
}

static void data_source_target_cb(void *data, wl_data_source *source, const char *type)
{
  if (wl_data_source_get_version(source) < WL_DATA_SOURCE_ACTION_SINCE_VERSION)
    data_source_action_cb(data, source, type ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0);
}

static void data_source_send_cb(void *data, wl_data_source *, const char *type, int32_t fd)
{
  auto *source = (SWELL_WaylandTransferSource *)data;
  auto hold = drag_source == source ? drag_source : selection_source;
  if (!hold || hold != source)
  {
    close(fd);
    return;
  }
  hold->retain();
  if (hold->callback && !hold->generated)
  {
    hold->generated = true;
    hold->callback(hold->filename.Get());
  }
  const WDL_FastString *bytes = hold->data.Get(type);
  if (bytes) transfer_send(fd, *bytes);
  else close(fd);
  hold->release();
}

static void data_source_cancelled_cb(void *data, wl_data_source *)
{
  auto *source = (SWELL_WaylandTransferSource *)data;
  if (selection_source == source)
  {
    selection_source = NULL;
    source->release();
  }
  else
    transfer_finish_source(source, true);
}

static void data_source_dnd_drop_performed_cb(void *data, wl_data_source *)
{
  ((SWELL_WaylandTransferSource *)data)->released = true;
}

static void data_source_dnd_finished_cb(void *data, wl_data_source *)
{
  transfer_finish_source((SWELL_WaylandTransferSource *)data, false);
}

static const wl_data_source_listener transfer_source_listener = {data_source_target_cb, data_source_send_cb,
    data_source_cancelled_cb, data_source_dnd_drop_performed_cb, data_source_dnd_finished_cb,
    data_source_action_cb};

static SWELL_WaylandTransferSource *transfer_source()
{
  if (!data_manager) return NULL;
  SWELL_WaylandTransferSource *result = new SWELL_WaylandTransferSource;
  result->proxy = wl_data_device_manager_create_data_source(data_manager);
  if (!result->proxy)
  {
    delete result;
    return NULL;
  }
  wl_data_source_add_listener(result->proxy, &transfer_source_listener, result);
  return result;
}

static const char *clipboard_format(UINT type)
{
  if (type == CF_TEXT) return "text/plain;charset=utf-8";
  if (type == CF_HDROP) return "text/uri-list";
  return clipboard_formats.Get((INT_PTR)type - 3);
}

UINT RegisterClipboardFormat_wayland(const char *name)
{
  if (!name || !*name) return 0;
  for (int i = 1; i < clipboard_formats.GetSize() + 3; ++i)
    if (!strcmp(clipboard_format(i), name)) return i;
  clipboard_formats.Add(strdup(name));
  return clipboard_formats.GetSize() + 2;
}

bool OpenClipboard_wayland(HWND)
{
  ++clipboard_generation;
  GlobalFree(clipboard_read_data);
  clipboard_read_data = NULL;
  clipboard_read_type = 0;
  return true;
}

void EmptyClipboard_wayland()
{
  for (auto &entry : clipboard_contents) GlobalFree(entry.val);
  clipboard_contents.DeleteAll();
  clipboard_written = true;
}

void SetClipboardData_wayland(UINT type, HANDLE handle)
{
  if (!clipboard_format(type))
  {
    GlobalFree(handle);
    return;
  }
  HANDLE stored = clipboard_contents.Get(type);
  if (stored != handle) GlobalFree(stored);
  clipboard_contents.Insert(type, handle);
  clipboard_written = true;
}

static WDL_FastString transfer_clipboard_bytes(UINT type, HANDLE handle)
{
  WDL_FastString data;
  const char *bytes = (const char *)handle;
  const int size = GlobalSize(handle);
  if (!bytes || size <= 0) return data;
  if (type == CF_TEXT)
  {
    for (int i = 0; i < size && bytes[i]; ++i)
    {
      if (bytes[i] == '\r' && i + 1 < size && bytes[i + 1] == '\n') continue;
      data.AppendRaw(bytes + i, 1);
    }
  }
  else if (type == CF_HDROP)
  {
    const auto *drop = (const DROPFILES *)handle;
    if (size <= (int)sizeof(*drop) || drop->fWide || drop->pFiles >= (DWORD)size) return data;
    for (const char *p = bytes + drop->pFiles; p < bytes + size && *p;)
    {
      const char *end = (const char *)memchr(p, 0, bytes + size - p);
      if (!end) break;
      transfer_append_uri(data, p);
      p = end + 1;
    }
  }
  else
    data.SetRaw(bytes, size);
  return data;
}

void CloseClipboard_wayland()
{
  if (!clipboard_written || !data_device || !input_serial) return;
  auto source = transfer_source();
  if (!source) return;
  for (const auto &entry : clipboard_contents)
  {
    const char *mime = clipboard_format(entry.key);
    if (!entry.val || !mime) continue;
    source->data.Insert(mime, new WDL_FastString(transfer_clipboard_bytes(entry.key, entry.val)));
  }
  if (const WDL_FastString *text = source->data.Get("text/plain;charset=utf-8"))
  {
    source->data.Insert("text/plain", new WDL_FastString(*text));
    source->data.Insert("UTF8_STRING", new WDL_FastString(*text));
  }
  for (const auto &entry : source->data) wl_data_source_offer(source->proxy, entry.key);
  if (selection_source) selection_source->release();
  selection_source = source;
  if (selection_offer) selection_offer->release();
  selection_offer = NULL;
  wl_data_device_set_selection(data_device, source->proxy, input_serial);
  wl_display_flush(display);
  clipboard_written = false;
}

static const char *transfer_text_type(const SWELL_WaylandTransferOffer &offer)
{
  static const char *const types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
  for (const char *type : types)
    if (offer.has(type)) return type;
  return NULL;
}

UINT EnumClipboardFormats_wayland(UINT previous)
{
  for (UINT i = previous + 1; i < (UINT)clipboard_formats.GetSize() + 3; ++i)
  {
    if ((clipboard_written || selection_source) && clipboard_contents.Exists(i)) return i;
    if (selection_offer && (i == CF_TEXT ? transfer_text_type(*selection_offer) != NULL
                                         : selection_offer->has(clipboard_format(i))))
      return i;
  }
  return 0;
}

HANDLE GetClipboardData_wayland(UINT type)
{
  if (!clipboard_format(type)) return NULL;
  if (clipboard_written || selection_source)
  {
    return clipboard_contents.Get(type);
  }
  if (clipboard_read_data && clipboard_read_type == type) return clipboard_read_data;
  auto offer = selection_offer;
  if (!offer) return NULL;
  const char *mime = type == CF_TEXT ? transfer_text_type(*offer) : clipboard_format(type);
  if (!mime || !offer->has(mime)) return NULL;
  const uint64_t generation = clipboard_generation;
  auto input = transfer_receive(offer, mime);
  while (!input->finished && generation == clipboard_generation) g_main_context_iteration(NULL, TRUE);
  if (!input->finished) transfer_read_finish(input, false);
  if (!input->good || generation != clipboard_generation)
  {
    input->release();
    return NULL;
  }
  HANDLE result;
  if (type == CF_HDROP)
    result = transfer_dropfiles(NULL, input->bytes);
  else
  {
    result = GlobalAlloc(0, input->bytes.GetLength() + (type == CF_TEXT));
    if (result)
    {
      memcpy(result, input->bytes.Get(), input->bytes.GetLength());
      if (type == CF_TEXT) ((char *)result)[input->bytes.GetLength()] = 0;
    }
  }
  input->release();
  GlobalFree(clipboard_read_data);
  clipboard_read_data = result;
  clipboard_read_type = type;
  return result;
}

static void transfer_drag_update(SWELL_WaylandWindow *window, wl_fixed_t x, wl_fixed_t y)
{
  HWND hwnd = window ? window->hwnd : NULL;
  transfer_destination(hwnd);
  if (!hwnd) return;
  drag_position = window_origin(window);
  drag_position.x += (int)floor(wl_fixed_to_double(x) * window->scale);
  drag_position.y += (int)floor(wl_fixed_to_double(y) * window->scale);
}

static void transfer_drag_preview(uint64_t generation, bool good, const WDL_FastString &bytes)
{
  if (generation != drag_generation || !drag_destination) return;
  if (!good) return;
  if (SWELL_DDrop_onDragEnter)
  {
    HANDLE files = transfer_dropfiles(NULL, bytes);
    if (files)
    {
      SWELL_DDrop_onDragEnter(files, drag_position);
      GlobalFree(files);
    }
  }
}

static void transfer_deliver_drop(HWND target, POINT position,
    SWELL_WaylandTransferOffer *offer, bool good, const WDL_FastString &bytes)
{
  if (good && target && !target->m_hashaddestroy)
  {
    RECT rect;
    GetWindowContentViewRect(target, &rect);
    POINT local = position;
    local.x -= rect.left;
    local.y -= rect.top;
    HWND child = ChildWindowFromPoint(target, local);
    if (!child) child = target;
    child->Retain();
    local = position;
    ScreenToClient(child, &local);
    HANDLE files = transfer_dropfiles(&local, bytes);
    if (files)
    {
      if (!child->m_hashaddestroy) SendMessage(child, WM_DROPFILES, (WPARAM)files, 0);
      GlobalFree(files);
    }
    child->Release();
  }
  if (good && offer && offer->action && wl_data_offer_get_version(offer->proxy) >= 3)
    wl_data_offer_finish(offer->proxy);
  if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
}

static void data_offer_offer_cb(void *data, wl_data_offer *, const char *mime)
{
  ((SWELL_WaylandTransferOffer *)data)->types.Insert(mime, true);
}

static void data_offer_source_actions_cb(void *, wl_data_offer *, uint32_t) {}

static void data_offer_action_cb(void *data, wl_data_offer *, uint32_t action)
{
  ((SWELL_WaylandTransferOffer *)data)->action = action;
}

static const wl_data_offer_listener transfer_offer_listener = {
    data_offer_offer_cb, data_offer_source_actions_cb, data_offer_action_cb};

static void data_device_data_offer_cb(void *, wl_data_device *, wl_data_offer *proxy)
{
  SWELL_WaylandTransferOffer *offer = new SWELL_WaylandTransferOffer(proxy);
  wl_data_offer_add_listener(proxy, &transfer_offer_listener, offer);
  pending_offers.Insert(proxy, offer);
}

static void transfer_preview_read(SWELL_WaylandTransferRead *in)
{
  transfer_drag_preview(in->generation, in->good, in->bytes);
}

static void transfer_drop_read(SWELL_WaylandTransferRead *in)
{
  transfer_deliver_drop(in->target, in->position, in->offer, in->good, in->bytes);
}

static void data_device_enter_cb(void *, wl_data_device *, uint32_t serial, wl_surface *surface, wl_fixed_t x,
    wl_fixed_t y, wl_data_offer *proxy)
{
  ++drag_generation;
  drag_dropped = false;
  if (drag_offer) drag_offer->release();
  drag_offer = pending_offers.Get(proxy);
  pending_offers.Delete(proxy);
  SWELL_WaylandWindow *window = window_from_surface(surface);
  drag_window = window;
  transfer_drag_update(window, x, y);
  if (!drag_offer) return;
  const bool internal = drag_source && drag_source->internal && drag_offer->has("application/x-swell-drag");
  const char *mime = internal ? "application/x-swell-drag" :
    drag_offer->has("text/uri-list") ? "text/uri-list" : NULL;
  if (!window) mime = NULL;
  wl_data_offer_accept(proxy, serial, mime);
  if (wl_data_offer_get_version(proxy) >= 3)
    wl_data_offer_set_actions(proxy, mime ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0,
        mime ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0);
  if (!mime) return;
  if (internal)
    transfer_drag_mouse(WM_MOUSEMOVE);
  else
    transfer_receive(drag_offer, mime, transfer_preview_read)->release();
}

static void data_device_leave_cb(void *, wl_data_device *)
{
  ++drag_generation;
  const bool internal = drag_source && drag_source->internal;
  transfer_destination(NULL);
  drag_window = NULL;
  if (drag_offer) drag_offer->release();
  drag_offer = NULL;
  if (internal && !drag_dropped)
  {
    for (SWELL_WaylandWindow *w : windows)
      if (w->hwnd && w->mapped && !w->is_view && w->role == SWELL_WAYLAND_TOPLEVEL)
      {
        drag_position.x = max(drag_position.x, w->hwnd->m_position.right + 1);
        drag_position.y = max(drag_position.y, w->hwnd->m_position.bottom + 1);
      }
    transfer_drag_mouse(WM_MOUSEMOVE);
  }
  else if (!internal && !drag_dropped && SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
}

static void data_device_motion_cb(void *, wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
  if (!drag_destination || drag_destination->m_hashaddestroy) return;
  SWELL_WaylandWindow *window = drag_window;
  if (!window) return;
  transfer_drag_update(window, x, y);
  if (drag_source && drag_source->internal)
    transfer_drag_mouse(WM_MOUSEMOVE);
  else if (SWELL_DDrop_onDragOver)
    SWELL_DDrop_onDragOver(drag_position);
}

static void data_device_drop_cb(void *, wl_data_device *)
{
  drag_dropped = true;
  auto offer = drag_offer;
  if (!offer || !drag_destination) return;
  if (drag_source && drag_source->internal && offer->has("application/x-swell-drag"))
  {
    offer->retain();
    drag_source->released = true;
    transfer_drag_mouse(WM_LBUTTONUP);
    if (offer->action && wl_data_offer_get_version(offer->proxy) >= 3) wl_data_offer_finish(offer->proxy);
    offer->release();
    return;
  }
  if (!offer->has("text/uri-list")) return;
  transfer_receive(offer, "text/uri-list", transfer_drop_read, drag_destination, drag_position)->release();
}

static void data_device_selection_cb(void *, wl_data_device *, wl_data_offer *proxy)
{
  if (selection_offer) selection_offer->release();
  selection_offer = pending_offers.Get(proxy);
  pending_offers.Delete(proxy);
  ++clipboard_generation;
  GlobalFree(clipboard_read_data);
  clipboard_read_data = NULL;
  clipboard_read_type = 0;
}

static const wl_data_device_listener transfer_device_listener = {data_device_data_offer_cb,
    data_device_enter_cb, data_device_leave_cb, data_device_motion_cb, data_device_drop_cb,
    data_device_selection_cb};

static bool transfer_start_drag(SWELL_WaylandTransferSource *source)
{
  if (!source || drag_source || !data_device || !button_window || !button_serial || !button_window->surface)
    return false;
  for (const auto &entry : source->data) wl_data_source_offer(source->proxy, entry.key);
  if (wl_data_source_get_version(source->proxy) >= 3)
    wl_data_source_set_actions(source->proxy, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  ::GetCursorPos(&drag_position);
  drag_icon_offset = {};
  source->retain();
  drag_source = source;
  update_cursor();
  wl_surface *icon = native_drag_icon();
  if (drag_icon) set_scale(drag_icon, button_window->scale, true);
  wl_data_device_start_drag(data_device, source->proxy, button_window->surface, icon, button_serial);
  wl_display_flush(display);
  return true;
}

static void transfer_begin_drag(HWND source)
{
  HWND capture = GetCapture();
  if (drag_source || !capture || capture == source || !button_window || !button_serial) return;
  auto drag = transfer_source();
  if (!drag) return;
  drag->internal = true;
  drag->outside_release = !source;
  drag->hwnd = capture;
  capture->Retain();
  drag->data.Insert("application/x-swell-drag", new WDL_FastString);
  transfer_start_drag(drag);
  drag->release();
}

static bool transfer_drag_active()
{
  return drag_source != NULL;
}

static const char *transfer_cursor_name()
{
  if (!drag_source || drag_source->internal || drag_source->released) return NULL;
  return drag_source->action ? "dnd-copy" : "dnd-none";
}

static bool transfer_from_point(POINT point, HWND *target)
{
  if (!drag_source && !drag_destination) return false;
  *target = drag_destination;
  if (*target && (*target)->m_hashaddestroy) *target = NULL;
  if (*target)
  {
    SWELL_WaylandHWNDGuard hold(*target);
    RECT rect;
    GetWindowContentViewRect(*target, &rect);
    point.x -= rect.left;
    point.y -= rect.top;
    *target = hold.valid() ? ChildWindowFromPoint(*target, point) : NULL;
  }
  return true;
}

static bool transfer_cursor_pos(POINT *point)
{
  if (!drag_source && !drag_destination) return false;
  *point = drag_position;
  return true;
}

static bool transfer_async_key_state(int key, WORD *state)
{
  if (!drag_source) return false;
  if (key == VK_LBUTTON)
    *state = drag_source->released ? 0 : 0x8000;
  else if (key == VK_ESCAPE)
    *state = drag_source->cancelled ? 0x8000 : 0;
  else
    return false;
  return true;
}

static void transfer_drag_files(
    HWND hwnd, const char *const *files, int count, const char *filename, void (*callback)(const char *))
{
  auto source = transfer_source();
  if (!source) return;
  if (hwnd)
  {
    hwnd->Retain();
    source->hwnd = hwnd;
  }
  WDL_FastString *uris = new WDL_FastString;
  source->data.Insert("text/uri-list", uris);
  for (int i = 0; i < count; ++i) transfer_append_uri(*uris, files[i]);
  if (filename)
  {
    source->filename.Set(filename);
    source->callback = callback;
    transfer_append_uri(*uris, filename);
  }
  if (!uris->GetLength() || !transfer_start_drag(source))
  {
    source->release();
    return;
  }
  ReleaseCapture();
  while (!source->finished && (!hwnd || !hwnd->m_hashaddestroy))
  {
    SWELL_RunMessageLoop();
    Sleep(1);
  }
  if (drag_source == source) transfer_finish_source(source, true);
  source->release();
}

void SWELL_InitiateDragDrop_wayland(HWND hwnd, RECT *, const char *filename, void (*callback)(const char *))
{
  transfer_drag_files(hwnd, NULL, 0, filename, callback);
}

void SWELL_InitiateDragDropOfFileList_wayland(HWND hwnd, RECT *, const char **files, int count, HICON)
{
  if (files && count > 0) transfer_drag_files(hwnd, files, count, NULL, NULL);
}

void SWELL_FinishDragDrop_wayland()
{
  if (drag_source) transfer_finish_source(drag_source, true);
}

static zwp_text_input_manager_v3 *text_manager;
static zwp_text_input_v3 *text_input;
static wl_surface *text_surface;
static HWND text_target;
static uint32_t text_serial, text_enabled_serial;
static uint64_t text_batch, text_input_generation;
static bool text_enabled, text_dirty, text_can_commit = true, text_applying;
static WDL_FastString text_surrounding, text_commit, text_preedit;
static int text_cursor, text_anchor, text_preedit_cursor;
static RECT text_caret;
static uint32_t text_before, text_after;

static void text_publish()
{
  if (!text_input || !text_dirty || !text_can_commit || !text_enabled) return;
  zwp_text_input_v3_set_text_change_cause(text_input,
      text_applying ? ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD : ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
  const size_t size = text_surrounding.GetLength();
  if (text_cursor >= 0 && text_anchor >= 0 && (size_t)text_cursor <= size && (size_t)text_anchor <= size)
  {
    size_t first = wdl_min(text_cursor, text_anchor), last = wdl_max(text_cursor, text_anchor);
    if (last - first <= 4000)
    {
      const size_t spare = 4000 - (last - first);
      first -= wdl_min(first, spare / 2);
      last = wdl_min(size, first + 4000);
      while (first < size && ((unsigned char)text_surrounding.Get()[first] & 0xc0) == 0x80) ++first;
      while (last < size && last > first && ((unsigned char)text_surrounding.Get()[last] & 0xc0) == 0x80) --last;
      WDL_FastString context;
      context.SetRaw(text_surrounding.Get() + first, last - first);
      zwp_text_input_v3_set_surrounding_text(
          text_input, context.Get(), text_cursor - first, text_anchor - first);
    }
  }
  zwp_text_input_v3_set_cursor_rectangle(text_input, text_caret.left, text_caret.top,
      wdl_max(1, text_caret.right - text_caret.left), wdl_max(1, text_caret.bottom - text_caret.top));
  zwp_text_input_v3_commit(text_input);
  ++text_serial;
  text_dirty = false;
}

static void text_sync_focus()
{
  SWELL_WaylandWindow *window = text_target ? window_from_hwnd(text_target) : NULL;
  const bool enabled = window && !window->dead && window->surface == text_surface;
  if (!text_input || enabled == text_enabled) return;
  text_enabled = enabled;
  text_can_commit = true;
  if (enabled)
  {
    zwp_text_input_v3_enable(text_input);
    const char *surrounding = NULL;
    if (swell_ime_get_surrounding(&surrounding, &text_cursor, &text_anchor)) text_surrounding.Set(surrounding);
    const bool password = (text_target->m_style & ES_PASSWORD) != 0;
    zwp_text_input_v3_set_content_type(text_input,
        password ? ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT | ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA
                 : ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
        password ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    text_dirty = true;
    text_enabled_serial = text_serial + 1;
    text_publish();
  }
  else
  {
    zwp_text_input_v3_disable(text_input);
    zwp_text_input_v3_commit(text_input);
    ++text_serial;
    swell_ime_preedit(NULL, 0);
  }
}

void swell_oswindow_ime_focus_wayland(HWND hwnd)
{
  if (text_target == hwnd)
  {
    text_sync_focus();
    return;
  }
  if (hwnd) hwnd->Retain();
  HWND old = text_target;
  text_target = NULL;
  text_sync_focus();
  if (old) old->Release();
  text_target = hwnd;
  text_surrounding.Set("");
  text_cursor = text_anchor = 0;
  text_commit.Set("");
  text_preedit.Set("");
  text_before = text_after = 0;
  text_sync_focus();
}

void swell_oswindow_ime_update_wayland(
    HWND hwnd, HWND root, const RECT *caret, const char *text, int cursor, int anchor)
{
  if (hwnd != text_target) return;
  SWELL_WaylandWindow *window = window_from_hwnd(root);
  if (!window) return;
  RECT rect = {caret->left / window->scale, caret->top / window->scale,
      (caret->right + window->scale - 1) / window->scale,
      (caret->bottom + window->scale - 1) / window->scale};
  const char *surrounding = text ? text : "";
  if (!strcmp(text_surrounding.Get(), surrounding) && text_cursor == cursor && text_anchor == anchor &&
      !memcmp(&text_caret, &rect, sizeof(rect)))
    return;
  text_surrounding.Set(surrounding);
  text_cursor = cursor;
  text_anchor = anchor;
  text_caret = rect;
  text_dirty = true;
  if (!text_applying) text_publish();
}

static void text_input_enter_cb(void *, zwp_text_input_v3 *, wl_surface *surface)
{
  text_surface = surface;
  text_sync_focus();
}

static void text_input_leave_cb(void *, zwp_text_input_v3 *, wl_surface *surface)
{
  if (text_surface == surface)
  {
    text_surface = NULL;
    text_sync_focus();
  }
}

static void text_input_preedit_string_cb(
    void *, zwp_text_input_v3 *, const char *text, int32_t begin, int32_t)
{
  text_preedit.Set(text && g_utf8_validate(text, -1, NULL) ? text : "");
  const int byte = wdl_max(0, wdl_min(begin, (int)text_preedit.GetLength()));
  text_preedit_cursor = g_utf8_pointer_to_offset(text_preedit.Get(), text_preedit.Get() + byte);
}

static void text_input_commit_string_cb(void *, zwp_text_input_v3 *, const char *text)
{
  text_commit.Set(text && g_utf8_validate(text, -1, NULL) ? text : "");
}

static void text_input_delete_surrounding_text_cb(
    void *, zwp_text_input_v3 *, uint32_t before, uint32_t after)
{
  text_before = before;
  text_after = after;
}

static void text_input_done_cb(void *, zwp_text_input_v3 *, uint32_t serial)
{
  WDL_FastString committed, preedit;
  committed.SwapContentsWith(&text_commit);
  preedit.SwapContentsWith(&text_preedit);
  const uint32_t before = text_before, after = text_after;
  const int preedit_cursor = text_preedit_cursor;
  text_before = text_after = 0;
  if (text_enabled && text_target == swell_ime_get_target() && (int32_t)(serial - text_enabled_serial) >= 0)
  {
    HWND target = text_target;
    const uint32_t enabled_serial = text_enabled_serial;
    const uint64_t generation = text_input_generation;
    const uint64_t batch = ++text_batch;
    target->Retain();
    const bool was_applying = text_applying;
    text_applying = true;
    text_can_commit = serial == text_serial;
    swell_ime_preedit(NULL, 0);
    int cursor = 0, anchor = 0;
    swell_ime_delete_surrounding(before, after);
    if (!target->m_hashaddestroy && text_target == target && text_input_generation == generation &&
        text_enabled_serial == enabled_serial && committed.GetLength())
      swell_ime_commit(committed.Get());
    if (!target->m_hashaddestroy && text_target == target && text_input_generation == generation &&
        text_enabled_serial == enabled_serial)
    {
      if (batch == text_batch) swell_ime_preedit(preedit.Get(), preedit_cursor);
      const char *surrounding = NULL;
      if (swell_ime_get_surrounding(&surrounding, &cursor, &anchor) &&
          (strcmp(text_surrounding.Get(), surrounding) || text_cursor != cursor || text_anchor != anchor))
      {
        text_surrounding.Set(surrounding);
        text_cursor = cursor;
        text_anchor = anchor;
        text_dirty = true;
      }
      swell_im_update_candidates_location();
      text_publish();
    }
    text_applying = was_applying;
    target->Release();
  }
}

static const zwp_text_input_v3_listener text_listener = {text_input_enter_cb, text_input_leave_cb,
    text_input_preedit_string_cb, text_input_commit_string_cb, text_input_delete_surrounding_text_cb,
    text_input_done_cb};

static wl_seat *transfer_seat;

static void transfer_seat_changed()
{
  if (transfer_seat != seat)
  {
    ++text_input_generation;
    if (text_input)
    {
      zwp_text_input_v3_destroy(text_input);
      text_input = NULL;
    }
    if (data_device)
    {
      if (wl_data_device_get_version(data_device) >= 2)
        wl_data_device_release(data_device);
      else
        wl_data_device_destroy(data_device);
      data_device = NULL;
    }
    transfer_seat = seat;
    text_surface = NULL;
    text_enabled = false;
    text_serial = text_enabled_serial = 0;
    text_commit.Set("");
    text_preedit.Set("");
    text_before = text_after = 0;
    swell_ime_preedit(NULL, 0);
  }
  if (seat && data_manager && !data_device)
  {
    data_device = wl_data_device_manager_get_data_device(data_manager, seat);
    wl_data_device_add_listener(data_device, &transfer_device_listener, NULL);
  }
  if (seat && text_manager && !text_input)
  {
    text_input = zwp_text_input_manager_v3_get_text_input(text_manager, seat);
    zwp_text_input_v3_add_listener(text_input, &text_listener, NULL);
  }
}

static void transfer_registry(uint32_t name, const char *interface, uint32_t version)
{
  if (!strcmp(interface, wl_data_device_manager_interface.name) && !data_manager)
    data_manager = (wl_data_device_manager *)wl_registry_bind(
        registry, name, &wl_data_device_manager_interface, wdl_min(version, 3u));
  else if (!strcmp(interface, zwp_text_input_manager_v3_interface.name) && !text_manager)
    text_manager = (zwp_text_input_manager_v3 *)wl_registry_bind(
        registry, name, &zwp_text_input_manager_v3_interface, 1);
  transfer_seat_changed();
}

static void transfer_init()
{
  transfer_seat_changed();
}

static void transfer_dispatch()
{
  if (clipboard_written && data_device && input_serial) CloseClipboard_wayland();
  if (text_target && text_target->m_hashaddestroy) swell_oswindow_ime_focus_wayland(NULL);
  text_sync_focus();
  text_publish();
}

static void transfer_destroy_window(SWELL_WaylandWindow *window)
{
  if (drag_window == window) drag_window = NULL;
  if (drag_destination == window->hwnd) transfer_destination(NULL);
  if (drag_source && (drag_source->hwnd == window->hwnd || button_window == window))
    transfer_finish_source(drag_source, true);
  if (text_surface == window->surface)
  {
    text_surface = NULL;
    text_sync_focus();
  }
  if (text_target && window_from_hwnd(text_target) == window) swell_oswindow_ime_focus_wayland(NULL);
}

#endif
