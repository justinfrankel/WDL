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


#ifndef SWELL_PLATFORM_H
#define SWELL_PLATFORM_H

#define SWELL_PLATFORM_API(X) \
  X(void, SWELL_initargs, (int *argc, char ***argv), (argc, argv)) \
  X(void, SWELL_RunEvents, (), ()) \
  X(void, swell_oswindow_destroy, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_destroy_spare, (SWELL_OSWINDOW window), (window)) \
  X(void, swell_oswindow_update_text, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_focus, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_manage, (HWND hwnd, bool wantfocus), (hwnd, wantfocus)) \
  X(void, swell_oswindow_maximize, (HWND hwnd, bool wantmax), (hwnd, wantmax)) \
  X(void, swell_recalcMinMaxInfo, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_update_style, (HWND hwnd, LONG oldstyle), (hwnd, oldstyle)) \
  X(void, swell_oswindow_update_enable, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_begin_resize, (SWELL_OSWINDOW window), (window)) \
  X(void, swell_oswindow_resize, (SWELL_OSWINDOW window, int flags, RECT rect), (window, flags, rect)) \
  X(void, swell_oswindow_postresize, (HWND hwnd, RECT rect), (hwnd, rect)) \
  X(void, swell_oswindow_invalidate, (HWND hwnd, const RECT *rect), (hwnd, rect)) \
  X(void, UpdateWindow, (HWND hwnd), (hwnd)) \
  X(bool, GetWindowRect, (HWND hwnd, RECT *rect), (hwnd, rect)) \
  X(int, SWELL_SetWindowLevel, (HWND hwnd, int level), (hwnd, level)) \
  X(void, SWELL_GetViewPort, (RECT *rect, const RECT *source, bool work), (rect, source, work)) \
  X(void, GetCursorPos, (POINT *point), (point)) \
  X(WORD, GetAsyncKeyState, (int key), (key)) \
  X(DWORD, GetMessagePos, (), ()) \
  X(HWND, SWELL_CreateXBridgeWindow, (HWND parent, void **window, const RECT *rect), (parent, window, rect)) \
  X(void, SWELL_InitiateDragDrop, (HWND hwnd, RECT *rect, const char *file, void (*callback)(const char *)), (hwnd, rect, file, callback)) \
  X(void, SWELL_InitiateDragDropOfFileList, (HWND hwnd, RECT *rect, const char **files, int count, HICON icon), (hwnd, rect, files, count, icon)) \
  X(void, SWELL_FinishDragDrop, (), ()) \
  X(void, SWELL_SetCursor, (HCURSOR cursor), (cursor)) \
  X(HCURSOR, SWELL_GetCursor, (), ()) \
  X(HCURSOR, SWELL_GetLastSetCursor, (), ()) \
  X(bool, SWELL_IsCursorVisible, (), ()) \
  X(int, SWELL_ShowCursor, (BOOL show), (show)) \
  X(BOOL, SWELL_SetCursorPos, (int x, int y), (x, y)) \
  X(HCURSOR, SWELL_LoadCursorFromFile, (const char *file), (file)) \
  X(HCURSOR, SWELL_LoadCursor, (const char *id), (id)) \
  X(void, SWELL_Register_Cursor_Resource, (const char *id, const char *name, int x, int y), (id, name, x, y)) \
  X(int, SWELL_KeyToASCII, (int key, int flags, int *newflags), (key, flags, newflags)) \
  X(int, swell_is_app_inactive, (), ()) \
  X(void *, SWELL_GetOSWindow, (HWND hwnd, const char *type), (hwnd, type)) \
  X(void *, SWELL_GetOSEvent, (const char *type), (type)) \
  X(bool, OpenClipboard, (HWND hwnd), (hwnd)) \
  X(void, CloseClipboard, (), ()) \
  X(UINT, EnumClipboardFormats, (UINT format), (format)) \
  X(HANDLE, GetClipboardData, (UINT format), (format)) \
  X(void, EmptyClipboard, (), ()) \
  X(void, SetClipboardData, (UINT format, HANDLE data), (format, data)) \
  X(UINT, RegisterClipboardFormat, (const char *name), (name)) \
  X(void, swell_scaling_init, (bool no_auto_hidpi), (no_auto_hidpi)) \
  X(bool, swell_oswindow_set_fullscreen, (HWND hwnd, int mode), (hwnd, mode)) \
  X(void, swell_oswindow_reactivate_app, (), ()) \
  X(void, swell_oswindow_prevent_screensaver, (bool prevent, const char *description), (prevent, description)) \
  X(void, swell_oswindow_ime_focus, (HWND hwnd), (hwnd)) \
  X(void, swell_oswindow_ime_update, (HWND hwnd, HWND root, const RECT *caret, const char *text, int cursor, int anchor), (hwnd, root, caret, text, cursor, anchor))

#define SWELL_PLATFORM_GUI_API(X) \
  X(void, SWELL_SetViewGL, (HWND hwnd, char mode), (hwnd, mode)) \
  X(bool, SWELL_GetViewGL, (HWND hwnd), (hwnd)) \
  X(bool, SWELL_SetGLContextToView, (HWND hwnd), (hwnd)) \
  X(BOOL, EnumDisplayMonitors, (HDC dc, const LPRECT rect, MONITORENUMPROC proc, LPARAM data), (dc, rect, proc, data)) \
  X(BOOL, GetMonitorInfo, (HMONITOR monitor, void *info), (monitor, info))

#ifdef SWELL_LICE_GDI
#define SWELL_PLATFORM_BITMAP_API(X) \
  X(LICE_IBitmap *, swell_oswindow_acquire_bitmap, (LICE_IBitmap *bitmap, int x, int y, int w, int h), (bitmap, x, y, w, h)) \
  X(void, swell_oswindow_release_bitmap, (HWND hwnd, LICE_IBitmap *bitmap, const RECT *dirty), (hwnd, bitmap, dirty))
#else
#define SWELL_PLATFORM_BITMAP_API(X)
#endif

#define SWELL_PLATFORM_DECLARE(ret, name, args, call) ret name args;
SWELL_PLATFORM_API(SWELL_PLATFORM_DECLARE)
SWELL_PLATFORM_GUI_API(SWELL_PLATFORM_DECLARE)
SWELL_PLATFORM_BITMAP_API(SWELL_PLATFORM_DECLARE)
#undef SWELL_PLATFORM_DECLARE
#define SWELL_PLATFORM_DECLARE(ret, name, args, call) ret name##_gdk args;
bool SWELL_load_gtk_gdk();
SWELL_PLATFORM_API(SWELL_PLATFORM_DECLARE)
SWELL_PLATFORM_GUI_API(SWELL_PLATFORM_DECLARE)
SWELL_PLATFORM_BITMAP_API(SWELL_PLATFORM_DECLARE)
#undef SWELL_PLATFORM_DECLARE

struct SWELL_Platform
{
#define SWELL_PLATFORM_MEMBER(ret, name, args, call) ret (*name) args;
  SWELL_PLATFORM_API(SWELL_PLATFORM_MEMBER)
  SWELL_PLATFORM_GUI_API(SWELL_PLATFORM_MEMBER)
  SWELL_PLATFORM_BITMAP_API(SWELL_PLATFORM_MEMBER)
#undef SWELL_PLATFORM_MEMBER
};

bool SWELL_load_gtk();
void swell_im_update_candidates_location();
HWND swell_ime_get_target();
void swell_ime_commit(const char *text);
void swell_ime_preedit(const char *text, int cursor);
bool swell_ime_delete_surrounding_chars(int offset, int count);
bool swell_ime_get_surrounding(const char **text, int *cursor, int *anchor);

#endif
