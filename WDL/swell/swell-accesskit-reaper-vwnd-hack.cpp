/*
  Experimental REAPER/WDL virtual-window AccessKit bridge.

  This is intentionally kept out of wingui. It treats REAPER's WDL_VWnd
  objects as foreign ABI objects, finds roots whose m_realparent is a SWELL
  HWND, and registers a SWELL custom accessibility provider for them.
*/

#include "swell.h"
#include "swell-internal.h"

#if defined(SWELL_ACCESSKIT) && defined(SWELL_TARGET_GDK)

#include "../wingui/virtwnd-controls.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

struct SWELL_ReaperVWndMapRange
{
  uintptr_t start;
  uintptr_t end;
  bool scan;
  bool reaper;
};

struct SWELL_ReaperVWndMapRanges
{
  SWELL_ReaperVWndMapRange ranges[512];
  int count;
};

struct SWELL_ReaperVWndProvider
{
  SWELL_AccessibilityCustomProvider provider;
  HWND hwnd;
  WDL_VWnd *root;
  DWORD last_scan;
  bool registered;
  WDL_FastString label;

  SWELL_ReaperVWndProvider()
  {
    memset(&provider,0,sizeof(provider));
    hwnd = NULL;
    root = NULL;
    last_scan = 0;
    registered = false;
  }
};

static WDL_PtrList<SWELL_ReaperVWndProvider> s_reaper_vwnd_providers;
static bool s_reaper_vwnd_init;
static bool s_reaper_vwnd_enabled;
static bool s_reaper_vwnd_debug;

static const size_t WDL_VWND_REALPARENT_OFFSETS[] = { 56, 48, 64, 72, 80, 88, 96 };

static bool swell_reaper_vwnd_debug_enabled(void)
{
  return s_reaper_vwnd_debug || getenv("SWELL_ACCESSKIT_DEBUG") != NULL;
}

static void swell_reaper_vwnd_log(const char *fmt, ...)
{
  if (!swell_reaper_vwnd_debug_enabled()) return;
  va_list ap;
  va_start(ap,fmt);
  fprintf(stderr,"SWELL AccessKit REAPER VWnd hack: ");
  vfprintf(stderr,fmt,ap);
  fprintf(stderr,"\n");
  va_end(ap);
}

static const char *swell_reaper_basename(const char *path)
{
  const char *last = path;
  for (const char *p = path; p && *p; ++p)
    if (*p == '/') last = p + 1;
  return last;
}

static bool swell_reaper_vwnd_is_reaper_process(void)
{
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe",exe,sizeof(exe)-1);
  if (len <= 0) return false;
  exe[len] = 0;
  return !strcmp(swell_reaper_basename(exe),"reaper");
}

static bool swell_reaper_vwnd_is_enabled(void)
{
  if (!s_reaper_vwnd_init)
  {
    s_reaper_vwnd_init = true;
    const char *env = getenv("SWELL_ACCESSKIT_REAPER_VWND_HACK");
    s_reaper_vwnd_debug = env && !strcmp(env,"debug");
    s_reaper_vwnd_enabled = env && (!strcmp(env,"1") ||
                                    !strcmp(env,"true") ||
                                    !strcmp(env,"on") ||
                                    !strcmp(env,"debug")) &&
        swell_reaper_vwnd_is_reaper_process();
    if (s_reaper_vwnd_enabled)
      swell_reaper_vwnd_log("enabled");
  }
  return s_reaper_vwnd_enabled;
}

static void swell_reaper_vwnd_read_maps(SWELL_ReaperVWndMapRanges *ranges)
{
  if (!ranges) return;
  ranges->count = 0;

  FILE *fp = fopen("/proc/self/maps","r");
  if (!fp) return;

  char line[4096];
  while (fgets(line,sizeof(line),fp))
  {
    unsigned long long start = 0, end = 0;
    char perms[8] = {0,};
    char path[3072] = {0,};
    if (sscanf(line,"%llx-%llx %7s %*s %*s %*s %3071[^\n]",&start,&end,perms,path) < 3)
      continue;

    const bool readable = perms[0] == 'r';
    const bool writable = perms[1] == 'w';
    const bool private_map = perms[3] == 'p';
    const bool is_heap = strstr(path,"[heap]") != NULL;
    const bool is_anon = path[0] == 0;
    const bool is_reaper = strstr(path,"/reaper") != NULL;

    SWELL_ReaperVWndMapRange r;
    r.start = (uintptr_t)start;
    r.end = (uintptr_t)end;
    r.scan = readable && writable && private_map && (is_heap || is_anon);
    r.reaper = readable && is_reaper;
    if (r.end > r.start && ranges->count < (int)(sizeof(ranges->ranges)/sizeof(ranges->ranges[0])))
      ranges->ranges[ranges->count++] = r;
  }
  fclose(fp);
}

static bool swell_reaper_vwnd_ptr_in_ranges(uintptr_t ptr, size_t bytes, const SWELL_ReaperVWndMapRanges *ranges, bool want_reaper)
{
  if (!ranges) return false;
  if (!ptr || ptr + bytes < ptr) return false;
  for (int i = 0; i < ranges->count; ++i)
  {
    const SWELL_ReaperVWndMapRange &r = ranges->ranges[i];
    if (want_reaper && !r.reaper) continue;
    if (!want_reaper && !r.scan && !r.reaper) continue;
    if (ptr >= r.start && ptr + bytes <= r.end) return true;
  }
  return false;
}

static bool swell_reaper_vwnd_read_mem(int memfd, uintptr_t ptr, void *buf, size_t len)
{
  if (memfd < 0 || !ptr || !buf || !len) return false;
  return pread(memfd,buf,len,(off_t)ptr) == (ssize_t)len;
}

static bool swell_reaper_vwnd_read_ptr(int memfd, uintptr_t ptr, uintptr_t *out)
{
  uintptr_t value = 0;
  if (!out || !swell_reaper_vwnd_read_mem(memfd,ptr,&value,sizeof(value))) return false;
  *out = value;
  return true;
}

static bool swell_reaper_vwnd_read_cstring(int memfd, uintptr_t ptr, const SWELL_ReaperVWndMapRanges *ranges, char *buf, size_t buflen)
{
  if (!buf || buflen < 2 || !swell_reaper_vwnd_ptr_in_ranges(ptr,1,ranges,true)) return false;
  size_t i;
  for (i = 0; i + 1 < buflen; ++i)
  {
    if (!swell_reaper_vwnd_ptr_in_ranges(ptr + i,1,ranges,true)) return false;
    char c = 0;
    if (!swell_reaper_vwnd_read_mem(memfd,ptr + i,&c,sizeof(c))) return false;
    buf[i] = c;
    if (!c) return i > 0;
    if (!isprint((unsigned char)c)) return false;
  }
  buf[i] = 0;
  return false;
}

static bool swell_reaper_vwnd_type_name(int memfd, WDL_VWnd *vwnd, const SWELL_ReaperVWndMapRanges *ranges, char *buf, size_t buflen)
{
  if (!vwnd || !buf || !buflen) return false;
  buf[0] = 0;

  uintptr_t obj = (uintptr_t)vwnd;
  if (!swell_reaper_vwnd_ptr_in_ranges(obj,sizeof(void *),ranges,false)) return false;

  uintptr_t vtable = 0;
  if (!swell_reaper_vwnd_read_ptr(memfd,obj,&vtable)) return false;
  if (!swell_reaper_vwnd_ptr_in_ranges(vtable - 2 * sizeof(void *),2 * sizeof(void *),ranges,true)) return false;

  uintptr_t typeinfo = 0;
  if (!swell_reaper_vwnd_read_ptr(memfd,vtable - sizeof(void *),&typeinfo)) return false;
  if (!swell_reaper_vwnd_ptr_in_ranges(typeinfo + 2 * sizeof(void *),sizeof(void *),ranges,true)) return false;

  uintptr_t name = 0;
  if (!swell_reaper_vwnd_read_ptr(memfd,typeinfo + 2 * sizeof(void *),&name)) return false;
  if (!swell_reaper_vwnd_read_cstring(memfd,name,ranges,buf,buflen)) return false;
  return strstr(buf,"VWnd") != NULL || strstr(buf,"Virtual") != NULL;
}

static bool swell_reaper_vwnd_validate_root_at_offset(int memfd, WDL_VWnd *vwnd, HWND hwnd, size_t realparent_offset, const SWELL_ReaperVWndMapRanges *ranges)
{
  if (!vwnd || !hwnd || !realparent_offset) return false;
  uintptr_t obj = (uintptr_t)vwnd;
  if (!swell_reaper_vwnd_ptr_in_ranges(obj,realparent_offset + sizeof(void *),ranges,false)) return false;
  uintptr_t realparent = 0;
  if (!swell_reaper_vwnd_read_ptr(memfd,obj + realparent_offset,&realparent)) return false;
  if ((HWND)realparent != hwnd) return false;

  char type_name[256];
  if (!swell_reaper_vwnd_type_name(memfd,vwnd,ranges,type_name,sizeof(type_name))) return false;
  return true;
}

static bool swell_reaper_vwnd_validate_root(int memfd, WDL_VWnd *vwnd, HWND hwnd, const SWELL_ReaperVWndMapRanges *ranges)
{
  for (int i = 0; i < (int)(sizeof(WDL_VWND_REALPARENT_OFFSETS)/sizeof(WDL_VWND_REALPARENT_OFFSETS[0])); ++i)
    if (swell_reaper_vwnd_validate_root_at_offset(memfd,vwnd,hwnd,WDL_VWND_REALPARENT_OFFSETS[i],ranges))
      return true;
  return false;
}

static bool swell_reaper_vwnd_validate_any(int memfd, WDL_VWnd *vwnd, const SWELL_ReaperVWndMapRanges *ranges)
{
  char type_name[256];
  return swell_reaper_vwnd_type_name(memfd,vwnd,ranges,type_name,sizeof(type_name));
}

static WDL_VWnd *swell_reaper_vwnd_probe_wm_getobject(HWND hwnd)
{
  WDL_VWnd *root = NULL;
  SendMessage(hwnd,WM_GETOBJECT,0x1001,(LPARAM)&root);
  return root;
}

static WDL_VWnd *swell_reaper_vwnd_probe_hwnd_fields(int memfd, HWND hwnd, const SWELL_ReaperVWndMapRanges *ranges)
{
  if (!hwnd) return NULL;
  uintptr_t candidates[66];
  int count = 0;
  candidates[count++] = (uintptr_t)hwnd->m_userdata;
  candidates[count++] = (uintptr_t)hwnd->m_private_data;
  for (int i = 0; i < 64; ++i)
    candidates[count++] = (uintptr_t)hwnd->m_extra[i];

  for (int i = 0; i < count; ++i)
  {
    WDL_VWnd *candidate = (WDL_VWnd *)candidates[i];
    if (swell_reaper_vwnd_validate_any(memfd,candidate,ranges))
      return candidate;
  }
  return NULL;
}

static bool swell_reaper_vwnd_is_interesting_hwnd(HWND hwnd)
{
  if (!hwnd) return false;
  const char *title = hwnd->m_title.Get();
  if (title && (!strcmp(title,"Main toolbar") || !strcmp(title,"Transport"))) return true;
  return hwnd->m_classname && !strcmp(hwnd->m_classname,"unknown") &&
      (hwnd->m_userdata || hwnd->m_private_data || hwnd->m_extra[0] || hwnd->m_extra[1]);
}

static void swell_reaper_vwnd_log_hwnd_fields(HWND hwnd)
{
  if (!swell_reaper_vwnd_debug_enabled() || !swell_reaper_vwnd_is_interesting_hwnd(hwnd)) return;
  swell_reaper_vwnd_log(
      "hwnd=%p title=%s class=%s wndproc=%p userdata=%p private=%p extra0=%p extra1=%p extra2=%p extra3=%p",
      hwnd,
      hwnd->m_title.Get(),
      hwnd->m_classname ? hwnd->m_classname : "",
      (void *)hwnd->m_wndproc,
      (void *)hwnd->m_userdata,
      (void *)hwnd->m_private_data,
      (void *)hwnd->m_extra[0],
      (void *)hwnd->m_extra[1],
      (void *)hwnd->m_extra[2],
      (void *)hwnd->m_extra[3]);
}

static WDL_VWnd *swell_reaper_vwnd_scan_for_root(int memfd, HWND hwnd, const SWELL_ReaperVWndMapRanges *ranges)
{
  if (!hwnd) return NULL;

  const uintptr_t target = (uintptr_t)hwnd;
  if (!ranges) return NULL;
  unsigned char *buf = (unsigned char *)malloc(65536);
  if (!buf) return NULL;
  for (int i = 0; i < ranges->count; ++i)
  {
    const SWELL_ReaperVWndMapRange &r = ranges->ranges[i];
    if (!r.scan) continue;
    uintptr_t p = (r.start + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    while (p + sizeof(void *) <= r.end)
    {
      size_t want = (size_t)wdl_min((uintptr_t)65536, r.end - p);
      want &= ~(size_t)(sizeof(void *) - 1);
      if (want < sizeof(void *)) break;
      if (pread(memfd,buf,want,(off_t)p) != (ssize_t)want)
      {
        p += want;
        continue;
      }
      for (size_t off = 0; off + sizeof(void *) <= want; off += sizeof(void *))
      {
        if (*(uintptr_t *)(buf + off) == target)
        {
          for (int oi = 0; oi < (int)(sizeof(WDL_VWND_REALPARENT_OFFSETS)/sizeof(WDL_VWND_REALPARENT_OFFSETS[0])); ++oi)
          {
            const size_t realparent_offset = WDL_VWND_REALPARENT_OFFSETS[oi];
            if (p + off < r.start + realparent_offset) continue;
            WDL_VWnd *candidate = (WDL_VWnd *)(p + off - realparent_offset);
            if (swell_reaper_vwnd_validate_root_at_offset(memfd,candidate,hwnd,realparent_offset,ranges))
            {
              free(buf);
              return candidate;
            }
          }
        }
      }
      p += want;
    }
  }
  free(buf);
  return NULL;
}

static uint64_t swell_reaper_vwnd_node_id(WDL_VWnd *vwnd)
{
  const uint64_t id = (uint64_t)(uintptr_t)vwnd & 0xffffffffu;
  return id ? id : 1;
}

static bool swell_reaper_vwnd_is_visible_node(WDL_VWnd *vwnd)
{
  if (!vwnd || !vwnd->IsVisible()) return false;
  RECT r;
  vwnd->GetPosition(&r);
  return r.right > r.left && r.bottom > r.top;
}

static bool swell_reaper_vwnd_is_known_type(const char *type)
{
  return type && (!strcmp(type,"vwnd_iconbutton") ||
                  !strcmp(type,"vwnd_statictext") ||
                  !strcmp(type,"vwnd_slider") ||
                  !strcmp(type,"vwnd_combobox") ||
                  !strcmp(type,"vwnd_tabctrl_child") ||
                  !strcmp(type,"vwnd_tabctrl_proxy") ||
                  !strcmp(type,"vwnd_listbox"));
}

static bool swell_reaper_vwnd_has_text(WDL_VWnd *vwnd)
{
  if (!vwnd) return false;
  const char *desc = vwnd->GetAccessDesc();
  if (desc && *desc) return true;
  const char *type = vwnd->GetType();
  if (!type) return false;
  if (!strcmp(type,"vwnd_iconbutton"))
  {
    const char *label = ((WDL_VirtualIconButton *)vwnd)->GetTextLabel();
    return label && *label;
  }
  if (!strcmp(type,"vwnd_statictext"))
  {
    const char *text = ((WDL_VirtualStaticText *)vwnd)->GetText();
    return text && *text;
  }
  if (!strcmp(type,"vwnd_combobox"))
  {
    WDL_VirtualComboBox *combo = (WDL_VirtualComboBox *)vwnd;
    const char *item = combo->GetItem(combo->GetCurSel());
    return item && *item;
  }
  return false;
}

static bool swell_reaper_vwnd_is_exportable(WDL_VWnd *vwnd);

static bool swell_reaper_vwnd_has_exportable_descendant(WDL_VWnd *vwnd)
{
  if (!vwnd) return false;
  const int count = vwnd->GetNumChildren();
  for (int i = 0; i < count; ++i)
  {
    WDL_VWnd *child = vwnd->EnumChildren(i);
    if (swell_reaper_vwnd_is_exportable(child) || swell_reaper_vwnd_has_exportable_descendant(child)) return true;
  }
  return false;
}

static bool swell_reaper_vwnd_is_exportable(WDL_VWnd *vwnd)
{
  if (!swell_reaper_vwnd_is_visible_node(vwnd)) return false;
  return !vwnd->GetParent() ||
      swell_reaper_vwnd_is_known_type(vwnd->GetType()) ||
      swell_reaper_vwnd_has_text(vwnd) ||
      swell_reaper_vwnd_has_exportable_descendant(vwnd);
}

static int swell_reaper_vwnd_count_exported(WDL_VWnd *vwnd)
{
  if (!vwnd) return 0;
  int count = swell_reaper_vwnd_is_exportable(vwnd) ? 1 : 0;
  const int child_count = vwnd->GetNumChildren();
  for (int i = 0; i < child_count; ++i)
    count += swell_reaper_vwnd_count_exported(vwnd->EnumChildren(i));
  return count;
}

static uint64_t swell_reaper_vwnd_nearest_exported_parent_id(WDL_VWnd *vwnd)
{
  for (WDL_VWnd *parent = vwnd ? vwnd->GetParent() : NULL; parent; parent = parent->GetParent())
  {
    if (swell_reaper_vwnd_is_exportable(parent)) return swell_reaper_vwnd_node_id(parent);
  }
  return 0;
}

static void swell_reaper_vwnd_make_label(SWELL_ReaperVWndProvider *provider, WDL_VWnd *vwnd)
{
  provider->label.Set("");
  if (!vwnd) return;
  const char *type = vwnd->GetType();
  const char *text = NULL;
  const char *desc = vwnd->GetAccessDesc();

  if (type && !strcmp(type,"vwnd_iconbutton"))
    text = ((WDL_VirtualIconButton *)vwnd)->GetTextLabel();
  else if (type && !strcmp(type,"vwnd_statictext"))
    text = ((WDL_VirtualStaticText *)vwnd)->GetText();
  else if (type && !strcmp(type,"vwnd_combobox"))
  {
    WDL_VirtualComboBox *combo = (WDL_VirtualComboBox *)vwnd;
    text = combo->GetItem(combo->GetCurSel());
  }

  if (desc && *desc && text && *text)
  {
    if (type && !strcmp(type,"vwnd_iconbutton"))
      provider->label.SetFormatted(1024,"%.500s %.500s",text,desc);
    else
      provider->label.SetFormatted(1024,"%.500s %.500s",desc,text);
  }
  else if (text && *text)
    provider->label.Set(text);
  else if (desc && *desc)
    provider->label.Set(desc);
}

static int swell_reaper_vwnd_role_for(WDL_VWnd *vwnd)
{
  if (!vwnd) return 0;
  const char *type = vwnd->GetType();
  if (type && !strcmp(type,"vwnd_iconbutton"))
  {
    WDL_VirtualIconButton *button = (WDL_VirtualIconButton *)vwnd;
    if (!button->GetIsButton()) return SWELL_ACCESSIBILITY_ROLE_LABEL;
    return button->GetCheckState() >= 0 ? SWELL_ACCESSIBILITY_ROLE_CHECK_BOX : SWELL_ACCESSIBILITY_ROLE_BUTTON;
  }
  if (type && !strcmp(type,"vwnd_statictext")) return SWELL_ACCESSIBILITY_ROLE_LABEL;
  if (type && !strcmp(type,"vwnd_slider")) return SWELL_ACCESSIBILITY_ROLE_SLIDER;
  if (type && (!strcmp(type,"vwnd_combobox") || !strcmp(type,"vwnd_tabctrl_child")))
    return SWELL_ACCESSIBILITY_ROLE_BUTTON;
  return SWELL_ACCESSIBILITY_ROLE_GROUP;
}

static bool swell_reaper_vwnd_fill_node(SWELL_ReaperVWndProvider *provider, WDL_VWnd *vwnd, SWELL_AccessibilityCustomNode *node)
{
  if (!provider || !vwnd || !node) return false;
  memset(node,0,sizeof(*node));
  node->id = swell_reaper_vwnd_node_id(vwnd);
  node->parent_id = vwnd == provider->root ? 0 : swell_reaper_vwnd_nearest_exported_parent_id(vwnd);
  node->role = swell_reaper_vwnd_role_for(vwnd);
  vwnd->GetPositionInTopVWnd(&node->bounds);

  swell_reaper_vwnd_make_label(provider,vwnd);
  const char *type = vwnd->GetType();
  if (node->role == SWELL_ACCESSIBILITY_ROLE_LABEL)
    node->value = provider->label.Get();
  else
    node->label = provider->label.Get();

  if (type && !strcmp(type,"vwnd_iconbutton"))
  {
    WDL_VirtualIconButton *button = (WDL_VirtualIconButton *)vwnd;
    if (!button->GetEnabled()) node->flags |= SWELL_ACCESSIBILITY_NODE_DISABLED;
    if (button->GetCheckState() > 0) node->flags |= SWELL_ACCESSIBILITY_NODE_CHECKED;
    if (button->GetIsButton()) node->action_mask |= SWELL_ACCESSIBILITY_ACTION_CLICK;
  }
  else if (type && !strcmp(type,"vwnd_slider"))
  {
    node->action_mask |= SWELL_ACCESSIBILITY_ACTION_CLICK;
  }
  else if (type && (!strcmp(type,"vwnd_combobox") ||
                    !strcmp(type,"vwnd_tabctrl_child") ||
                    !strcmp(type,"vwnd_statictext")))
  {
    node->action_mask |= SWELL_ACCESSIBILITY_ACTION_CLICK;
  }
  return node->id && node->role;
}

static bool swell_reaper_vwnd_find_by_index(SWELL_ReaperVWndProvider *provider, WDL_VWnd *vwnd, int target_index, int *index, SWELL_AccessibilityCustomNode *node)
{
  if (!vwnd || !index) return false;
  if (swell_reaper_vwnd_is_exportable(vwnd))
  {
    if (*index == target_index) return swell_reaper_vwnd_fill_node(provider,vwnd,node);
    ++*index;
  }
  const int count = vwnd->GetNumChildren();
  for (int i = 0; i < count; ++i)
    if (swell_reaper_vwnd_find_by_index(provider,vwnd->EnumChildren(i),target_index,index,node))
      return true;
  return false;
}

static WDL_VWnd *swell_reaper_vwnd_find_by_id(WDL_VWnd *vwnd, uint64_t node_id)
{
  if (!vwnd) return NULL;
  if (swell_reaper_vwnd_node_id(vwnd) == node_id) return vwnd;
  const int count = vwnd->GetNumChildren();
  for (int i = 0; i < count; ++i)
  {
    WDL_VWnd *found = swell_reaper_vwnd_find_by_id(vwnd->EnumChildren(i),node_id);
    if (found) return found;
  }
  return NULL;
}

static int swell_reaper_vwnd_get_node_count(const SWELL_AccessibilityCustomProvider *provider)
{
  SWELL_ReaperVWndProvider *p = provider ? (SWELL_ReaperVWndProvider *)provider->user_data : NULL;
  return p && p->root ? swell_reaper_vwnd_count_exported(p->root) : 0;
}

static bool swell_reaper_vwnd_get_node(const SWELL_AccessibilityCustomProvider *provider, int index, SWELL_AccessibilityCustomNode *node)
{
  SWELL_ReaperVWndProvider *p = provider ? (SWELL_ReaperVWndProvider *)provider->user_data : NULL;
  int cur = 0;
  return p && p->root && swell_reaper_vwnd_find_by_index(p,p->root,index,&cur,node);
}

static bool swell_reaper_vwnd_do_action(const SWELL_AccessibilityCustomProvider *provider, uint64_t node_id, int action, const SWELL_AccessibilityCustomActionData *data)
{
  (void)data;
  SWELL_ReaperVWndProvider *p = provider ? (SWELL_ReaperVWndProvider *)provider->user_data : NULL;
  WDL_VWnd *vwnd = p ? swell_reaper_vwnd_find_by_id(p->root,node_id) : NULL;
  if (!vwnd || action != SWELL_ACCESSIBILITY_ACTION_CLICK) return false;
  RECT r;
  vwnd->GetPosition(&r);
  const int x = (r.right - r.left) / 2;
  const int y = (r.bottom - r.top) / 2;
  vwnd->OnMouseDown(x,y);
  vwnd->OnMouseUp(x,y);
  swell_accesskit_notify_custom_provider_changed(p->hwnd);
  return true;
}

static SWELL_ReaperVWndProvider *swell_reaper_vwnd_find_provider(HWND hwnd)
{
  for (int i = 0; i < s_reaper_vwnd_providers.GetSize(); ++i)
  {
    SWELL_ReaperVWndProvider *p = s_reaper_vwnd_providers.Get(i);
    if (p && p->hwnd == hwnd) return p;
  }
  return NULL;
}

static void swell_reaper_vwnd_unregister(SWELL_ReaperVWndProvider *p)
{
  if (!p || !p->registered) return;
  SWELL_AccessibilityCustomProvider provider = p->provider;
  provider.get_node_count = NULL;
  provider.get_node = NULL;
  provider.do_action = NULL;
  swell_accesskit_set_custom_provider(&provider);
  p->registered = false;
}

static void swell_reaper_vwnd_register(SWELL_ReaperVWndProvider *p)
{
  if (!p) return;
  p->provider.version = 1;
  p->provider.hwnd = p->hwnd;
  p->provider.user_data = p;
  p->provider.get_node_count = swell_reaper_vwnd_get_node_count;
  p->provider.get_node = swell_reaper_vwnd_get_node;
  p->provider.do_action = swell_reaper_vwnd_do_action;
  swell_accesskit_set_custom_provider(&p->provider);
  p->registered = true;
}

void swell_accesskit_reaper_vwnd_ensure_provider(HWND hwnd)
{
  if (!hwnd || hwnd->m_hashaddestroy || !swell_reaper_vwnd_is_enabled()) return;

  SWELL_ReaperVWndProvider *p = swell_reaper_vwnd_find_provider(hwnd);
  const DWORD now = GetTickCount();
  if (p && (now - p->last_scan) < 1000) return;

  SWELL_ReaperVWndMapRanges ranges;
  swell_reaper_vwnd_read_maps(&ranges);
  int memfd = open("/proc/self/mem",O_RDONLY);
  if (memfd < 0) return;

  WDL_VWnd *root = swell_reaper_vwnd_probe_wm_getobject(hwnd);
  if (!swell_reaper_vwnd_validate_root(memfd,root,hwnd,&ranges))
    root = swell_reaper_vwnd_probe_hwnd_fields(memfd,hwnd,&ranges);
  if (!swell_reaper_vwnd_validate_any(memfd,root,&ranges))
    root = swell_reaper_vwnd_scan_for_root(memfd,hwnd,&ranges);

  if (!root)
  {
    swell_reaper_vwnd_log_hwnd_fields(hwnd);
    if (!p)
    {
      p = new SWELL_ReaperVWndProvider;
      p->hwnd = hwnd;
      s_reaper_vwnd_providers.Add(p);
    }
    if (p)
    {
      p->last_scan = now;
      p->root = NULL;
      swell_reaper_vwnd_unregister(p);
    }
    close(memfd);
    return;
  }

  if (!p)
  {
    p = new SWELL_ReaperVWndProvider;
    p->hwnd = hwnd;
    s_reaper_vwnd_providers.Add(p);
  }

  p->last_scan = now;
  if (p->root != root || !p->registered)
  {
    p->root = root;
    char type_name[256] = {0,};
    swell_reaper_vwnd_type_name(memfd,root,&ranges,type_name,sizeof(type_name));
    swell_reaper_vwnd_log("register hwnd=%p root=%p type=%s nodes=%d",
        hwnd,root,type_name,swell_reaper_vwnd_count_exported(root));
    swell_reaper_vwnd_register(p);
  }
  close(memfd);
}

#else

void swell_accesskit_reaper_vwnd_ensure_provider(HWND hwnd)
{
  (void)hwnd;
}

#endif
