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
#include "swell-internal.h"

#if defined(SWELL_TARGET_GDK) && defined(SWELL_ACCESSKIT)

#include "swell-accesskit-shim.h"

#include "../mutex.h"
#include "../wdlutf8.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <string>
#include <vector>

struct SWELL_AccessKitOwnedNode
{
  HWND hwnd;
  swell_accesskit_node pod;
  std::string label_storage;
  std::string value_storage;
  std::string access_key_storage;
  std::string keyboard_shortcut_storage;
  std::vector<uint64_t> children_storage;
  std::vector<uint64_t> labelled_by_storage;
  std::vector<uint8_t> character_lengths_storage;
  std::vector<float> character_positions_storage;
  std::vector<float> character_widths_storage;
};

struct SWELL_AccessKitOwnedSnapshot
{
  uint64_t root_id;
  uint64_t focus_id;
  std::vector<SWELL_AccessKitOwnedNode> nodes;
  std::vector<swell_accesskit_node> exported_nodes;
  swell_accesskit_tree_snapshot snapshot;
};

struct SWELL_AccessKitWindowState
{
  HWND hwnd;
  swell_accesskit_host *host;
  bool dirty;
  SWELL_AccessKitWindowState *next;
};

static WDL_Mutex g_accesskit_mutex;
static SWELL_AccessKitWindowState *g_accesskit_windows;
static bool g_accesskit_debug = false;

static bool swell_accesskit_contains_hwnd(HWND parent, HWND target);
static int swell_accesskit_count_accessible_menu_items(HMENU menu);

static bool swell_accesskit_is_live_toplevel_hwnd(HWND root)
{
  if (!root) return false;
  HWND hwnd = SWELL_topwindows;
  while (hwnd)
  {
    if (hwnd == root) return !hwnd->m_parent && !hwnd->m_hashaddestroy;
    hwnd = hwnd->m_next;
  }
  return false;
}

static HWND swell_accesskit_get_root(HWND hwnd)
{
  while (hwnd && hwnd->m_parent) hwnd = hwnd->m_parent;
  return hwnd;
}

static uint64_t swell_accesskit_node_id_for_hwnd(HWND hwnd)
{
  return (uint64_t)(uintptr_t)hwnd;
}

static bool swell_accesskit_hwnd_has_text_run(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "Edit");
}

static bool swell_accesskit_hwnd_is_combo(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "combobox");
}

static bool swell_accesskit_combo_is_editable(HWND hwnd)
{
  return swell_accesskit_hwnd_is_combo(hwnd) && (hwnd->m_style & CBS_DROPDOWNLIST) != CBS_DROPDOWNLIST;
}

static bool swell_accesskit_hwnd_has_combo_text_run(HWND hwnd)
{
  return swell_accesskit_combo_is_editable(hwnd);
}

static bool swell_accesskit_hwnd_is_listview(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "SysListView32");
}

static bool swell_accesskit_hwnd_is_treeview(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "SysTreeView32");
}

static bool swell_accesskit_hwnd_is_tab(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "SysTabControl32");
}

static uint64_t swell_accesskit_text_run_id_for_hwnd(HWND hwnd)
{
  return ((uint64_t)(uintptr_t)hwnd << 1) | 1u;
}

static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_MENU_BAR = 0x1000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_MENU_BAR_ITEM = 0x2000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_POPUP_MENU = 0x3000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_POPUP_ITEM = 0x4000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_COMBO_TEXT_RUN = 0x5000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_LIST_ITEM = 0x6000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_GRID_ROW = 0x7000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_GRID_CELL = 0x8000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_COLUMN_HEADER = 0x9000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_TREE_ITEM = 0xa000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_TAB = 0xb000000000000000ull;
static const uint64_t SWELL_ACCESSKIT_SYNTHETIC_COMBO_OPTION = 0xc000000000000000ull;

static uint64_t swell_accesskit_pointer_bits(const void *ptr)
{
  return ((uint64_t)(uintptr_t)ptr) & 0x0000ffffffffffffull;
}

static uint64_t swell_accesskit_menu_bar_id_for_hwnd(HWND hwnd)
{
  return SWELL_ACCESSKIT_SYNTHETIC_MENU_BAR | swell_accesskit_pointer_bits(hwnd);
}

static uint64_t swell_accesskit_menu_bar_item_id_for_hwnd(HWND hwnd, int index)
{
  return SWELL_ACCESSKIT_SYNTHETIC_MENU_BAR_ITEM | ((swell_accesskit_pointer_bits(hwnd) & 0x000000ffffffffffull) << 8) | (uint64_t)(index & 0xff);
}

static uint64_t swell_accesskit_popup_menu_id_for_hwnd(HWND hwnd)
{
  const uint64_t serial = swell_accesskit_get_active_menu_serial(hwnd);
  return SWELL_ACCESSKIT_SYNTHETIC_POPUP_MENU | (serial ? (serial & 0x0000ffffffffffffull) : swell_accesskit_pointer_bits(hwnd));
}

static uint64_t swell_accesskit_popup_item_id_for_hwnd(HWND hwnd, int index)
{
  const uint64_t serial = swell_accesskit_get_active_menu_serial(hwnd);
  const uint64_t instance = serial ? serial : swell_accesskit_pointer_bits(hwnd);
  return SWELL_ACCESSKIT_SYNTHETIC_POPUP_ITEM | ((instance & 0x00000fffffffffffull) << 12) | (uint64_t)(index & 0xfff);
}

static uint64_t swell_accesskit_combo_text_run_id_for_hwnd(HWND hwnd)
{
  return SWELL_ACCESSKIT_SYNTHETIC_COMBO_TEXT_RUN | swell_accesskit_pointer_bits(hwnd);
}

static uint64_t swell_accesskit_indexed_id(HWND hwnd, uint64_t ns, int index)
{
  return ns | ((swell_accesskit_pointer_bits(hwnd) & 0x000000ffffffffull) << 12) | (uint64_t)(index & 0xfff);
}

static uint64_t swell_accesskit_list_item_id_for_hwnd(HWND hwnd, int index, uintptr_t identity)
{
  if (identity && identity != (uintptr_t)index)
    return SWELL_ACCESSKIT_SYNTHETIC_LIST_ITEM | (identity & 0x0fffffffffffffffull);
  return swell_accesskit_indexed_id(hwnd, SWELL_ACCESSKIT_SYNTHETIC_LIST_ITEM, index);
}

static uint64_t swell_accesskit_grid_row_id_for_hwnd(HWND hwnd, int index)
{
  return swell_accesskit_indexed_id(hwnd, SWELL_ACCESSKIT_SYNTHETIC_GRID_ROW, index);
}

static uint64_t swell_accesskit_grid_cell_id_for_hwnd(HWND hwnd, int row, int col)
{
  return SWELL_ACCESSKIT_SYNTHETIC_GRID_CELL |
      ((swell_accesskit_pointer_bits(hwnd) & 0x000000ffffffull) << 20) |
      ((uint64_t)(row & 0xfff) << 8) | (uint64_t)(col & 0xff);
}

static uint64_t swell_accesskit_column_header_id_for_hwnd(HWND hwnd, int col)
{
  return swell_accesskit_indexed_id(hwnd, SWELL_ACCESSKIT_SYNTHETIC_COLUMN_HEADER, col);
}

static uint64_t swell_accesskit_tree_item_id(HTREEITEM item)
{
  return SWELL_ACCESSKIT_SYNTHETIC_TREE_ITEM | (swell_accesskit_pointer_bits(item) & 0x0fffffffffffffffull);
}

static uint64_t swell_accesskit_tab_id_for_hwnd(HWND hwnd, int index)
{
  return swell_accesskit_indexed_id(hwnd, SWELL_ACCESSKIT_SYNTHETIC_TAB, index);
}

static uint64_t swell_accesskit_combo_option_id_for_hwnd(HWND hwnd, int index)
{
  return swell_accesskit_indexed_id(hwnd, SWELL_ACCESSKIT_SYNTHETIC_COMBO_OPTION, index + 1);
}

static bool swell_accesskit_get_combo_current_option(HWND hwnd, std::string *text, int *selected_index, int *item_count)
{
  if (text) text->clear();
  if (selected_index) *selected_index = -1;
  if (item_count) *item_count = 0;
  if (!swell_accesskit_hwnd_is_combo(hwnd)) return false;

  const int count = (int)SendMessage(hwnd, CB_GETCOUNT, 0, 0);
  if (item_count) *item_count = count > 0 ? count : 0;

  const int sel = (int)SendMessage(hwnd, CB_GETCURSEL, 0, 0);
  if (sel >= 0 && count > 0 && sel < count)
  {
    if (selected_index) *selected_index = sel;
    const int len = (int)SendMessage(hwnd, CB_GETLBTEXTLEN, (WPARAM)sel, 0);
    if (len >= 0 && text)
    {
      std::vector<char> buf((size_t)len + 1);
      if (SendMessage(hwnd, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf.data()) != CB_ERR)
        text->assign(buf.data());
    }
  }

  if (text && text->empty() && hwnd->m_title.Get() && hwnd->m_title.Get()[0])
    text->assign(hwnd->m_title.Get());

  return text ? !text->empty() : (sel >= 0 && count > 0 && sel < count);
}

static bool swell_accesskit_hwnd_has_collapsed_combo_option(HWND hwnd)
{
  std::string text;
  return swell_accesskit_hwnd_is_combo(hwnd) &&
      swell_accesskit_get_active_menu_owner() != hwnd &&
      swell_accesskit_get_combo_current_option(hwnd, &text, NULL, NULL);
}

static void swell_accesskit_tree_id_for_hwnd(HWND hwnd, uint8_t tree_id[16])
{
  memset(tree_id, 0, 16);
  memcpy(tree_id, "swelllnx", 8);
  const uint64_t value = (uint64_t)(uintptr_t)hwnd;
  int i;
  for (i = 0; i < 8; ++i)
    tree_id[8 + i] = (uint8_t)((value >> ((7 - i) * 8)) & 0xff);
}

static SWELL_AccessKitWindowState *swell_accesskit_find_window_state_locked(HWND root)
{
  SWELL_AccessKitWindowState *state = g_accesskit_windows;
  while (state)
  {
    if (state->hwnd == root) return state;
    state = state->next;
  }
  return NULL;
}

static bool swell_accesskit_window_exists_locked(SWELL_AccessKitWindowState *needle)
{
  SWELL_AccessKitWindowState *state = g_accesskit_windows;
  while (state)
  {
    if (state == needle) return true;
    state = state->next;
  }
  return false;
}

static bool swell_accesskit_is_window_focused(HWND root)
{
  if (!swell_accesskit_is_live_toplevel_hwnd(root) || !root->m_oswindow) return false;
  if (SWELL_focused_oswindow == root->m_oswindow && swell_is_app_inactive() <= 0) return true;

  HWND focused = SWELL_GetFocusedChild(root);
  if (!focused) focused = GetFocus();
  return focused && swell_accesskit_contains_hwnd(root, focused) && swell_is_app_inactive() <= 0;
}

static swell_accesskit_rect swell_accesskit_rect_from_rect(const RECT *r)
{
  swell_accesskit_rect rect;
  rect.x0 = (double)r->left;
  rect.y0 = (double)r->top;
  rect.x1 = (double)r->right;
  rect.y1 = (double)r->bottom;
  return rect;
}

static void swell_accesskit_update_root_bounds(SWELL_AccessKitWindowState *state)
{
  if (!state || !state->host || !state->hwnd || !state->hwnd->m_oswindow) return;

  RECT outer = {0,};
  RECT inner = {0,};
  GetWindowRect(state->hwnd, &outer);
  GetClientRect(state->hwnd, &inner);
  ClientToScreen(state->hwnd, (POINT *)&inner);
  ClientToScreen(state->hwnd, ((POINT *)&inner) + 1);

  const swell_accesskit_rect outer_rect = swell_accesskit_rect_from_rect(&outer);
  const swell_accesskit_rect inner_rect = swell_accesskit_rect_from_rect(&inner);
  swell_accesskit_host_set_root_window_bounds(state->host, &outer_rect, &inner_rect);
}

static int swell_accesskit_count_nodes(HWND hwnd)
{
  if (!hwnd || hwnd->m_hashaddestroy || !hwnd->m_visible) return 0;

  int count = 1;
  if (swell_accesskit_hwnd_is_listview(hwnd))
  {
    swell_accesskit_listview_info info;
    swell_accesskit_collection_range range;
    if (swell_accesskit_get_listview_info(hwnd,&info) && swell_accesskit_get_listview_export_range(hwnd,&range))
    {
      if (info.is_listbox || !info.is_report)
        count += range.count;
      else
        count += info.column_count + range.count + range.count * info.column_count;
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(hwnd))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(hwnd,&info)) count += info.visible_count;
  }
  else if (swell_accesskit_hwnd_is_tab(hwnd))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(hwnd,&info)) count += info.count;
  }
  if (swell_accesskit_hwnd_has_text_run(hwnd)) ++count;
  if (swell_accesskit_hwnd_has_combo_text_run(hwnd)) ++count;
  if (swell_accesskit_hwnd_has_collapsed_combo_option(hwnd)) ++count;
  if (!hwnd->m_parent && hwnd->m_menu)
    count += 1 + swell_accesskit_count_accessible_menu_items(hwnd->m_menu);
  HWND child = hwnd->m_children;
  while (child)
  {
    count += swell_accesskit_count_nodes(child);
    child = child->m_next;
  }
  if (!hwnd->m_parent)
  {
    const int active_menu_count = swell_accesskit_get_active_menu_count();
    HWND menu_owner = swell_accesskit_get_active_menu_owner();
    if (menu_owner && swell_accesskit_contains_hwnd(hwnd, menu_owner))
    {
      for (int i = 0; i < active_menu_count; ++i)
      {
        HMENU menu = swell_accesskit_get_active_menu(i);
        count += 1 + swell_accesskit_count_accessible_menu_items(menu);
      }
    }
  }
  return count;
}

static uint32_t swell_accesskit_role_for_hwnd(HWND hwnd)
{
  if (!hwnd) return SWELL_ACCESSKIT_ROLE_UNKNOWN;
  if (!hwnd->m_parent) return SWELL_ACCESSKIT_ROLE_WINDOW;

  const char *classname = hwnd->m_classname ? hwnd->m_classname : "";
  if (!strcmp(classname, "Static")) return SWELL_ACCESSKIT_ROLE_LABEL;
  if (!strcmp(classname, "Edit"))
    return (hwnd->m_style & ES_MULTILINE) ? SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT : SWELL_ACCESSKIT_ROLE_TEXT_INPUT;
  if (!strcmp(classname, "msctls_trackbar32") || !strcmp(classname, "REAPERhfader"))
    return SWELL_ACCESSKIT_ROLE_SLIDER;
  if (!strcmp(classname, "msctls_progress32")) return SWELL_ACCESSKIT_ROLE_PROGRESS_INDICATOR;
  if (!strcmp(classname, "combobox"))
    return swell_accesskit_combo_is_editable(hwnd) ? SWELL_ACCESSKIT_ROLE_EDITABLE_COMBO_BOX : SWELL_ACCESSKIT_ROLE_COMBO_BOX;
  if (!strcmp(classname, "SysListView32"))
  {
    swell_accesskit_listview_info info;
    if (swell_accesskit_get_listview_info(hwnd,&info))
    {
      if (info.is_listbox) return SWELL_ACCESSKIT_ROLE_LIST_BOX;
      return info.is_report ? SWELL_ACCESSKIT_ROLE_GRID : SWELL_ACCESSKIT_ROLE_LIST;
    }
  }
  if (!strcmp(classname, "SysTreeView32")) return SWELL_ACCESSKIT_ROLE_TREE;
  if (!strcmp(classname, "SysTabControl32")) return SWELL_ACCESSKIT_ROLE_TAB_LIST;
  if (!strcmp(classname, "Button"))
  {
    if (hwnd->m_style & BS_GROUPBOX) return SWELL_ACCESSKIT_ROLE_GROUP;
    switch (hwnd->m_style & 0xf)
    {
      case BS_AUTOCHECKBOX:
      case BS_AUTO3STATE:
        return SWELL_ACCESSKIT_ROLE_CHECK_BOX;
      case BS_AUTORADIOBUTTON:
        return SWELL_ACCESSKIT_ROLE_RADIO_BUTTON;
    }
    return (hwnd->m_style & BS_DEFPUSHBUTTON) ? SWELL_ACCESSKIT_ROLE_DEFAULT_BUTTON : SWELL_ACCESSKIT_ROLE_BUTTON;
  }
  return SWELL_ACCESSKIT_ROLE_UNKNOWN;
}

static void swell_accesskit_copy_string(swell_accesskit_string_ref *dest, std::string *storage, const char *value)
{
  if (!dest || !storage) return;
  dest->ptr = NULL;
  dest->len = 0;
  storage->clear();
  if (!value) return;
  storage->assign(value);
  dest->ptr = storage->c_str();
  dest->len = storage->size();
}

static void swell_accesskit_copy_std_string(swell_accesskit_string_ref *dest, std::string *storage, const std::string &value)
{
  swell_accesskit_copy_string(dest, storage, value.c_str());
}

static bool swell_accesskit_menu_item_is_string(const MENUITEMINFO *item)
{
  return item && (item->fType == MFT_STRING || item->fType == MFT_RADIOCHECK);
}

static std::string swell_accesskit_menu_label_without_mnemonic(const char *raw, std::string *access_key, std::string *shortcut)
{
  if (access_key) access_key->clear();
  if (shortcut) shortcut->clear();
  std::string label;
  if (!raw) return label;

  const char *tab = strchr(raw, '\t');
  const char *end = tab ? tab : raw + strlen(raw);
  for (const char *p = raw; p < end && *p; ++p)
  {
    if (*p == '&')
    {
      if (p + 1 < end && p[1] == '&')
      {
        label.push_back('&');
        ++p;
      }
      else if (p + 1 < end && p[1] && access_key && access_key->empty())
      {
        char buf[2] = { (char)toupper((unsigned char)p[1]), 0 };
        access_key->assign(buf);
      }
    }
    else
    {
      label.push_back(*p);
    }
  }
  if (tab && shortcut) shortcut->assign(tab + 1);
  return label;
}

static int swell_accesskit_count_accessible_menu_items(HMENU menu)
{
  int count = 0;
  if (!menu) return count;
  for (int i = 0; i < menu->items.GetSize(); ++i)
  {
    MENUITEMINFO *item = menu->items.Get(i);
    if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP)) ++count;
  }
  return count;
}

static int swell_accesskit_menu_position_in_set(HMENU menu, int index)
{
  int pos = 0;
  if (!menu) return 0;
  for (int i = 0; i <= index && i < menu->items.GetSize(); ++i)
  {
    MENUITEMINFO *item = menu->items.Get(i);
    if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP)) ++pos;
  }
  return pos;
}

static void swell_accesskit_build_character_lengths(const char *value, std::vector<uint8_t> *storage)
{
  if (!storage) return;
  storage->clear();
  if (!value) return;

  while (*value)
  {
    const int len = wdl_utf8_parsechar(value, NULL);
    const int use_len = len > 0 ? wdl_min(len, 255) : 1;
    storage->push_back((uint8_t)use_len);
    value += len > 0 ? len : 1;
  }
}

static int swell_accesskit_measure_text_width(HDC hdc, const char *value, int byte_count)
{
  if (!hdc || !value || byte_count <= 0) return 0;

  RECT rect = {0,};
  DrawText(hdc, value, byte_count, &rect, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
  return rect.right - rect.left;
}

static void swell_accesskit_build_character_geometry(HWND hwnd, const char *value, std::vector<float> *positions, std::vector<float> *widths)
{
  if (!positions || !widths) return;
  positions->clear();
  widths->clear();
  if (!hwnd || !value) return;

  int scroll_x = 0;
  swell_edit_control_get_accessibility_text_state(hwnd, NULL, NULL, NULL, &scroll_x);

  HDC hdc = GetDC(hwnd);
  if (!hdc) return;

  int byte_pos = 0;
  int previous_width = 0;
  const float origin_offset = (float)-scroll_x;
  while (value[byte_pos])
  {
    const int char_len = wdl_utf8_parsechar(value + byte_pos, NULL);
    byte_pos += char_len > 0 ? char_len : 1;
    const int next_width = swell_accesskit_measure_text_width(hdc, value, byte_pos);
    positions->push_back(origin_offset + (float)previous_width);
    widths->push_back((float)wdl_max(next_width - previous_width, 0));
    previous_width = next_width;
  }

  ReleaseDC(hwnd, hdc);
}

static swell_accesskit_rect swell_accesskit_text_run_bounds_for_hwnd(HWND hwnd)
{
  RECT bounds = {0,};
  if (!hwnd) return swell_accesskit_rect_from_rect(&bounds);

  GetClientRect(hwnd, &bounds);
  if (bounds.right - bounds.left > 4)
  {
    bounds.left += 2;
    bounds.right -= 2;
  }
  if (bounds.bottom - bounds.top > 4)
  {
    bounds.top += 2;
    bounds.bottom -= 2;
  }
  ClientToScreen(hwnd, (POINT *)&bounds);
  ClientToScreen(hwnd, ((POINT *)&bounds) + 1);
  return swell_accesskit_rect_from_rect(&bounds);
}

static swell_accesskit_rect swell_accesskit_combo_text_run_bounds_for_hwnd(HWND hwnd)
{
  RECT bounds = {0,};
  if (!hwnd) return swell_accesskit_rect_from_rect(&bounds);
  GetClientRect(hwnd, &bounds);
  bounds.left += SWELL_UI_SCALE(3);
  bounds.right -= SWELL_UI_SCALE(21);
  if (bounds.right < bounds.left) bounds.right = bounds.left;
  ClientToScreen(hwnd, (POINT *)&bounds);
  ClientToScreen(hwnd, ((POINT *)&bounds) + 1);
  return swell_accesskit_rect_from_rect(&bounds);
}

static void swell_accesskit_fill_text_selection(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node || (!swell_accesskit_hwnd_has_text_run(hwnd) && !swell_accesskit_hwnd_has_combo_text_run(hwnd))) return;

  const int text_len = WDL_utf8_get_charlen(hwnd->m_title.Get());
  int cursor_pos = text_len;
  int sel1 = -1;
  int sel2 = -1;
  if (swell_accesskit_hwnd_has_combo_text_run(hwnd))
  {
    const DWORD sel = (DWORD)SendMessage(hwnd, EM_GETSEL, (WPARAM)&sel1, (LPARAM)&sel2);
    if (sel != (DWORD)-1)
    {
      cursor_pos = sel2 >= 0 ? sel2 : text_len;
    }
  }
  else if (!swell_edit_control_get_accessibility_text_state(hwnd, &cursor_pos, &sel1, &sel2, NULL))
  {
    cursor_pos = text_len;
    sel1 = sel2 = -1;
  }

  if (cursor_pos < 0) cursor_pos = 0;
  if (cursor_pos > text_len) cursor_pos = text_len;

  size_t anchor = (size_t)cursor_pos;
  size_t focus = (size_t)cursor_pos;
  if (sel1 >= 0 && sel2 > sel1)
  {
    if (cursor_pos == sel1)
      anchor = (size_t)wdl_min(sel2, text_len);
    else
      anchor = (size_t)wdl_min(sel1, text_len);
  }

  node->pod.text_selection_node = swell_accesskit_hwnd_has_combo_text_run(hwnd) ?
      swell_accesskit_combo_text_run_id_for_hwnd(hwnd) : swell_accesskit_text_run_id_for_hwnd(hwnd);
  node->pod.text_selection_anchor_node = node->pod.text_selection_node;
  node->pod.text_selection_focus_node = node->pod.text_selection_node;
  node->pod.text_selection_anchor = anchor;
  node->pod.text_selection_focus = focus;
}

static void swell_accesskit_fill_button_state(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node || !hwnd->m_classname || strcmp(hwnd->m_classname, "Button")) return;
  if (hwnd->m_style & BS_GROUPBOX) return;

  const int button_style = hwnd->m_style & 0xf;
  if (button_style == BS_AUTOCHECKBOX || button_style == BS_AUTO3STATE || button_style == BS_AUTORADIOBUTTON)
  {
    const int state = (int)SendMessage(hwnd, BM_GETCHECK, 0, 0) & 3;
    node->pod.toggled = state == 2 ? SWELL_ACCESSKIT_TOGGLED_MIXED :
                       state ? SWELL_ACCESSKIT_TOGGLED_TRUE : SWELL_ACCESSKIT_TOGGLED_FALSE;
  }
}

static void swell_accesskit_fill_numeric_state(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node || !hwnd->m_private_data || !hwnd->m_classname) return;

  if (!strcmp(hwnd->m_classname, "msctls_trackbar32") || !strcmp(hwnd->m_classname, "REAPERhfader"))
  {
    int *state = (int *)hwnd->m_private_data;
    const int range = state[1];
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_NUMERIC_VALUE |
                       SWELL_ACCESSKIT_NODE_FLAG_HAS_MIN_NUMERIC_VALUE |
                       SWELL_ACCESSKIT_NODE_FLAG_HAS_MAX_NUMERIC_VALUE |
                       SWELL_ACCESSKIT_NODE_FLAG_HAS_NUMERIC_VALUE_STEP;
    node->pod.numeric_value = state[0];
    node->pod.min_numeric_value = LOWORD(range);
    node->pod.max_numeric_value = HIWORD(range);
    node->pod.numeric_value_step = 1.0;
    node->pod.orientation = SWELL_ACCESSKIT_ORIENTATION_HORIZONTAL;
  }
  else if (!strcmp(hwnd->m_classname, "msctls_progress32"))
  {
    int *state = (int *)hwnd->m_private_data;
    const int range = state[1];
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_NUMERIC_VALUE |
                       SWELL_ACCESSKIT_NODE_FLAG_HAS_MIN_NUMERIC_VALUE |
                       SWELL_ACCESSKIT_NODE_FLAG_HAS_MAX_NUMERIC_VALUE;
    node->pod.numeric_value = state[0];
    node->pod.min_numeric_value = LOWORD(range);
    node->pod.max_numeric_value = HIWORD(range);
    node->pod.numeric_value_step = 0.0;
  }
}

static int swell_accesskit_count_direct_visible_children(HWND hwnd)
{
  int count = swell_accesskit_hwnd_has_text_run(hwnd) ? 1 : 0;
  if (swell_accesskit_hwnd_has_combo_text_run(hwnd)) ++count;
  if (swell_accesskit_hwnd_has_collapsed_combo_option(hwnd)) ++count;
  if (swell_accesskit_hwnd_is_listview(hwnd))
  {
    swell_accesskit_listview_info info;
    swell_accesskit_collection_range range;
    if (swell_accesskit_get_listview_info(hwnd,&info) && swell_accesskit_get_listview_export_range(hwnd,&range))
    {
      if (info.is_report) count += info.column_count + range.count;
      else count += range.count;
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(hwnd))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(hwnd,&info)) count += info.visible_count;
  }
  else if (swell_accesskit_hwnd_is_tab(hwnd))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(hwnd,&info)) count += info.count;
  }
  if (hwnd && !hwnd->m_parent && hwnd->m_menu) ++count;
  HWND child = hwnd ? hwnd->m_children : NULL;
  while (child)
  {
    if (!child->m_hashaddestroy && child->m_visible) ++count;
    child = child->m_next;
  }
  if (hwnd && !hwnd->m_parent)
  {
    HWND menu_owner = swell_accesskit_get_active_menu_owner();
    if (menu_owner && swell_accesskit_contains_hwnd(hwnd, menu_owner))
      count += swell_accesskit_get_active_menu_count();
  }
  return count;
}

static void swell_accesskit_populate_node(HWND hwnd, HWND focused, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;

  node->hwnd = hwnd;
  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_node_id_for_hwnd(hwnd);
  node->pod.role = swell_accesskit_role_for_hwnd(hwnd);
  node->pod.toggled = SWELL_ACCESSKIT_TOGGLED_NONE;
  node->pod.orientation = SWELL_ACCESSKIT_ORIENTATION_NONE;
  node->pod.numeric_value = 0.0;
  node->pod.min_numeric_value = 0.0;
  node->pod.max_numeric_value = 0.0;
  node->pod.numeric_value_step = 0.0;
  RECT empty_rect = {0,};
  node->pod.bounds = swell_accesskit_rect_from_rect(&empty_rect);

  RECT bounds = {0,};
  GetWindowRect(hwnd, &bounds);
  node->pod.bounds = swell_accesskit_rect_from_rect(&bounds);

  if (!hwnd->m_enabled) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_DISABLED;
  if (hwnd->m_classname && !strcmp(hwnd->m_classname, "Edit") && (hwnd->m_style & ES_READONLY))
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_READ_ONLY;
  if (swell_accesskit_hwnd_is_combo(hwnd))
  {
    node->pod.has_popup = SWELL_ACCESSKIT_HAS_POPUP_LISTBOX;
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_EXPANDED;
    HWND menu_owner = swell_accesskit_get_active_menu_owner();
    if (menu_owner == hwnd && swell_accesskit_get_active_menu_count() > 0)
    {
      node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_EXPANDED;
      HWND menu_hwnd = swell_accesskit_get_active_menu_window(0);
      HMENU menu = swell_accesskit_get_active_menu(0);
      if (menu_hwnd && menu && menu->sel_vis >= 0)
        node->pod.active_descendant = swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, menu->sel_vis);
    }
    else
    {
      int selected_index = -1;
      std::string text;
      if (swell_accesskit_get_combo_current_option(hwnd, &text, &selected_index, NULL))
        node->pod.active_descendant = swell_accesskit_combo_option_id_for_hwnd(hwnd, selected_index);
    }
  }
  if (swell_accesskit_hwnd_is_listview(hwnd))
  {
    swell_accesskit_listview_info info;
    if (swell_accesskit_get_listview_info(hwnd,&info))
    {
      node->pod.size_of_set = (size_t)info.item_count;
      node->pod.row_count = (size_t)info.item_count;
      node->pod.column_count = (size_t)info.column_count;
      node->pod.scroll_x = info.scroll_x;
      node->pod.scroll_x_min = 0.0;
      node->pod.scroll_x_max = info.scroll_x_max;
      node->pod.scroll_y = info.scroll_y;
      node->pod.scroll_y_min = 0.0;
      node->pod.scroll_y_max = info.scroll_y_max;
      node->pod.child_action_mask |= SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW_MASK;
      if (info.is_multiselect) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_MULTISELECTABLE;
      const int active = info.focused_index >= 0 ? info.focused_index : info.selected_index;
      if (active >= 0)
      {
        uintptr_t identity = 0;
        swell_accesskit_get_listview_item_identity(hwnd,active,&identity);
        node->pod.active_descendant = info.is_report ? swell_accesskit_grid_row_id_for_hwnd(hwnd,active) :
            swell_accesskit_list_item_id_for_hwnd(hwnd,active,identity);
      }
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(hwnd))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(hwnd,&info))
    {
      node->pod.size_of_set = (size_t)info.visible_count;
      node->pod.scroll_y = info.scroll_y;
      node->pod.scroll_y_min = 0.0;
      node->pod.scroll_y_max = info.scroll_y_max;
      node->pod.child_action_mask |= SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW_MASK;
      if (info.selected_item) node->pod.active_descendant = swell_accesskit_tree_item_id(info.selected_item);
    }
  }

  const char *title = hwnd->m_title.Get();
  if (title && (title[0] || node->pod.role == SWELL_ACCESSKIT_ROLE_TEXT_INPUT || node->pod.role == SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT))
  {
    if (node->pod.role == SWELL_ACCESSKIT_ROLE_TEXT_INPUT || node->pod.role == SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT ||
        node->pod.role == SWELL_ACCESSKIT_ROLE_COMBO_BOX || node->pod.role == SWELL_ACCESSKIT_ROLE_EDITABLE_COMBO_BOX ||
        node->pod.role == SWELL_ACCESSKIT_ROLE_LABEL)
      swell_accesskit_copy_string(&node->pod.value, &node->value_storage, title);
    else
      swell_accesskit_copy_string(&node->pod.label, &node->label_storage, title);
  }
  if (swell_accesskit_hwnd_is_combo(hwnd) && swell_accesskit_get_active_menu_owner() == hwnd)
  {
    swell_accesskit_copy_string(&node->pod.value, &node->value_storage, NULL);
  }

  swell_accesskit_fill_button_state(hwnd, node);
  swell_accesskit_fill_numeric_state(hwnd, node);

  switch (node->pod.role)
  {
    case SWELL_ACCESSKIT_ROLE_TEXT_INPUT:
    case SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT:
    case SWELL_ACCESSKIT_ROLE_EDITABLE_COMBO_BOX:
      node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK;
      if (!(node->pod.flags & SWELL_ACCESSKIT_NODE_FLAG_READ_ONLY))
        node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_SET_VALUE_MASK;
      if (swell_accesskit_hwnd_has_text_run(hwnd) || swell_accesskit_hwnd_has_combo_text_run(hwnd))
      {
        node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_SET_TEXT_SELECTION_MASK;
        swell_accesskit_fill_text_selection(hwnd, node);
      }
    break;
    case SWELL_ACCESSKIT_ROLE_COMBO_BOX:
      node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK;
    break;
    case SWELL_ACCESSKIT_ROLE_BUTTON:
    case SWELL_ACCESSKIT_ROLE_DEFAULT_BUTTON:
    case SWELL_ACCESSKIT_ROLE_CHECK_BOX:
    case SWELL_ACCESSKIT_ROLE_RADIO_BUTTON:
      node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK;
    break;
    case SWELL_ACCESSKIT_ROLE_SLIDER:
      node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK |
                               SWELL_ACCESSKIT_ACTION_INCREMENT_MASK |
                               SWELL_ACCESSKIT_ACTION_DECREMENT_MASK;
    break;
  }

  node->children_storage.reserve((size_t)swell_accesskit_count_direct_visible_children(hwnd));
  HWND child = hwnd->m_children;
  while (child)
  {
    if (!child->m_hashaddestroy && child->m_visible)
      node->children_storage.push_back(swell_accesskit_node_id_for_hwnd(child));
    child = child->m_next;
  }
  if (swell_accesskit_hwnd_has_text_run(hwnd))
    node->children_storage.push_back(swell_accesskit_text_run_id_for_hwnd(hwnd));
  if (swell_accesskit_hwnd_has_combo_text_run(hwnd))
    node->children_storage.push_back(swell_accesskit_combo_text_run_id_for_hwnd(hwnd));
  if (swell_accesskit_hwnd_has_collapsed_combo_option(hwnd))
  {
    int selected_index = -1;
    std::string text;
    swell_accesskit_get_combo_current_option(hwnd, &text, &selected_index, NULL);
    node->children_storage.push_back(swell_accesskit_combo_option_id_for_hwnd(hwnd, selected_index));
  }
  if (swell_accesskit_hwnd_is_listview(hwnd))
  {
    swell_accesskit_listview_info info;
    swell_accesskit_collection_range range;
    if (swell_accesskit_get_listview_info(hwnd,&info) && swell_accesskit_get_listview_export_range(hwnd,&range))
    {
      if (info.is_report)
      {
        for (int col = 0; col < info.column_count; ++col)
          node->children_storage.push_back(swell_accesskit_column_header_id_for_hwnd(hwnd,col));
        for (int row = range.first; row < range.first + range.count; ++row)
          node->children_storage.push_back(swell_accesskit_grid_row_id_for_hwnd(hwnd,row));
      }
      else
      {
        for (int row = range.first; row < range.first + range.count; ++row)
        {
          uintptr_t identity = 0;
          swell_accesskit_get_listview_item_identity(hwnd,row,&identity);
          node->children_storage.push_back(swell_accesskit_list_item_id_for_hwnd(hwnd,row,identity));
        }
      }
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(hwnd))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(hwnd,&info))
    {
      for (int i = 0; i < info.visible_count; ++i)
      {
        HTREEITEM item = swell_accesskit_get_treeview_visible_item(hwnd,i);
        if (item) node->children_storage.push_back(swell_accesskit_tree_item_id(item));
      }
    }
  }
  else if (swell_accesskit_hwnd_is_tab(hwnd))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(hwnd,&info))
    {
      for (int i = 0; i < info.count; ++i)
        node->children_storage.push_back(swell_accesskit_tab_id_for_hwnd(hwnd,i));
    }
  }
  if (!hwnd->m_parent && hwnd->m_menu)
    node->children_storage.push_back(swell_accesskit_menu_bar_id_for_hwnd(hwnd));
  if (!hwnd->m_parent)
  {
    HWND menu_owner = swell_accesskit_get_active_menu_owner();
    if (menu_owner && swell_accesskit_contains_hwnd(hwnd, menu_owner))
    {
      const int active_menu_count = swell_accesskit_get_active_menu_count();
      for (int i = 0; i < active_menu_count; ++i)
      {
        HWND menu_hwnd = swell_accesskit_get_active_menu_window(i);
        if (menu_hwnd) node->children_storage.push_back(swell_accesskit_popup_menu_id_for_hwnd(menu_hwnd));
      }
    }
  }
  node->pod.child_count = node->children_storage.size();
  node->pod.children = node->children_storage.empty() ? NULL : node->children_storage.data();

  if (hwnd == focused) node->pod.action_mask |= 0;
}

static void swell_accesskit_populate_text_run_node(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;

  memset(&node->pod, 0, sizeof(node->pod));
  const bool combo_text_run = swell_accesskit_hwnd_has_combo_text_run(hwnd);
  node->pod.id = combo_text_run ? swell_accesskit_combo_text_run_id_for_hwnd(hwnd) : swell_accesskit_text_run_id_for_hwnd(hwnd);
  node->pod.role = SWELL_ACCESSKIT_ROLE_TEXT_RUN;
  node->pod.bounds = combo_text_run ? swell_accesskit_combo_text_run_bounds_for_hwnd(hwnd) : swell_accesskit_text_run_bounds_for_hwnd(hwnd);
  swell_accesskit_copy_string(&node->pod.value, &node->value_storage, hwnd->m_title.Get() ? hwnd->m_title.Get() : "");
  swell_accesskit_build_character_lengths(hwnd->m_title.Get(), &node->character_lengths_storage);
  swell_accesskit_build_character_geometry(hwnd, hwnd->m_title.Get(), &node->character_positions_storage, &node->character_widths_storage);
  node->pod.character_length_count = node->character_lengths_storage.size();
  node->pod.character_lengths = node->character_lengths_storage.empty() ? NULL : node->character_lengths_storage.data();
  node->pod.character_position_count = node->character_positions_storage.size();
  node->pod.character_positions = node->character_positions_storage.empty() ? NULL : node->character_positions_storage.data();
  node->pod.character_width_count = node->character_widths_storage.size();
  node->pod.character_widths = node->character_widths_storage.empty() ? NULL : node->character_widths_storage.data();
}

static void swell_accesskit_populate_combo_option_node(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;

  std::string text;
  int selected_index = -1;
  int item_count = 0;
  if (!swell_accesskit_get_combo_current_option(hwnd, &text, &selected_index, &item_count)) return;

  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_combo_option_id_for_hwnd(hwnd, selected_index);
  node->pod.role = SWELL_ACCESSKIT_ROLE_LIST_BOX_OPTION;
  swell_accesskit_copy_std_string(&node->pod.label, &node->label_storage, text);
  node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED | SWELL_ACCESSKIT_NODE_FLAG_SELECTED;
  if (selected_index >= 0) node->pod.position_in_set = (size_t)selected_index + 1;
  if (item_count > 0) node->pod.size_of_set = (size_t)item_count;
  node->pod.bounds = swell_accesskit_combo_text_run_bounds_for_hwnd(hwnd);
}

static void swell_accesskit_populate_menu_item_common(SWELL_AccessKitOwnedNode *node, MENUITEMINFO *item, bool combo_option)
{
  if (!node || !item) return;

  if (item->fState & (MF_GRAYED | MF_DISABLED)) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_DISABLED;
  if (combo_option)
  {
    node->pod.role = SWELL_ACCESSKIT_ROLE_MENU_LIST_OPTION;
  }
  else if (item->fState & MF_CHECKED)
  {
    node->pod.role = (item->fType & MFT_RADIOCHECK) ? SWELL_ACCESSKIT_ROLE_MENU_ITEM_RADIO : SWELL_ACCESSKIT_ROLE_MENU_ITEM_CHECK_BOX;
    node->pod.toggled = SWELL_ACCESSKIT_TOGGLED_TRUE;
  }
  else
  {
    node->pod.role = SWELL_ACCESSKIT_ROLE_MENU_ITEM;
  }

  if (item->hSubMenu)
  {
    node->pod.has_popup = SWELL_ACCESSKIT_HAS_POPUP_MENU;
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_EXPANDED;
  }

  std::string access_key;
  std::string shortcut;
  const std::string label = swell_accesskit_menu_label_without_mnemonic(
      swell_accesskit_menu_item_is_string(item) ? item->dwTypeData : "", &access_key, &shortcut);
  swell_accesskit_copy_std_string(&node->pod.label, &node->label_storage, label);
  if (!access_key.empty())
    swell_accesskit_copy_std_string(&node->pod.access_key, &node->access_key_storage, access_key);
  if (!shortcut.empty())
    swell_accesskit_copy_std_string(&node->pod.keyboard_shortcut, &node->keyboard_shortcut_storage, shortcut);
  node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK;
}

static bool swell_accesskit_find_menu_label_for_submenu(HMENU menu, HMENU target, std::string *label)
{
  if (!menu || !target || !label) return false;
  for (int i = 0; i < menu->items.GetSize(); ++i)
  {
    MENUITEMINFO *item = menu->items.Get(i);
    if (!item) continue;
    if (item->hSubMenu == target)
    {
      std::string access_key;
      std::string shortcut;
      *label = swell_accesskit_menu_label_without_mnemonic(
          swell_accesskit_menu_item_is_string(item) ? item->dwTypeData : "", &access_key, &shortcut);
      return !label->empty();
    }
    if (item->hSubMenu && swell_accesskit_find_menu_label_for_submenu(item->hSubMenu, target, label))
      return true;
  }
  return false;
}

static void swell_accesskit_populate_menu_bar_node(HWND hwnd, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node || !hwnd->m_menu) return;
  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_menu_bar_id_for_hwnd(hwnd);
  node->pod.role = SWELL_ACCESSKIT_ROLE_MENU_BAR;
  RECT bounds = {0,};
  GetWindowRect(hwnd, &bounds);
  bounds.bottom = bounds.top + GetSystemMetrics(SM_CYMENU);
  node->pod.bounds = swell_accesskit_rect_from_rect(&bounds);
  if (swell_accesskit_get_active_menubar_window() == hwnd && swell_accesskit_get_active_menubar_index() >= 0)
    node->pod.active_descendant = swell_accesskit_menu_bar_item_id_for_hwnd(hwnd, swell_accesskit_get_active_menubar_index());
  for (int i = 0; i < hwnd->m_menu->items.GetSize(); ++i)
  {
    MENUITEMINFO *item = hwnd->m_menu->items.Get(i);
    if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP))
      node->children_storage.push_back(swell_accesskit_menu_bar_item_id_for_hwnd(hwnd, i));
  }
  node->pod.child_count = node->children_storage.size();
  node->pod.children = node->children_storage.empty() ? NULL : node->children_storage.data();
}

static void swell_accesskit_populate_menu_bar_item_node(HWND hwnd, int index, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node || !hwnd->m_menu) return;
  MENUITEMINFO *item = hwnd->m_menu->items.Get(index);
  if (!item) return;
  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_menu_bar_item_id_for_hwnd(hwnd, index);
  swell_accesskit_populate_menu_item_common(node, item, false);
  node->pod.position_in_set = swell_accesskit_menu_position_in_set(hwnd->m_menu, index);
  node->pod.size_of_set = swell_accesskit_count_accessible_menu_items(hwnd->m_menu);
  if (swell_accesskit_get_active_menubar_window() == hwnd && swell_accesskit_get_active_menubar_index() == index)
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED | SWELL_ACCESSKIT_NODE_FLAG_SELECTED;

  RECT bounds = {0,};
  GetWindowRect(hwnd, &bounds);
  const int menu_h = GetSystemMetrics(SM_CYMENU);
  const int count = wdl_max(swell_accesskit_count_accessible_menu_items(hwnd->m_menu), 1);
  const int width = wdl_max((bounds.right - bounds.left) / count, 1);
  const int pos = wdl_max((int)node->pod.position_in_set - 1, 0);
  bounds.left += width * pos;
  bounds.right = bounds.left + width;
  bounds.bottom = bounds.top + menu_h;
  node->pod.bounds = swell_accesskit_rect_from_rect(&bounds);
}

static void swell_accesskit_populate_popup_menu_node(HWND menu_hwnd, HMENU menu, bool combo_popup, SWELL_AccessKitOwnedNode *node)
{
  if (!menu_hwnd || !menu || !node) return;
  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_popup_menu_id_for_hwnd(menu_hwnd);
  node->pod.role = combo_popup ? SWELL_ACCESSKIT_ROLE_MENU_LIST_POPUP : SWELL_ACCESSKIT_ROLE_MENU;
  if (!combo_popup)
  {
    HWND owner = swell_accesskit_get_active_menu_owner();
    std::string label;
    if (owner && owner->m_menu && swell_accesskit_find_menu_label_for_submenu(owner->m_menu, menu, &label))
      swell_accesskit_copy_std_string(&node->pod.label, &node->label_storage, label);
  }
  RECT bounds = {0,};
  GetWindowRect(menu_hwnd, &bounds);
  node->pod.bounds = swell_accesskit_rect_from_rect(&bounds);
  node->pod.size_of_set = swell_accesskit_count_accessible_menu_items(menu);
  if (menu->sel_vis >= 0)
    node->pod.active_descendant = swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, menu->sel_vis);
  for (int i = 0; i < menu->items.GetSize(); ++i)
  {
    MENUITEMINFO *item = menu->items.Get(i);
    if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP))
      node->children_storage.push_back(swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, i));
  }
  node->pod.child_count = node->children_storage.size();
  node->pod.children = node->children_storage.empty() ? NULL : node->children_storage.data();
}

static void swell_accesskit_populate_popup_item_node(HWND menu_hwnd, HMENU menu, int index, bool combo_option, SWELL_AccessKitOwnedNode *node)
{
  if (!menu_hwnd || !menu || !node) return;
  MENUITEMINFO *item = menu->items.Get(index);
  if (!item) return;
  memset(&node->pod, 0, sizeof(node->pod));
  node->pod.id = swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, index);
  swell_accesskit_populate_menu_item_common(node, item, combo_option);
  node->pod.position_in_set = swell_accesskit_menu_position_in_set(menu, index);
  node->pod.size_of_set = swell_accesskit_count_accessible_menu_items(menu);
  if (menu->sel_vis == index)
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED | SWELL_ACCESSKIT_NODE_FLAG_SELECTED;

  RECT bounds = {0,};
  GetWindowRect(menu_hwnd, &bounds);
  const int count = wdl_max(swell_accesskit_count_accessible_menu_items(menu), 1);
  const int height = wdl_max((bounds.bottom - bounds.top) / count, 1);
  const int pos = wdl_max((int)node->pod.position_in_set - 1, 0);
  bounds.top += height * pos;
  bounds.bottom = bounds.top + height;
  node->pod.bounds = swell_accesskit_rect_from_rect(&bounds);
}

static void swell_accesskit_set_labelled_by(SWELL_AccessKitOwnedNode *node, uint64_t label_id)
{
  if (!node || !label_id || node->pod.label.ptr || node->pod.labelled_by_count) return;
  node->labelled_by_storage.push_back(label_id);
  node->pod.labelled_by_count = node->labelled_by_storage.size();
  node->pod.labelled_by = node->labelled_by_storage.data();
}

static bool swell_accesskit_role_takes_nearby_label(uint32_t role)
{
  return role == SWELL_ACCESSKIT_ROLE_TEXT_INPUT ||
      role == SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT ||
      role == SWELL_ACCESSKIT_ROLE_COMBO_BOX ||
      role == SWELL_ACCESSKIT_ROLE_EDITABLE_COMBO_BOX ||
      role == SWELL_ACCESSKIT_ROLE_SLIDER ||
      role == SWELL_ACCESSKIT_ROLE_PROGRESS_INDICATOR;
}

static bool swell_accesskit_is_text_static(HWND hwnd)
{
  if (!hwnd || hwnd->m_hashaddestroy || !hwnd->m_visible ||
      !hwnd->m_classname || strcmp(hwnd->m_classname,"Static") ||
      !hwnd->m_title.Get() || !hwnd->m_title.Get()[0])
    return false;

  switch (hwnd->m_style & SS_TYPEMASK)
  {
    case SS_BLACKRECT:
    case SS_ETCHEDHORZ:
    case SS_ETCHEDVERT:
    case SS_ETCHEDFRAME:
      return false;
  }
  return true;
}

static bool swell_accesskit_hwnd_precedes_sibling(HWND candidate, HWND hwnd)
{
  if (!candidate || !hwnd || candidate->m_parent != hwnd->m_parent) return false;
  for (HWND sibling = candidate; sibling; sibling = sibling->m_next)
  {
    if (sibling == hwnd) return true;
  }
  return false;
}

static HWND swell_accesskit_find_preceding_static_label(HWND hwnd)
{
  if (!hwnd || !hwnd->m_parent) return NULL;
  for (HWND sibling = hwnd->m_prev; sibling; sibling = sibling->m_prev)
  {
    if (sibling->m_hashaddestroy || !sibling->m_visible) continue;
    if (swell_accesskit_is_text_static(sibling)) return sibling;

    const uint32_t role = swell_accesskit_role_for_hwnd(sibling);
    if (sibling->m_wantfocus || swell_accesskit_role_takes_nearby_label(role))
      break;
  }
  return NULL;
}

static void swell_accesskit_apply_nearby_labels(SWELL_AccessKitOwnedSnapshot *snapshot)
{
  if (!snapshot) return;
  for (size_t i = 0; i < snapshot->nodes.size(); ++i)
  {
    SWELL_AccessKitOwnedNode *node = &snapshot->nodes[i];
    if (!swell_accesskit_role_takes_nearby_label(node->pod.role) || node->pod.label.ptr || node->pod.labelled_by_count) continue;

    HWND sibling_label = swell_accesskit_find_preceding_static_label(node->hwnd);
    if (sibling_label)
    {
      swell_accesskit_set_labelled_by(node,swell_accesskit_node_id_for_hwnd(sibling_label));
      continue;
    }

    int best_same_row = -1;
    double best_same_row_gap = 1.0e30;
    int best_above = -1;
    double best_above_gap = 1.0e30;
    const double node_mid_y = (node->pod.bounds.y0 + node->pod.bounds.y1) * 0.5;
    const double node_width = wdl_max(node->pod.bounds.x1 - node->pod.bounds.x0, 1.0);
    const double node_height = wdl_max(node->pod.bounds.y1 - node->pod.bounds.y0, 1.0);
    const double max_same_row_gap = wdl_max(node_height * 2.0, (double)SWELL_UI_SCALE(32));
    const double max_above_gap = wdl_max(node_height * 0.75, (double)SWELL_UI_SCALE(12));
    for (size_t j = 0; j < snapshot->nodes.size(); ++j)
    {
      const SWELL_AccessKitOwnedNode *candidate = &snapshot->nodes[j];
      if (candidate->pod.role != SWELL_ACCESSKIT_ROLE_LABEL || !candidate->pod.value.ptr) continue;
      if (!swell_accesskit_is_text_static(candidate->hwnd) ||
          !swell_accesskit_hwnd_precedes_sibling(candidate->hwnd,node->hwnd))
        continue;
      const double candidate_width = wdl_max(candidate->pod.bounds.x1 - candidate->pod.bounds.x0, 1.0);
      if (candidate->pod.bounds.x1 <= node->pod.bounds.x0)
      {
        const double cand_mid_y = (candidate->pod.bounds.y0 + candidate->pod.bounds.y1) * 0.5;
        const double row_delta = cand_mid_y > node_mid_y ? cand_mid_y - node_mid_y : node_mid_y - cand_mid_y;
        const double x_gap = node->pod.bounds.x0 - candidate->pod.bounds.x1;
        if (row_delta <= node_height * 0.6 && x_gap <= max_same_row_gap)
        {
          const double gap = x_gap + row_delta;
          if (gap < best_same_row_gap)
          {
            best_same_row_gap = gap;
            best_same_row = (int)j;
          }
        }
      }
      if (candidate->pod.bounds.y1 <= node->pod.bounds.y0)
      {
        const double x_overlap = wdl_min(node->pod.bounds.x1,candidate->pod.bounds.x1) -
            wdl_max(node->pod.bounds.x0,candidate->pod.bounds.x0);
        if (x_overlap >= wdl_min(node_width, candidate_width) * 0.5)
        {
          const double gap = node->pod.bounds.y0 - candidate->pod.bounds.y1;
          if (gap <= max_above_gap && gap < best_above_gap)
          {
            best_above_gap = gap;
            best_above = (int)j;
          }
        }
      }
    }
    if (best_same_row >= 0)
      swell_accesskit_set_labelled_by(node,snapshot->nodes[(size_t)best_same_row].pod.id);
    else if (best_above >= 0)
      swell_accesskit_set_labelled_by(node,snapshot->nodes[(size_t)best_above].pod.id);
  }
}

static void swell_accesskit_screen_rect(HWND hwnd, RECT *rect)
{
  if (!hwnd || !rect) return;
  ClientToScreen(hwnd,(POINT *)rect);
  ClientToScreen(hwnd,((POINT *)rect) + 1);
}

static void swell_accesskit_populate_list_item_node(HWND hwnd, int index, bool listbox, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;
  memset(&node->pod,0,sizeof(node->pod));
  uintptr_t identity = 0;
  swell_accesskit_get_listview_item_identity(hwnd,index,&identity);
  node->pod.id = swell_accesskit_list_item_id_for_hwnd(hwnd,index,identity);
  node->pod.role = listbox ? SWELL_ACCESSKIT_ROLE_LIST_BOX_OPTION : SWELL_ACCESSKIT_ROLE_LIST_ITEM;
  char text[1024];
  swell_accesskit_get_listview_item_text(hwnd,index,0,text,sizeof(text));
  swell_accesskit_copy_string(&node->pod.label,&node->label_storage,text);
  node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED;
  if (ListView_GetItemState(hwnd,index,LVIS_SELECTED)) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_SELECTED;
  node->pod.position_in_set = (size_t)index + 1;
  node->pod.size_of_set = (size_t)ListView_GetItemCount(hwnd);
  node->pod.action_mask = SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK |
      SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW_MASK;
  RECT rect = {0,};
  swell_accesskit_get_listview_item_rect(hwnd,index,-1,&rect);
  swell_accesskit_screen_rect(hwnd,&rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&rect);
}

static void swell_accesskit_populate_column_header_node(HWND hwnd, int column, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;
  memset(&node->pod,0,sizeof(node->pod));
  node->pod.id = swell_accesskit_column_header_id_for_hwnd(hwnd,column);
  node->pod.role = SWELL_ACCESSKIT_ROLE_COLUMN_HEADER;
  char text[512];
  swell_accesskit_get_listview_column_text(hwnd,column,text,sizeof(text));
  swell_accesskit_copy_string(&node->pod.label,&node->label_storage,text);
  node->pod.column_index = (size_t)column + 1;
  RECT rect = {0,};
  swell_accesskit_listview_info info;
  swell_accesskit_get_listview_info(hwnd,&info);
  rect.left = column == 0 ? 0 : column * 80;
  rect.right = rect.left + 80;
  rect.top = 0;
  rect.bottom = info.header_height;
  swell_accesskit_screen_rect(hwnd,&rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&rect);
}

static void swell_accesskit_append_listview_cell_text(HWND hwnd, int row, int col, std::string *label)
{
  if (!hwnd || !label) return;

  char text[1024];
  if (!swell_accesskit_get_listview_item_text(hwnd,row,col,text,sizeof(text)) || !text[0]) return;

  if (!label->empty()) label->append(", ");

  char header[512];
  header[0] = 0;
  swell_accesskit_get_listview_column_text(hwnd,col,header,sizeof(header));
  if (header[0])
  {
    label->append(header);
    label->append(": ");
  }
  label->append(text);
}

static void swell_accesskit_populate_grid_row_node(HWND hwnd, int row, int column_count, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;
  memset(&node->pod,0,sizeof(node->pod));
  node->pod.id = swell_accesskit_grid_row_id_for_hwnd(hwnd,row);
  node->pod.role = SWELL_ACCESSKIT_ROLE_ROW;
  std::string label;
  for (int col = 0; col < column_count; ++col)
    swell_accesskit_append_listview_cell_text(hwnd,row,col,&label);
  if (label.empty())
  {
    char buf[64];
    snprintf(buf,sizeof(buf),"Row %d",row + 1);
    label.assign(buf);
  }
  swell_accesskit_copy_std_string(&node->pod.label,&node->label_storage,label);
  node->pod.row_index = (size_t)row + 1;
  node->pod.position_in_set = (size_t)row + 1;
  node->pod.size_of_set = (size_t)ListView_GetItemCount(hwnd);
  node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED;
  if (ListView_GetItemState(hwnd,row,LVIS_SELECTED)) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_SELECTED;
  node->pod.action_mask = SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK |
      SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW_MASK;
  RECT rect = {0,};
  swell_accesskit_get_listview_item_rect(hwnd,row,-1,&rect);
  swell_accesskit_screen_rect(hwnd,&rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&rect);
  for (int col = 0; col < column_count; ++col)
    node->children_storage.push_back(swell_accesskit_grid_cell_id_for_hwnd(hwnd,row,col));
  node->pod.child_count = node->children_storage.size();
  node->pod.children = node->children_storage.empty() ? NULL : node->children_storage.data();
}

static void swell_accesskit_populate_grid_cell_node(HWND hwnd, int row, int col, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;
  memset(&node->pod,0,sizeof(node->pod));
  node->pod.id = swell_accesskit_grid_cell_id_for_hwnd(hwnd,row,col);
  node->pod.role = SWELL_ACCESSKIT_ROLE_GRID_CELL;
  char text[1024];
  swell_accesskit_get_listview_item_text(hwnd,row,col,text,sizeof(text));
  swell_accesskit_copy_string(&node->pod.label,&node->label_storage,text);
  node->pod.row_index = (size_t)row + 1;
  node->pod.column_index = (size_t)col + 1;
  node->labelled_by_storage.push_back(swell_accesskit_column_header_id_for_hwnd(hwnd,col));
  node->pod.labelled_by_count = node->labelled_by_storage.size();
  node->pod.labelled_by = node->labelled_by_storage.data();
  RECT rect = {0,};
  swell_accesskit_get_listview_item_rect(hwnd,row,col,&rect);
  swell_accesskit_screen_rect(hwnd,&rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&rect);
}

static void swell_accesskit_populate_tree_item_node(HWND hwnd, HTREEITEM item, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !item || !node) return;
  swell_accesskit_treeitem_info info;
  if (!swell_accesskit_get_treeview_item_info(hwnd,item,&info)) return;
  memset(&node->pod,0,sizeof(node->pod));
  node->pod.id = swell_accesskit_tree_item_id(item);
  node->pod.role = SWELL_ACCESSKIT_ROLE_TREE_ITEM;
  swell_accesskit_copy_string(&node->pod.label,&node->label_storage,info.label);
  node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED;
  if (info.selected) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_SELECTED;
  if (info.has_children)
  {
    node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_EXPANDED;
    if (info.expanded) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_EXPANDED;
    node->pod.action_mask |= info.expanded ? SWELL_ACCESSKIT_ACTION_COLLAPSE_MASK : SWELL_ACCESSKIT_ACTION_EXPAND_MASK;
  }
  node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK |
      SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW_MASK;
  node->pod.level = (size_t)info.level;
  node->pod.position_in_set = (size_t)info.position_in_set;
  node->pod.size_of_set = (size_t)info.size_of_set;
  swell_accesskit_screen_rect(hwnd,&info.rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&info.rect);
}

static void swell_accesskit_populate_tab_node(HWND hwnd, int index, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;
  swell_accesskit_tab_info info;
  if (!swell_accesskit_get_tab_info(hwnd,&info)) return;
  memset(&node->pod,0,sizeof(node->pod));
  node->pod.id = swell_accesskit_tab_id_for_hwnd(hwnd,index);
  node->pod.role = SWELL_ACCESSKIT_ROLE_TAB;
  char text[512];
  swell_accesskit_get_tab_text(hwnd,index,text,sizeof(text));
  swell_accesskit_copy_string(&node->pod.label,&node->label_storage,text);
  node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_HAS_SELECTED;
  if (index == info.selected_index) node->pod.flags |= SWELL_ACCESSKIT_NODE_FLAG_SELECTED;
  node->pod.position_in_set = (size_t)index + 1;
  node->pod.size_of_set = (size_t)info.count;
  node->pod.action_mask = SWELL_ACCESSKIT_ACTION_FOCUS_MASK | SWELL_ACCESSKIT_ACTION_CLICK_MASK;
  RECT rect = {0,};
  swell_accesskit_get_tab_rect(hwnd,index,&rect);
  swell_accesskit_screen_rect(hwnd,&rect);
  node->pod.bounds = swell_accesskit_rect_from_rect(&rect);
}

static void swell_accesskit_snapshot_build_recursive(SWELL_AccessKitOwnedSnapshot *snapshot, HWND hwnd, HWND focused)
{
  if (!snapshot || !hwnd || hwnd->m_hashaddestroy || !hwnd->m_visible) return;

  snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
  SWELL_AccessKitOwnedNode *node = &snapshot->nodes.back();
  swell_accesskit_populate_node(hwnd, focused, node);

  if (hwnd == focused) snapshot->focus_id = node->pod.id;

  if (swell_accesskit_hwnd_has_text_run(hwnd))
  {
    snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
    swell_accesskit_populate_text_run_node(hwnd, &snapshot->nodes.back());
  }
  if (swell_accesskit_hwnd_has_combo_text_run(hwnd))
  {
    snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
    swell_accesskit_populate_text_run_node(hwnd, &snapshot->nodes.back());
  }
  if (swell_accesskit_hwnd_has_collapsed_combo_option(hwnd))
  {
    snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
    swell_accesskit_populate_combo_option_node(hwnd, &snapshot->nodes.back());
  }
  if (swell_accesskit_hwnd_is_listview(hwnd))
  {
    swell_accesskit_listview_info info;
    swell_accesskit_collection_range range;
    if (swell_accesskit_get_listview_info(hwnd,&info) && swell_accesskit_get_listview_export_range(hwnd,&range))
    {
      if (info.is_report)
      {
        for (int col = 0; col < info.column_count; ++col)
        {
          snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
          swell_accesskit_populate_column_header_node(hwnd,col,&snapshot->nodes.back());
        }
        for (int row = range.first; row < range.first + range.count; ++row)
        {
          snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
          swell_accesskit_populate_grid_row_node(hwnd,row,info.column_count,&snapshot->nodes.back());
          for (int col = 0; col < info.column_count; ++col)
          {
            snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
            swell_accesskit_populate_grid_cell_node(hwnd,row,col,&snapshot->nodes.back());
          }
        }
      }
      else
      {
        for (int row = range.first; row < range.first + range.count; ++row)
        {
          snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
          swell_accesskit_populate_list_item_node(hwnd,row,info.is_listbox,&snapshot->nodes.back());
        }
      }
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(hwnd))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(hwnd,&info))
    {
      for (int i = 0; i < info.visible_count; ++i)
      {
        HTREEITEM item = swell_accesskit_get_treeview_visible_item(hwnd,i);
        if (item)
        {
          snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
          swell_accesskit_populate_tree_item_node(hwnd,item,&snapshot->nodes.back());
        }
      }
    }
  }
  else if (swell_accesskit_hwnd_is_tab(hwnd))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(hwnd,&info))
    {
      for (int i = 0; i < info.count; ++i)
      {
        snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
        swell_accesskit_populate_tab_node(hwnd,i,&snapshot->nodes.back());
      }
    }
  }

  HWND child = hwnd->m_children;
  while (child)
  {
    if (!child->m_hashaddestroy && child->m_visible)
      swell_accesskit_snapshot_build_recursive(snapshot, child, focused);
    child = child->m_next;
  }

  if (!hwnd->m_parent && hwnd->m_menu)
  {
    snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
    swell_accesskit_populate_menu_bar_node(hwnd, &snapshot->nodes.back());
    for (int i = 0; i < hwnd->m_menu->items.GetSize(); ++i)
    {
      MENUITEMINFO *item = hwnd->m_menu->items.Get(i);
      if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP))
      {
        snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
        swell_accesskit_populate_menu_bar_item_node(hwnd, i, &snapshot->nodes.back());
      }
    }
  }

  if (!hwnd->m_parent)
  {
    HWND menu_owner = swell_accesskit_get_active_menu_owner();
    const bool combo_popup = swell_accesskit_hwnd_is_combo(menu_owner);
    if (menu_owner && swell_accesskit_contains_hwnd(hwnd, menu_owner))
    {
      const int active_menu_count = swell_accesskit_get_active_menu_count();
      for (int menu_index = 0; menu_index < active_menu_count; ++menu_index)
      {
        HWND menu_hwnd = swell_accesskit_get_active_menu_window(menu_index);
        HMENU menu = swell_accesskit_get_active_menu(menu_index);
        if (!menu_hwnd || !menu) continue;
        snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
        swell_accesskit_populate_popup_menu_node(menu_hwnd, menu, combo_popup, &snapshot->nodes.back());
        for (int i = 0; i < menu->items.GetSize(); ++i)
        {
          MENUITEMINFO *item = menu->items.Get(i);
          if (swell_accesskit_menu_item_is_string(item) || (item && item->fType == MFT_BITMAP))
          {
            snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
            swell_accesskit_populate_popup_item_node(menu_hwnd, menu, i, combo_popup, &snapshot->nodes.back());
          }
        }
      }
    }
  }
}

static bool swell_accesskit_build_snapshot(HWND root, SWELL_AccessKitOwnedSnapshot *snapshot)
{
  if (!snapshot || !root || root->m_hashaddestroy || !root->m_visible) return false;

  const int node_count = swell_accesskit_count_nodes(root);
  if (node_count < 1) return false;

  snapshot->root_id = swell_accesskit_node_id_for_hwnd(root);
  snapshot->focus_id = snapshot->root_id;
  snapshot->nodes.clear();
  snapshot->nodes.reserve((size_t)node_count);

  HWND focused = SWELL_GetFocusedChild(root);
  if (focused && !swell_accesskit_contains_hwnd(root, focused)) focused = NULL;
  if (!focused) focused = GetFocus();
  if (focused && swell_accesskit_get_root(focused) != root) focused = NULL;

  swell_accesskit_snapshot_build_recursive(snapshot, root, focused);
  swell_accesskit_apply_nearby_labels(snapshot);

  if (focused && swell_accesskit_hwnd_is_listview(focused))
  {
    swell_accesskit_listview_info info;
    if (swell_accesskit_get_listview_info(focused,&info))
    {
      const int active = info.focused_index >= 0 ? info.focused_index : info.selected_index;
      if (active >= 0)
      {
        uintptr_t identity = 0;
        swell_accesskit_get_listview_item_identity(focused,active,&identity);
        snapshot->focus_id = info.is_report ? swell_accesskit_grid_row_id_for_hwnd(focused,active) :
            swell_accesskit_list_item_id_for_hwnd(focused,active,identity);
      }
    }
  }
  else if (focused && swell_accesskit_hwnd_is_treeview(focused))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(focused,&info) && info.selected_item)
      snapshot->focus_id = swell_accesskit_tree_item_id(info.selected_item);
  }
  else if (focused && swell_accesskit_hwnd_is_tab(focused))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(focused,&info) && info.selected_index >= 0)
      snapshot->focus_id = swell_accesskit_tab_id_for_hwnd(focused,info.selected_index);
  }

  HWND active_menubar = swell_accesskit_get_active_menubar_window();
  const int active_menubar_index = swell_accesskit_get_active_menubar_index();
  if (active_menubar && swell_accesskit_contains_hwnd(root, active_menubar) && active_menubar_index >= 0)
    snapshot->focus_id = swell_accesskit_menu_bar_item_id_for_hwnd(active_menubar, active_menubar_index);

  HWND menu_owner = swell_accesskit_get_active_menu_owner();
  if (menu_owner && swell_accesskit_contains_hwnd(root, menu_owner) && swell_accesskit_get_active_menu_count() > 0)
  {
    HWND menu_hwnd = swell_accesskit_get_active_menu_window(swell_accesskit_get_active_menu_count() - 1);
    HMENU menu = swell_accesskit_get_active_menu(swell_accesskit_get_active_menu_count() - 1);
    if (menu_hwnd && menu && menu->sel_vis >= 0)
      snapshot->focus_id = swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, menu->sel_vis);
    else if (menu_hwnd)
      snapshot->focus_id = swell_accesskit_popup_menu_id_for_hwnd(menu_hwnd);
  }

  snapshot->exported_nodes.resize(snapshot->nodes.size());
  size_t i;
  for (i = 0; i < snapshot->nodes.size(); ++i)
    snapshot->exported_nodes[i] = snapshot->nodes[i].pod;

  snapshot->snapshot.root_id = snapshot->root_id;
  snapshot->snapshot.focus_id = snapshot->focus_id;
  snapshot->snapshot.node_count = snapshot->exported_nodes.size();
  snapshot->snapshot.nodes = snapshot->exported_nodes.empty() ? NULL : snapshot->exported_nodes.data();
  return snapshot->snapshot.nodes != NULL;
}

static void swell_accesskit_debug_dump(SWELL_AccessKitWindowState *state)
{
  if (!g_accesskit_debug || !state || !state->host) return;

  char *dump = swell_accesskit_host_debug(state->host);
  if (dump)
  {
    fprintf(stderr, "SWELL AccessKit tree for %p:\n%s\n", state->hwnd, dump);
    swell_accesskit_string_free(dump);
  }
}

static bool swell_accesskit_contains_hwnd(HWND parent, HWND target)
{
  if (!parent || !target) return false;
  if (parent == target) return true;

  HWND child = parent->m_children;
  while (child)
  {
    if (swell_accesskit_contains_hwnd(child, target)) return true;
    child = child->m_next;
  }
  return false;
}

static HWND swell_accesskit_resolve_node_id(HWND parent, uint64_t node_id)
{
  if (!parent || parent->m_hashaddestroy || !parent->m_visible) return NULL;
  if (swell_accesskit_node_id_for_hwnd(parent) == node_id) return parent;
  if (swell_accesskit_hwnd_has_text_run(parent) && swell_accesskit_text_run_id_for_hwnd(parent) == node_id) return parent;
  if (swell_accesskit_hwnd_has_combo_text_run(parent) && swell_accesskit_combo_text_run_id_for_hwnd(parent) == node_id) return parent;

  HWND child = parent->m_children;
  while (child)
  {
    HWND resolved = swell_accesskit_resolve_node_id(child, node_id);
    if (resolved) return resolved;
    child = child->m_next;
  }
  return NULL;
}

static bool swell_accesskit_is_slider(HWND hwnd)
{
  return hwnd && hwnd->m_classname &&
         (!strcmp(hwnd->m_classname, "msctls_trackbar32") || !strcmp(hwnd->m_classname, "REAPERhfader"));
}

static bool swell_accesskit_node_is_popup_item(uint64_t node_id)
{
  return (node_id & 0xf000000000000000ull) == SWELL_ACCESSKIT_SYNTHETIC_POPUP_ITEM;
}

static bool swell_accesskit_node_is_menu_bar_item(uint64_t node_id)
{
  return (node_id & 0xf000000000000000ull) == SWELL_ACCESSKIT_SYNTHETIC_MENU_BAR_ITEM;
}

static bool swell_accesskit_node_has_namespace(uint64_t node_id, uint64_t ns)
{
  return (node_id & 0xf000000000000000ull) == ns;
}

static int swell_accesskit_popup_item_index_from_node(uint64_t node_id)
{
  return (int)(node_id & 0xfff);
}

static HWND swell_accesskit_find_collection_for_synthetic_node(HWND parent, uint64_t node_id, int *index_out)
{
  if (!parent || parent->m_hashaddestroy || !parent->m_visible) return NULL;
  if (swell_accesskit_hwnd_is_listview(parent))
  {
    swell_accesskit_listview_info info;
    if (swell_accesskit_get_listview_info(parent,&info))
    {
      for (int i = 0; i < info.item_count; ++i)
      {
        uintptr_t identity = 0;
        swell_accesskit_get_listview_item_identity(parent,i,&identity);
        uint64_t id = info.is_report ? swell_accesskit_grid_row_id_for_hwnd(parent,i) :
            swell_accesskit_list_item_id_for_hwnd(parent,i,identity);
        if (id == node_id)
        {
          if (index_out) *index_out = i;
          return parent;
        }
      }
    }
  }
  else if (swell_accesskit_hwnd_is_treeview(parent))
  {
    swell_accesskit_treeview_info info;
    if (swell_accesskit_get_treeview_info(parent,&info))
    {
      for (int i = 0; i < info.visible_count; ++i)
      {
        HTREEITEM item = swell_accesskit_get_treeview_visible_item(parent,i);
        if (item && swell_accesskit_tree_item_id(item) == node_id)
        {
          if (index_out) *index_out = i;
          return parent;
        }
      }
    }
  }
  else if (swell_accesskit_hwnd_is_tab(parent))
  {
    swell_accesskit_tab_info info;
    if (swell_accesskit_get_tab_info(parent,&info))
    {
      for (int i = 0; i < info.count; ++i)
      {
        if (swell_accesskit_tab_id_for_hwnd(parent,i) == node_id)
        {
          if (index_out) *index_out = i;
          return parent;
        }
      }
    }
  }

  HWND child = parent->m_children;
  while (child)
  {
    HWND found = swell_accesskit_find_collection_for_synthetic_node(child,node_id,index_out);
    if (found) return found;
    child = child->m_next;
  }
  return NULL;
}

static bool swell_accesskit_apply_synthetic_action(SWELL_AccessKitWindowState *state, const swell_accesskit_action_request *action)
{
  if (!state || !action) return false;

  if (swell_accesskit_node_has_namespace(action->target_node, SWELL_ACCESSKIT_SYNTHETIC_LIST_ITEM) ||
      swell_accesskit_node_has_namespace(action->target_node, SWELL_ACCESSKIT_SYNTHETIC_GRID_ROW) ||
      swell_accesskit_node_has_namespace(action->target_node, SWELL_ACCESSKIT_SYNTHETIC_TREE_ITEM) ||
      swell_accesskit_node_has_namespace(action->target_node, SWELL_ACCESSKIT_SYNTHETIC_TAB))
  {
    int index = -1;
    HWND target = swell_accesskit_find_collection_for_synthetic_node(state->hwnd,action->target_node,&index);
    if (!target) return true;
    if (swell_accesskit_hwnd_is_listview(target) && index >= 0)
    {
      if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS)
      {
        SetFocus(target);
        ListView_SetItemState(target,index,LVIS_FOCUSED,LVIS_FOCUSED);
      }
      else if (action->action == SWELL_ACCESSKIT_ACTION_CLICK)
      {
        SetFocus(target);
        if (target->m_classname && !strcmp(target->m_classname,"SysListView32"))
          ListView_SetItemState(target,index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
      }
      else if (action->action == SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW)
      {
        ListView_EnsureVisible(target,index,FALSE);
      }
      state->dirty = true;
      return true;
    }
    if (swell_accesskit_hwnd_is_treeview(target) && index >= 0)
    {
      HTREEITEM item = swell_accesskit_get_treeview_visible_item(target,index);
      if (item)
      {
        if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS || action->action == SWELL_ACCESSKIT_ACTION_CLICK)
        {
          SetFocus(target);
          TreeView_SelectItem(target,item);
        }
        else if (action->action == SWELL_ACCESSKIT_ACTION_EXPAND)
          TreeView_Expand(target,item,TVE_EXPAND);
        else if (action->action == SWELL_ACCESSKIT_ACTION_COLLAPSE)
          TreeView_Expand(target,item,TVE_COLLAPSE);
        else if (action->action == SWELL_ACCESSKIT_ACTION_SCROLL_INTO_VIEW)
          TreeView_EnsureVisible(target,item);
      }
      state->dirty = true;
      return true;
    }
    if (swell_accesskit_hwnd_is_tab(target) && index >= 0)
    {
      if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS || action->action == SWELL_ACCESSKIT_ACTION_CLICK)
      {
        SetFocus(target);
        TabCtrl_SetCurSel(target,index);
        if (target->m_parent)
        {
          NMHDR nm = { target, (UINT_PTR)target->m_id, TCN_SELCHANGE };
          SendMessage(target->m_parent,WM_NOTIFY,nm.idFrom,(LPARAM)&nm);
        }
      }
      state->dirty = true;
      return true;
    }
    return true;
  }

  if (swell_accesskit_node_is_menu_bar_item(action->target_node) && state->hwnd && state->hwnd->m_menu)
  {
    const int index = swell_accesskit_popup_item_index_from_node(action->target_node);
    if (swell_accesskit_menu_bar_item_id_for_hwnd(state->hwnd, index) != action->target_node) return false;
    if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS)
    {
      swell_accesskit_focus_menubar_item(state->hwnd,index);
      state->dirty = true;
    }
    else if (action->action == SWELL_ACCESSKIT_ACTION_CLICK)
    {
      SWELL_AccessKitOwnedNode temp;
      swell_accesskit_populate_menu_bar_item_node(state->hwnd, index, &temp);
      POINT pt = {
        (int)((temp.pod.bounds.x0 + temp.pod.bounds.x1) * 0.5),
        (int)((temp.pod.bounds.y0 + temp.pod.bounds.y1) * 0.5)
      };
      const LPARAM lp = MAKELPARAM(pt.x, pt.y);
      SendMessage(state->hwnd, WM_NCLBUTTONDOWN, HTMENU, lp);
      SendMessage(state->hwnd, WM_NCLBUTTONUP, HTMENU, lp);
      state->dirty = true;
    }
    return true;
  }

  if (!swell_accesskit_node_is_popup_item(action->target_node)) return false;

  const int index = swell_accesskit_popup_item_index_from_node(action->target_node);
  const int active_menu_count = swell_accesskit_get_active_menu_count();
  for (int i = 0; i < active_menu_count; ++i)
  {
    HWND menu_hwnd = swell_accesskit_get_active_menu_window(i);
    if (!menu_hwnd || swell_accesskit_popup_item_id_for_hwnd(menu_hwnd, index) != action->target_node) continue;
    if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS)
      swell_accesskit_select_menu_item(menu_hwnd, index);
    else if (action->action == SWELL_ACCESSKIT_ACTION_CLICK)
      swell_accesskit_activate_menu_item(menu_hwnd, index);
    state->dirty = true;
    return true;
  }
  return true;
}

static void swell_accesskit_apply_action(SWELL_AccessKitWindowState *state, const swell_accesskit_action_request *action)
{
  if (!state || !action) return;

  if (swell_accesskit_apply_synthetic_action(state, action)) return;

  HWND target = swell_accesskit_resolve_node_id(state->hwnd, action->target_node);
  if (!target || target->m_hashaddestroy || !swell_accesskit_contains_hwnd(state->hwnd, target)) return;

  if (action->action == SWELL_ACCESSKIT_ACTION_FOCUS)
  {
    SetFocus(target);
  }
  else if (action->action == SWELL_ACCESSKIT_ACTION_CLICK)
  {
    SendMessage(target, WM_KEYDOWN, VK_SPACE, 0);
  }
  else if (action->action == SWELL_ACCESSKIT_ACTION_SET_VALUE)
  {
    if (target->m_classname && (!strcmp(target->m_classname, "Edit") || swell_accesskit_combo_is_editable(target)) &&
        action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_STRING)
      SetWindowText(target, action->string_value ? action->string_value : "");
    else if (swell_accesskit_is_slider(target) && action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_NUMERIC)
    {
      SendMessage(target, TBM_SETPOS, TRUE, (LPARAM)(int)(action->numeric_value + 0.5));
      if (target->m_parent)
        SendMessage(target->m_parent, WM_HSCROLL, SB_ENDSCROLL, (LPARAM)target);
    }
  }
  else if (action->action == SWELL_ACCESSKIT_ACTION_SET_TEXT_SELECTION)
  {
    if (swell_accesskit_hwnd_has_combo_text_run(target) && action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_TEXT_SELECTION)
      SendMessage(target, EM_SETSEL, action->text_selection_anchor, action->text_selection_focus);
    else if (swell_accesskit_hwnd_has_text_run(target) && action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_TEXT_SELECTION)
      swell_edit_control_set_accessibility_selection(target, (int)action->text_selection_anchor, (int)action->text_selection_focus);
  }
  else if ((action->action == SWELL_ACCESSKIT_ACTION_INCREMENT || action->action == SWELL_ACCESSKIT_ACTION_DECREMENT) &&
           swell_accesskit_is_slider(target) && target->m_private_data)
  {
    int *track = (int *)target->m_private_data;
    const int range = track[1];
    const int low = LOWORD(range);
    const int high = HIWORD(range);
    int new_value = track[0] + (action->action == SWELL_ACCESSKIT_ACTION_INCREMENT ? 1 : -1);
    if (new_value < low) new_value = low;
    if (new_value > high) new_value = high;
    SendMessage(target, TBM_SETPOS, TRUE, (LPARAM)new_value);
    if (target->m_parent)
      SendMessage(target->m_parent, WM_HSCROLL, SB_ENDSCROLL, (LPARAM)target);
  }

  state->dirty = true;
}

static void swell_accesskit_rebuild_and_push(SWELL_AccessKitWindowState *state)
{
  if (!state || !state->host || !swell_accesskit_is_live_toplevel_hwnd(state->hwnd) || !state->hwnd->m_oswindow) return;

  SWELL_AccessKitOwnedSnapshot snapshot;
  if (!swell_accesskit_build_snapshot(state->hwnd, &snapshot)) return;

  swell_accesskit_update_root_bounds(state);
  swell_accesskit_host_update_window_focus_state(state->host, swell_accesskit_is_window_focused(state->hwnd) ? 1 : 0);
  if (swell_accesskit_host_commit_full_tree(state->host, &snapshot.snapshot))
  {
    state->dirty = false;
    swell_accesskit_debug_dump(state);
  }
}

void swell_accesskit_window_created(HWND hwnd)
{
  HWND root = swell_accesskit_get_root(hwnd);
  if (!root || root->m_parent || !root->m_oswindow) return;

  if (getenv("SWELL_ACCESSKIT_DEBUG")) g_accesskit_debug = true;

  WDL_MutexLock lock(&g_accesskit_mutex);
  if (swell_accesskit_find_window_state_locked(root)) return;

  SWELL_AccessKitWindowState *state = (SWELL_AccessKitWindowState *)calloc(1, sizeof(*state));
  if (!state) return;

  uint8_t tree_id[16];
  swell_accesskit_tree_id_for_hwnd(root, tree_id);
  state->host = swell_accesskit_host_new(tree_id);
  if (!state->host)
  {
    free(state);
    return;
  }

  state->hwnd = root;
  state->dirty = true;
  state->next = g_accesskit_windows;
  g_accesskit_windows = state;
}

void swell_accesskit_window_destroyed(HWND hwnd)
{
  HWND root = swell_accesskit_get_root(hwnd);
  if (!root) return;

  SWELL_AccessKitWindowState *state = NULL;
  SWELL_AccessKitWindowState **cursor = &g_accesskit_windows;

  g_accesskit_mutex.Enter();
  while (*cursor)
  {
    if ((*cursor)->hwnd == root)
    {
      state = *cursor;
      *cursor = state->next;
      break;
    }
    cursor = &(*cursor)->next;
  }
  g_accesskit_mutex.Leave();

  if (!state) return;

  swell_accesskit_host_free(state->host);
  free(state);
}

void swell_accesskit_window_changed(HWND hwnd)
{
  HWND root = swell_accesskit_get_root(hwnd);
  if (!root || root->m_parent) return;

  WDL_MutexLock lock(&g_accesskit_mutex);
  SWELL_AccessKitWindowState *state = swell_accesskit_find_window_state_locked(root);
  if (state) state->dirty = true;
}

void swell_accesskit_focus_changed(void)
{
  WDL_MutexLock lock(&g_accesskit_mutex);
  SWELL_AccessKitWindowState *state = g_accesskit_windows;
  while (state)
  {
    if (state->host)
      swell_accesskit_host_update_window_focus_state(state->host, swell_accesskit_is_window_focused(state->hwnd) ? 1 : 0);
    state->dirty = true;
    state = state->next;
  }
}

void swell_accesskit_pump(void)
{
  for (;;)
  {
    g_accesskit_mutex.Enter();
    SWELL_AccessKitWindowState *state = g_accesskit_windows;
    swell_accesskit_action_request action;
    memset(&action, 0, sizeof(action));
    while (state)
    {
      if (state->host && swell_accesskit_host_pop_action(state->host, &action))
        break;
      state = state->next;
    }
    g_accesskit_mutex.Leave();

    if (!state || !action.action) break;
    swell_accesskit_apply_action(state, &action);
    if (action.string_value) swell_accesskit_string_free(action.string_value);
  }

  g_accesskit_mutex.Enter();
  SWELL_AccessKitWindowState *state = g_accesskit_windows;
  while (state)
  {
    SWELL_AccessKitWindowState *next = state->next;
    const bool should_rebuild = state->dirty && state->hwnd && state->hwnd->m_oswindow && !state->hwnd->m_hashaddestroy;
    g_accesskit_mutex.Leave();

    if (should_rebuild) swell_accesskit_rebuild_and_push(state);

    g_accesskit_mutex.Enter();
    if (!swell_accesskit_window_exists_locked(next) && next != NULL)
      next = g_accesskit_windows;
    state = next;
  }
  g_accesskit_mutex.Leave();
}

void swell_accesskit_keyboard_event(uint32_t event_type, uint32_t keyval, uint32_t hardware_keycode, uint32_t modifiers, int32_t timestamp, const char *event_string, bool is_text)
{
  if (g_accesskit_debug)
  {
    fprintf(stderr, "SWELL AccessKit key event type=%u keyval=%u hardware=%u modifiers=%u text=%d\n",
        event_type, keyval, hardware_keycode, modifiers, is_text ? 1 : 0);
  }
  swell_accesskit_notify_keyboard_event(event_type, keyval, hardware_keycode, modifiers, timestamp, event_string, is_text ? 1 : 0);
}

#else

void swell_accesskit_window_created(HWND hwnd) { (void)hwnd; }
void swell_accesskit_window_destroyed(HWND hwnd) { (void)hwnd; }
void swell_accesskit_window_changed(HWND hwnd) { (void)hwnd; }
void swell_accesskit_focus_changed(void) {}
void swell_accesskit_pump(void) {}
void swell_accesskit_keyboard_event(uint32_t event_type, uint32_t keyval, uint32_t hardware_keycode, uint32_t modifiers, int32_t timestamp, const char *event_string, bool is_text)
{
  (void)event_type;
  (void)keyval;
  (void)hardware_keycode;
  (void)modifiers;
  (void)timestamp;
  (void)event_string;
  (void)is_text;
}

#endif

#endif
