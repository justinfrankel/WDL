#ifdef SWELL_SUPPORT_IM
#include "swell-im.h"

//-----------------------------------------------------------------------------
// Structure Definitions
//-----------------------------------------------------------------------------
struct __SWELL_editControlState
{
  __SWELL_editControlState() : cache_linelen_bytes(8192)
  {
    cursor_timer = 0;
    cursor_state = 0;
    sel1 = sel2 = -1;
    cursor_pos = 0;
    scroll_x = scroll_y = 0;
    max_height = 0;
    max_width = 0;
    cache_linelen_strlen = cache_linelen_w = 0;
    m_disable_contextmenu = false;
  }
  ~__SWELL_editControlState()
  {
  }

  int cursor_pos, sel1, sel2; // in character pos (*not* bytepos)
  int cursor_state;
  int cursor_timer;
  int scroll_x, scroll_y;
  int max_height; // only used in multiline
  int max_width;

  // used for caching line lengths for multiline word-wrapping edit controls
  int cache_linelen_w, cache_linelen_strlen;
  WDL_TypedBuf<int> cache_linelen_bytes;

  bool m_disable_contextmenu;

  bool deleteSelection(WDL_FastString *fs);
  int getSelection(WDL_FastString *fs, const char **ptrOut) const;
  void moveCursor(int cp); // extends selection if shift is held, otherwise clears
  void onMouseDown(int &capmode_state, int last_cursor);
  void onMouseDrag(int &capmode_state, int p);

  void autoScrollToOffset(HWND hwnd, int charpos, bool is_multiline, bool word_wrap);
};

struct __SWELL_ComboBoxInternalState_rec 
{ 
  __SWELL_ComboBoxInternalState_rec(const char *_desc=NULL, LPARAM _parm=0) { desc=_desc?strdup(_desc):NULL; parm=_parm; } 
  ~__SWELL_ComboBoxInternalState_rec() { free(desc); } 
  char *desc; 
  LPARAM parm; 
  static int cmpfunc(const __SWELL_ComboBoxInternalState_rec *a, const __SWELL_ComboBoxInternalState_rec *b) { return strcmp(a->desc, b->desc); }
};
class __SWELL_ComboBoxInternalState
{
  public:
    __SWELL_ComboBoxInternalState() { selidx=-1; }
    ~__SWELL_ComboBoxInternalState() { }

    int selidx;
    WDL_PtrList_DeleteOnDestroy<__SWELL_ComboBoxInternalState_rec> items;
    __SWELL_editControlState editstate;
};

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------
bool im_hwnd_match(HWND hwnd, const char* classname)
{
  if (hwnd && hwnd->m_classname && strcmp(hwnd->m_classname, classname) == 0)
  {
    // 特殊处理 combobox 类型
    if (strcmp(classname, "combobox") == 0)
    {
      if ((hwnd->m_style & CBS_DROPDOWNLIST) != CBS_DROPDOWNLIST)  
        return 1;
    }
    else if (!GetProp(hwnd, "IS_KEYFINDER"))
    {
      return 1;
    }
  }
  return 0;
}

static int utf8fs_charpos_to_bytepos(const WDL_FastString *fs, int charpos)
{
  return charpos < fs->GetLength() ? WDL_utf8_charpos_to_bytepos(fs->Get(),charpos) : fs->GetLength();
}

__SWELL_editControlState *swell_es_from_hwnd(HWND hwnd) // Get edit cursor
{
  if (!hwnd)
    return nullptr;

  if (!im_hwnd_match(hwnd, "Edit") && !im_hwnd_match(hwnd, "combobox"))
    return nullptr;

  if (hwnd->m_hashaddestroy)
    return nullptr;

  return (__SWELL_editControlState *)hwnd->m_private_data;
}

//-----------------------------------------------------------------------------
// Core Drawing Functions
//-----------------------------------------------------------------------------
static int editControlPaintLine(HDC hdc, const char *str, int str_len, int cursor_pos, int sel1, int sel2, const RECT *r, int dtflags)
{
  // cursor_pos, sel1, sel2 are all byte positions
  int rv = 0;
  if (str_len>0)
  {
    RECT outr = *r;
    if (sel2 < str_len || sel1 > 0)
    {
      RECT tmp={0,};
      DrawText(hdc,str,str_len,&tmp,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
      rv = tmp.right;
      DrawText(hdc,str,str_len,&outr,dtflags|DT_SINGLELINE|DT_NOPREFIX);
    }

    const int offs = wdl_max(sel1,0);
    const int endptr = wdl_min(sel2,str_len);
    if (endptr > offs)
    {
      SetBkMode(hdc,OPAQUE);
      SetBkColor(hdc,g_swell_ctheme.edit_bg_sel);
      const int oldc = GetTextColor(hdc);
      SetTextColor(hdc,g_swell_ctheme.edit_text_sel);

      RECT tmp={0,};
      DrawText(hdc,str,offs,&tmp,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
      outr.left += tmp.right;
      DrawText(hdc,str+offs,endptr-offs,&outr,dtflags|DT_SINGLELINE|DT_NOPREFIX);

      SetBkMode(hdc,TRANSPARENT);
      SetTextColor(hdc,oldc);
    }
  }

  if (cursor_pos >= 0 && cursor_pos <= str_len)
  {
    RECT mr={0,};
    if (cursor_pos>0) DrawText(hdc,str,cursor_pos,&mr,DT_CALCRECT|DT_NOPREFIX|DT_SINGLELINE);

    int oc = GetTextColor(hdc);
    SetTextColor(hdc,g_swell_ctheme.edit_cursor);
    mr.left = r->left + mr.right - 1;
    mr.right = mr.left+1;
    mr.top = r->top;
    mr.bottom = r->bottom;
    DrawText(hdc,"|",1,&mr,dtflags|DT_SINGLELINE|DT_NOPREFIX|DT_NOCLIP);
    SetTextColor(hdc,oc);
  }
  return rv;
}

//-----------------------------------------------------------------------------
// Input Method Callback Functions
//-----------------------------------------------------------------------------
static void commit_cb(GtkIMContext *im_context, const gchar *str, gpointer user_data)
{
  HWND hwnd = (HWND)user_data;
  if (!hwnd)
    return;

  __SWELL_editControlState *es = swell_es_from_hwnd(hwnd);
  if (!es) return;

  const char *p = str;
  while (*p) {
    int codepoint = 0;
    int charlen = wdl_utf8_parsechar(p, &codepoint);
    if (charlen < 1)
      break;
    SendMessage(hwnd, WM_CHAR, (WPARAM)codepoint, 1);
    p += charlen;
  }
}

static void preedit_changed_cb(GtkIMContext *im_context, gpointer user_data)
{
  HWND hwnd = (HWND)user_data;

  gchar *preedit_text;
  PangoAttrList *attrs;
  gint cursor_pos;
  gtk_im_context_get_preedit_string(im_context, &preedit_text, &attrs, &cursor_pos);

  if (preedit_text && *preedit_text) // Not empty preedit text
  {
    gchar *text_copy = g_strdup(preedit_text);
    if (text_copy) {
      SetProp(hwnd, "IM_PREEDIT_TEXT", (HANDLE)text_copy);
      InvalidateRect(hwnd, NULL, FALSE);
      SetProp(hwnd, "IM_PREEDIT_STATE", (HANDLE)1);
    }
  }
  else
  {
    SetProp(hwnd, "IM_PREEDIT_TEXT", (HANDLE)NULL);
    SetProp(hwnd, "IM_PREEDIT_STATE", (HANDLE)0);
  }

  g_free(preedit_text);
  if (attrs)
    pango_attr_list_unref(attrs);
}

static void preedit_end_cb(GtkIMContext *im_context, gpointer user_data) // If the preedit is finished
{
  HWND hwnd = (HWND)user_data;
  SetProp(hwnd, "IM_PREEDIT_TEXT", (HANDLE)NULL);
  SetProp(hwnd, "IM_PREEDIT_STATE", (HANDLE)0);
  InvalidateRect(hwnd, NULL, FALSE);
}

// Updates the pre-edit candidate window location
// Calculates the position based on cursor location
void im_update_candidates_location(HWND hwnd)
{
  GtkIMContext *im_context = (GtkIMContext *)GetProp(hwnd, "IM_CONTEXT");
  if (!im_context) return;

  __SWELL_editControlState *es = swell_es_from_hwnd(hwnd);
  if (!es) return;

  RECT hwnd_r;
  GetWindowRect(hwnd, &hwnd_r);

  HWND client_hwnd = (HWND)GetProp(hwnd, "IM_OSWINDOW");
  if (!client_hwnd) return;
  if (client_hwnd->m_refcnt == 0) return; // if hwnd start with REAPER (not close window in last quit)
  RECT client_r;
  GetWindowRect(client_hwnd, &client_r);

  int x = hwnd_r.left - client_r.left;
  int y = hwnd_r.top - client_r.top;
  y -= SWELL_UI_SCALE(30);

  // Start to cal cursor offsets
  int cursor_offset_x{}; // cursor_pos x
  int cursor_offset_y{}; // cursor_pos y
  int cursor_pos{};
  __SWELL_ComboBoxInternalState *s;
  if (im_hwnd_match(hwnd, "combobox")) {
    s = (__SWELL_ComboBoxInternalState*)hwnd->m_private_data;
    cursor_pos = s->editstate.cursor_pos;
    if (s->editstate.sel1 == 0) {
      cursor_pos = 0;
    }
  }
  else{
    cursor_pos = es->cursor_pos;
    if (es->sel1 == 0) {
      cursor_pos = 0;
    }
  }
  if (cursor_pos >= 0) {
    WDL_FastString *title = &hwnd->m_title;
    HDC hdc = GetDC(hwnd);
    if (hdc) {
      const bool multiline = (hwnd->m_style & ES_MULTILINE) != 0;
      RECT tmp = {
          0,
      };
      const int line_h = DrawText(hdc, " ", 1, &tmp,
                                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      const int wwrap = (hwnd->m_style & ES_AUTOHSCROLL)
                            ? 0
                            : hwnd->m_position.right - hwnd->m_position.left -
                                  g_swell_ctheme.scrollbar_width;
      POINT pt = {0, 0};
      int singleline_len = multiline ? -1 : title->GetLength();

      if (im_hwnd_match(hwnd, "combobox")) {
        if (editGetCharPos(hdc, title->Get(), singleline_len, cursor_pos,
                           line_h, &pt, wwrap, &s->editstate, hwnd)) {
          cursor_offset_x = pt.x;
          cursor_offset_y = pt.y;
        }
      } else {
        if (editGetCharPos(hdc, title->Get(), singleline_len, cursor_pos,
                           line_h, &pt, wwrap, es, hwnd)) {
          cursor_offset_x = pt.x;
          cursor_offset_y = pt.y;
        }
      }
      ReleaseDC(hwnd, hdc);
    }
  }
  x += cursor_offset_x;
  y += cursor_offset_y;
  GdkRectangle preedit_area = {x, y, 0, 0};
  gtk_im_context_set_cursor_location(im_context, &preedit_area);
}

//-----------------------------------------------------------------------------
// Initialization and Cleanup Functions
//-----------------------------------------------------------------------------
void im_init(HWND hwnd)
{
  if (im_hwnd_match(hwnd, "Edit") || im_hwnd_match(hwnd, "combobox"))
  {
    HWND temp_hwnd = SWELL_topwindows;
    while (temp_hwnd) {
        if (temp_hwnd->m_oswindow) {
          GtkIMContext *im_context = gtk_im_multicontext_new();
          SetProp(hwnd, "IM_CONTEXT", (HANDLE)im_context);
          gtk_im_context_set_client_window(im_context, temp_hwnd->m_oswindow);
          SetProp(hwnd, "IM_OSWINDOW", temp_hwnd);
          g_signal_connect(im_context, "commit", G_CALLBACK(commit_cb), hwnd);
          g_signal_connect(im_context, "preedit-changed", G_CALLBACK(preedit_changed_cb), hwnd);
          g_signal_connect(im_context, "preedit-end", G_CALLBACK(preedit_end_cb), hwnd);
          break;
        }
        temp_hwnd = temp_hwnd->m_next;
    }
  }
}

void im_free(HWND hwnd) // Free resources related to im_context
{
  GtkIMContext *im_context = (GtkIMContext *)GetProp(hwnd, "IM_CONTEXT");
  if (im_context)
  {
    g_object_unref(im_context);
    RemoveProp(hwnd, "IM_CONTEXT");
    RemoveProp(hwnd, "IM_PREEDIT_TEXT");
    RemoveProp(hwnd, "IM_PREEDIT_STATE");
    RemoveProp(hwnd, "IM_OSWINDOW");
  }
}

//-----------------------------------------------------------------------------
// Helper Functions (copied from swell-wnd-generic.cpp)
//-----------------------------------------------------------------------------
static int editMeasureLineLength(HDC hdc, const char *str, int str_len)
{
  RECT tmp = {0,};
  DrawText(hdc,str,str_len,&tmp,DT_NOPREFIX|DT_SINGLELINE|DT_CALCRECT|DT_RIGHT);
  return tmp.right;
}

static int swell_getLineLength(const char *buf, int *post_skip, int wrap_maxwid, HDC hdc)
{
  int lb=0;
  int ps = 0;
  while (buf[lb] && buf[lb] != '\r' && buf[lb] != '\n') lb++;

  if (wrap_maxwid > g_swell_ctheme.scrollbar_width && hdc && lb>0)
  {
    wrap_maxwid -= g_swell_ctheme.scrollbar_width;
    
    // step through a word at a time and find the most that can fit
    int x=0,best_len=0,sumw=0;
    for (;;)
    {
      while (x < lb && buf[x] > 0 && isspace(buf[x])) x++;
      while (x < lb && (buf[x]<0 || !isspace(buf[x]))) x++;
      const int thisw = editMeasureLineLength(hdc,buf+best_len,x-best_len);
      if (thisw+sumw > wrap_maxwid) break;
      sumw += thisw;
      best_len = x;
      if (x >= lb) break;
    }
    if (best_len == 0) best_len = lb; // if we can't fit anything, just use the whole line
    lb = best_len;
  }

  if (post_skip) *post_skip = ps;
  if (buf[lb] == '\r' && buf[lb+1] == '\n') ps = 2;
  else if (buf[lb] == '\r' || buf[lb] == '\n') ps = 1;
  if (post_skip) *post_skip = ps;

  return lb;
}

static bool editGetCharPos(HDC hdc, const char *str, int singleline_len, int charpos, int line_h, POINT *pt, int word_wrap,
    __SWELL_editControlState *es, HWND hwnd)
{
  int bytepos = WDL_utf8_charpos_to_bytepos(str,charpos);
  int ypos = 0;
  if (singleline_len >= 0)
  {
    if (bytepos > singleline_len) return false;
    pt->y=0;
    pt->x=editMeasureLineLength(hdc,str,bytepos);
    return true;
  }

  while (*str)
  {
    int pskip = 0, lb;
    lb = swell_getLineLength(str,&pskip,word_wrap,hdc);
    if (bytepos < lb+pskip)
    { 
      pt->x=editMeasureLineLength(hdc,str,bytepos);
      pt->y=ypos;
      return true;
    }
    str += lb+pskip;
    bytepos -= lb+pskip;
    if (*str || (pskip>0 && str[-1] == '\n')) ypos += line_h;
  }
  pt->x=0;
  pt->y=ypos;
  return true;
}

//-----------------------------------------------------------------------------
// Paint when preedit text typing
//-----------------------------------------------------------------------------
ImPreeditPaintResult im_preedit_paint(__SWELL_editControlState *es, HWND hwnd, int sel1, int sel2, int cursor_pos)
{
  ImPreeditPaintResult result;
  const int ori_sel1 = sel1;
  const int ori_sel2 = sel2; // For recovery sel1 & sel2
  WDL_FastString *title = &hwnd->m_title;
  WDL_FastString saved_title = hwnd->m_title; // For recovery title above

  WDL_FastString temp_str;
  const char *original_text = title->Get(); // original text in edit field
  int real_cursor_pos{}; // for es autoscroll, NOT byte, IS utf-8 char len

  bool preedit_state = (intptr_t)GetProp(hwnd, "IM_PREEDIT_STATE") ? true : false;
  if (preedit_state) {
    const char *preedit_text = (const char *)GetProp(hwnd, "IM_PREEDIT_TEXT");
    int preedit_byte_len = preedit_text ? strlen(preedit_text) : 0; // selection use byte NOT str len
    int preedit_len = preedit_text ? WDL_utf8_get_charlen(preedit_text) : 0; // selection use byte NOT str len
    int scroll_pos{};
    bool is_multiline = (hwnd->m_style & ES_MULTILINE) != 0;
    bool is_word_wrap =
        (hwnd->m_style & (ES_MULTILINE | ES_AUTOHSCROLL)) == ES_MULTILINE;
    if (ori_sel1 == ori_sel2) { // no selection
      temp_str.SetLen(cursor_pos, false);
      temp_str.Set(original_text, cursor_pos);           // before cursor
      temp_str.Append(preedit_text ? preedit_text : ""); // insert preedit text
      temp_str.Append(original_text + cursor_pos);       // after cursor
      sel1 = cursor_pos;
      sel2 = cursor_pos + preedit_byte_len; 
      // cursor_pos is from swell-wnd-generic.cpp
      // cursor_pos IS byte
      // es->cursor_pos IS utf-8 len
      // scroll NEEDS utf-8 len
      real_cursor_pos = cursor_pos + preedit_byte_len;
      scroll_pos = es->cursor_pos + preedit_len;
    } else if (ori_sel2 > ori_sel1) { // has selection
      if (ori_sel1 > 0) {
        temp_str.SetLen(ori_sel1, false);
        temp_str.Set(original_text, ori_sel1); // before sel1
      }
      temp_str.Append(preedit_text ? preedit_text : ""); // insert preedit text
      temp_str.Append(original_text + ori_sel2);         // after sel2
      sel1 = ori_sel1;
      sel2 = ori_sel1 + preedit_byte_len;
      real_cursor_pos = ori_sel1 + preedit_byte_len;
      scroll_pos = es->sel1 + preedit_len;
    }
    title = &temp_str;
    hwnd->m_title = &temp_str;
    es->autoScrollToOffset(hwnd, scroll_pos, is_multiline, is_word_wrap);
    hwnd->m_title = &saved_title;
  } else { // If not preedit
    sel1 = ori_sel1;
    sel2 = ori_sel2;
    real_cursor_pos = cursor_pos;
    title = &saved_title;
  }
  return {sel1, sel2, real_cursor_pos, title};
}
#endif
