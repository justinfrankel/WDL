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


#ifndef SWELL_PROVIDED_BY_APP

#include "swell.h"

#ifndef SWELL_TARGET_GDK

#include "swell-internal.h"
#include "swell-platform.h"
#include "swell-dlggen.h"
#include "../wdlcstring.h"

void swell_oswindow_destroy_spare_gdk(SWELL_OSWINDOW) { }

void swell_oswindow_destroy_gdk(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow)
  {
    if (SWELL_focused_oswindow == hwnd->m_oswindow) SWELL_focused_oswindow = NULL;
    hwnd->m_oswindow=NULL;
#ifdef SWELL_LICE_GDI
    delete hwnd->m_backingstore;
    hwnd->m_backingstore=0;
#endif
  }
}
void swell_oswindow_update_text_gdk(HWND hwnd)
{
  if (hwnd) printf("SWELL: swt '%s'\n",hwnd->m_title.Get());

}

void swell_oswindow_maximize_gdk(HWND, bool wantmax)
{
}

void swell_oswindow_focus_gdk(HWND hwnd)
{
  if (!hwnd)
  {
    SWELL_focused_oswindow = NULL;
  }
  while (hwnd && !hwnd->m_oswindow) hwnd=hwnd->m_parent;
  if (hwnd && hwnd->m_oswindow != SWELL_focused_oswindow)
  {
    SWELL_focused_oswindow = hwnd->m_oswindow;
  }
}

void swell_recalcMinMaxInfo_gdk(HWND hwnd)
{
}


void SWELL_initargs_gdk(int *argc, char ***argv)
{
}

#ifdef SWELL_LICE_GDI
LICE_IBitmap *swell_oswindow_acquire_bitmap_gdk(LICE_IBitmap *bitmap, int x, int y, int w, int h)
{
  return new LICE_SubBitmap(bitmap, x, y, w, h);
}

void swell_oswindow_release_bitmap_gdk(HWND hwnd, LICE_IBitmap *bitmap, const RECT *dirty)
{
  delete bitmap;
}
#endif

void swell_oswindow_manage_gdk(HWND hwnd, bool wantfocus)
{
  if (!hwnd) return;

  bool isVis = !!hwnd->m_oswindow;
  bool wantVis = !hwnd->m_parent && hwnd->m_visible;

  if (isVis != wantVis)
  {
    if (!wantVis) swell_oswindow_destroy_gdk(hwnd);
    else 
    {
       // generic implementation
      hwnd->m_oswindow = hwnd;
      if (wantfocus) swell_oswindow_focus_gdk(hwnd);
    }
  }
  if (wantVis) swell_oswindow_update_text_gdk(hwnd);
}



void SWELL_RunEvents_gdk()
{
}


void swell_oswindow_update_style_gdk(HWND hwnd, LONG oldstyle)
{
}

void swell_oswindow_update_enable_gdk(HWND hwnd)
{
}

int SWELL_SetWindowLevel_gdk(HWND hwnd, int newlevel)
{
  int rv=0;
  if (hwnd)
  {
    rv = hwnd->m_israised ? 1 : 0;
    hwnd->m_israised = newlevel>0;
  }
  return rv;
}

void SWELL_GetViewPort_gdk(RECT *r, const RECT *sourcerect, bool wantWork)
{
  r->left=r->top=0;
  r->right=1024;
  r->bottom=768;
}


bool GetWindowRect_gdk(HWND hwnd, RECT *r)
{
  if (!hwnd) return false;
  if (hwnd->m_oswindow)
  {
    *r = hwnd->m_position;
    return true;
  }

  r->left=r->top=0; 
  hwnd->Retain();
  ClientToScreen(hwnd,(LPPOINT)r);
  r->right = r->left + hwnd->m_position.right - hwnd->m_position.left;
  r->bottom = r->top + hwnd->m_position.bottom - hwnd->m_position.top;
  const bool destroyed = hwnd->m_hashaddestroy != 0;
  hwnd->Release();
  return !destroyed;
}

void swell_oswindow_begin_resize_gdk(SWELL_OSWINDOW wnd) { }
void swell_oswindow_resize_gdk(SWELL_OSWINDOW wnd, int reposflag, RECT f) { }
void swell_oswindow_postresize_gdk(HWND hwnd, RECT f) { }
void swell_oswindow_invalidate_gdk(HWND hwnd, const RECT *r) { }

void UpdateWindow_gdk(HWND hwnd) { }

static WDL_IntKeyedArray<HANDLE> m_clip_recs(GlobalFree);
static WDL_PtrList<char> m_clip_curfmts;

bool OpenClipboard_gdk(HWND hwndDlg)
{ 
  RegisterClipboardFormat_gdk(NULL);
  return true; 
}

void CloseClipboard_gdk() { }

UINT EnumClipboardFormats_gdk(UINT lastfmt)
{
  int x=0;
  for (;;)
  {
    int fmt=0;
    if (!m_clip_recs.Enumerate(x++,&fmt)) return 0;
    if (lastfmt == 0) return fmt;

    if ((UINT)fmt == lastfmt) return m_clip_recs.Enumerate(x++,&fmt) ? fmt : 0;
  }
}

HANDLE GetClipboardData_gdk(UINT type)
{
  return m_clip_recs.Get(type);
}


void EmptyClipboard_gdk()
{
  m_clip_recs.DeleteAll();
}

void SetClipboardData_gdk(UINT type, HANDLE h)
{
  if (h) m_clip_recs.Insert(type,h);
  else m_clip_recs.Delete(type);
}

UINT RegisterClipboardFormat_gdk(const char *desc)
{
  if (!m_clip_curfmts.GetSize())
  {
    m_clip_curfmts.Add(strdup("SWELL__CF_TEXT"));
    m_clip_curfmts.Add(strdup("SWELL__CF_HDROP"));
  }
  if (!desc || !*desc) return 0;
  int x;
  const int n = m_clip_curfmts.GetSize();
  for(x=0;x<n;x++) 
    if (!strcmp(m_clip_curfmts.Get(x),desc)) return x + 1;
  m_clip_curfmts.Add(strdup(desc));
  return n+1;
}


void GetCursorPos_gdk(POINT *pt)
{
  pt->x=0;
  pt->y=0;
}


WORD GetAsyncKeyState_gdk(int key)
{
  return 0;
}


DWORD GetMessagePos_gdk()
{  
  return 0;
}

HWND SWELL_CreateXBridgeWindow_gdk(HWND viewpar, void **wref, const RECT *r)
{
  *wref = NULL;
  return NULL;
}



void SWELL_InitiateDragDrop_gdk(HWND hwnd, RECT* srcrect, const char* srcfn, void (*callback)(const char* dropfn))
{
}

// owner owns srclist, make copies here etc
void SWELL_InitiateDragDropOfFileList_gdk(HWND hwnd, RECT *srcrect, const char **srclist, int srccount, HICON icon)
{
}

void SWELL_FinishDragDrop_gdk() { }

static HCURSOR m_last_setcursor;

void SWELL_SetCursor_gdk(HCURSOR curs)
{
  m_last_setcursor=curs;
}

HCURSOR SWELL_GetCursor_gdk()
{
  return m_last_setcursor;
}
HCURSOR SWELL_GetLastSetCursor_gdk()
{
  return m_last_setcursor;
}

static int m_curvis_cnt;
bool SWELL_IsCursorVisible_gdk()
{
  return m_curvis_cnt>=0;
}

int SWELL_ShowCursor_gdk(BOOL bShow)
{
  m_curvis_cnt += (bShow?1:-1);
  return m_curvis_cnt;
}

BOOL SWELL_SetCursorPos_gdk(int X, int Y)
{  
  return false;
}

HCURSOR SWELL_LoadCursorFromFile_gdk(const char *fn)
{
  return NULL;
}


static SWELL_CursorResourceIndex *SWELL_curmodule_cursorresource_head;

HCURSOR SWELL_LoadCursor_gdk(const char *_idx)
{
  return NULL;
}



void SWELL_Register_Cursor_Resource_gdk(const char *idx, const char *name, int hotspot_x, int hotspot_y)
{
  SWELL_CursorResourceIndex *ri = (SWELL_CursorResourceIndex*)malloc(sizeof(SWELL_CursorResourceIndex));
  ri->hotspot.x = hotspot_x;
  ri->hotspot.y = hotspot_y;
  ri->resname=name;
  ri->cachedCursor=0;
  ri->resid = idx;
  ri->_next = SWELL_curmodule_cursorresource_head;
  SWELL_curmodule_cursorresource_head = ri;
}

int SWELL_KeyToASCII_gdk(int wParam, int lParam, int *newflags)
{
  return 0;
}

int swell_is_app_inactive_gdk() { return 0; }

void *SWELL_GetOSWindow_gdk(HWND hwnd, const char *type) { return NULL; }
void *SWELL_GetOSEvent_gdk(const char *type) { return NULL; }

void SWELL_SetViewGL_gdk(HWND, char) { }
bool SWELL_GetViewGL_gdk(HWND) { return false; }
bool SWELL_SetGLContextToView_gdk(HWND) { return false; }
BOOL EnumDisplayMonitors_gdk(HDC, const LPRECT, MONITORENUMPROC, LPARAM) { return FALSE; }
BOOL GetMonitorInfo_gdk(HMONITOR, void *) { return FALSE; }
void swell_scaling_init_gdk(bool) { }
bool swell_oswindow_set_fullscreen_gdk(HWND, int) { return false; }
void swell_oswindow_reactivate_app_gdk() { }
void swell_oswindow_prevent_screensaver_gdk(bool, const char *) { }
void swell_oswindow_ime_focus_gdk(HWND) { }
void swell_oswindow_ime_update_gdk(HWND, HWND, const RECT *, const char *, int, int) { }

#endif
#endif
