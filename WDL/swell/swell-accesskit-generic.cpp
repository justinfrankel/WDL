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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  swell_accesskit_node pod;
  std::string label_storage;
  std::string value_storage;
  std::vector<uint64_t> children_storage;
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

static HWND swell_accesskit_get_root(HWND hwnd)
{
  while (hwnd && hwnd->m_parent) hwnd = hwnd->m_parent;
  return hwnd;
}

static uint64_t swell_accesskit_node_id_for_hwnd(HWND hwnd)
{
  return (uint64_t)(uintptr_t)hwnd;
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
  return root && root->m_oswindow && SWELL_focused_oswindow == root->m_oswindow && swell_is_app_inactive() <= 0;
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
  HWND child = hwnd->m_children;
  while (child)
  {
    count += swell_accesskit_count_nodes(child);
    child = child->m_next;
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
  if (!strcmp(classname, "combobox")) return SWELL_ACCESSKIT_ROLE_COMBO_BOX;
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
  if (!value || !*value) return;
  storage->assign(value);
  dest->ptr = storage->c_str();
  dest->len = storage->size();
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
  int count = 0;
  HWND child = hwnd ? hwnd->m_children : NULL;
  while (child)
  {
    if (!child->m_hashaddestroy && child->m_visible) ++count;
    child = child->m_next;
  }
  return count;
}

static void swell_accesskit_populate_node(HWND hwnd, HWND focused, SWELL_AccessKitOwnedNode *node)
{
  if (!hwnd || !node) return;

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

  const char *title = hwnd->m_title.Get();
  if (title && *title)
  {
    if (node->pod.role == SWELL_ACCESSKIT_ROLE_TEXT_INPUT || node->pod.role == SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT ||
        node->pod.role == SWELL_ACCESSKIT_ROLE_LABEL)
      swell_accesskit_copy_string(&node->pod.value, &node->value_storage, title);
    else
      swell_accesskit_copy_string(&node->pod.label, &node->label_storage, title);
  }

  swell_accesskit_fill_button_state(hwnd, node);
  swell_accesskit_fill_numeric_state(hwnd, node);

  switch (node->pod.role)
  {
    case SWELL_ACCESSKIT_ROLE_TEXT_INPUT:
    case SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT:
      node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_FOCUS_MASK;
      if (!(node->pod.flags & SWELL_ACCESSKIT_NODE_FLAG_READ_ONLY))
        node->pod.action_mask |= SWELL_ACCESSKIT_ACTION_SET_VALUE_MASK;
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
  node->pod.child_count = node->children_storage.size();
  node->pod.children = node->children_storage.empty() ? NULL : node->children_storage.data();

  if (hwnd == focused) node->pod.action_mask |= 0;
}

static void swell_accesskit_snapshot_build_recursive(SWELL_AccessKitOwnedSnapshot *snapshot, HWND hwnd, HWND focused)
{
  if (!snapshot || !hwnd || hwnd->m_hashaddestroy || !hwnd->m_visible) return;

  snapshot->nodes.push_back(SWELL_AccessKitOwnedNode());
  SWELL_AccessKitOwnedNode *node = &snapshot->nodes.back();
  swell_accesskit_populate_node(hwnd, focused, node);

  if (hwnd == focused) snapshot->focus_id = node->pod.id;

  HWND child = hwnd->m_children;
  while (child)
  {
    if (!child->m_hashaddestroy && child->m_visible)
      swell_accesskit_snapshot_build_recursive(snapshot, child, focused);
    child = child->m_next;
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

  HWND focused = GetFocus();
  if (focused && swell_accesskit_get_root(focused) != root) focused = NULL;

  swell_accesskit_snapshot_build_recursive(snapshot, root, focused);

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

static bool swell_accesskit_is_slider(HWND hwnd)
{
  return hwnd && hwnd->m_classname &&
         (!strcmp(hwnd->m_classname, "msctls_trackbar32") || !strcmp(hwnd->m_classname, "REAPERhfader"));
}

static void swell_accesskit_apply_action(SWELL_AccessKitWindowState *state, const swell_accesskit_action_request *action)
{
  if (!state || !action) return;

  HWND target = (HWND)(uintptr_t)action->target_node;
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
    if (target->m_classname && !strcmp(target->m_classname, "Edit") && action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_STRING)
      SetWindowText(target, action->string_value ? action->string_value : "");
    else if (swell_accesskit_is_slider(target) && action->data_kind == SWELL_ACCESSKIT_ACTION_DATA_NUMERIC)
    {
      SendMessage(target, TBM_SETPOS, TRUE, (LPARAM)(int)(action->numeric_value + 0.5));
      if (target->m_parent)
        SendMessage(target->m_parent, WM_HSCROLL, SB_ENDSCROLL, (LPARAM)target);
    }
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
  if (!state || !state->host || !state->hwnd || state->hwnd->m_hashaddestroy || !state->hwnd->m_oswindow) return;

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

#else

void swell_accesskit_window_created(HWND hwnd) { (void)hwnd; }
void swell_accesskit_window_destroyed(HWND hwnd) { (void)hwnd; }
void swell_accesskit_window_changed(HWND hwnd) { (void)hwnd; }
void swell_accesskit_focus_changed(void) {}
void swell_accesskit_pump(void) {}

#endif

#endif
