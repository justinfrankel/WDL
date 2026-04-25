/*
    swell_myapp

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

#ifdef _WIN32
#include <windows.h>
#include "../WDL/win32_utf8.h"
#endif

#include "../../swell/swell.h"

#include "../../wingui/wndsize.h"

#include "resource.h"

#include <stdio.h>

#if !defined(_WIN32) && !defined(__APPLE__)
bool g_quit;
#endif

HINSTANCE g_hInstance;
HWND g_hwnd;

static int get_slider_pos(HWND hwndDlg)
{
  HWND slider = GetDlgItem(hwndDlg, IDC_SLIDER1);
  return slider ? (int)SendMessage(slider, TBM_GETPOS, 0, 0) : 0;
}

static void set_status(HWND hwndDlg, const char *text)
{
  SetDlgItemText(hwndDlg, IDC_LASTMSG, text ? text : "");
}

WDL_DLGRET mainProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_WndSizer resize;
  switch (uMsg)
  {
    case WM_INITDIALOG:
      {
      g_hwnd=hwndDlg;
#ifdef _WIN32
      {
        HICON icon=LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLongPtr(hwndDlg,GCLP_HICON,(LPARAM)icon);
      }
#endif

      resize.init(hwndDlg);
      resize.init_item(IDC_LASTMSG, 1, 0, 1, 0);
      resize.init_item(IDC_EDIT1, 1, 0, 1, 0);
      resize.init_item(IDC_BUTTON1, 1, 0, 1, 0);
      resize.init_item(IDC_CHECK1, 1, 0, 1, 0);
      resize.init_item(IDC_COMBO_DROPDOWN, 1, 0, 1, 0);
      resize.init_item(IDC_COMBO_EDITABLE, 1, 0, 1, 0);
      resize.init_item(IDC_SLIDER1, 1, 0, 1, 0);
      resize.init_item(IDC_PROGRESS1, 1, 0, 1, 0);
      resize.init_item(IDC_LISTBOX1, 1, 0, 1, 1);
      resize.init_item(IDC_EDIT_MULTILINE, 1, 0, 1, 1);
      resize.init_item(IDC_LISTVIEW1, 1, 0, 1, 1);
      resize.init_item(IDC_TREE1, 1, 0, 1, 1);
      resize.init_item(IDC_TAB1, 1, 0, 1, 1);
      resize.init_item(IDCANCEL,0,1,0,1);

      HMENU menu = LoadMenu(NULL, MAKEINTRESOURCE(IDR_MENU1));
      if (menu)
      {
        SetMenu(hwndDlg, menu);
        EnableMenuItem(menu, ID_SAMPLE_DISABLED, MF_BYCOMMAND | MF_GRAYED);
        CheckMenuItem(menu, ID_SAMPLE_CHECKED, MF_BYCOMMAND | MF_CHECKED);
        CheckMenuItem(menu, ID_SAMPLE_RADIO, MF_BYCOMMAND | MF_CHECKED);
        MENUITEMINFO radio = { sizeof(radio), MIIM_TYPE, MFT_RADIOCHECK };
        SetMenuItemInfo(menu, ID_SAMPLE_RADIO, FALSE, &radio);
        DrawMenuBar(hwndDlg);
      }

      SetDlgItemText(hwndDlg, IDC_EDIT1, "Editable AccessKit text");
      CheckDlgButton(hwndDlg, IDC_CHECK1, 0);

      HWND dropdown = GetDlgItem(hwndDlg, IDC_COMBO_DROPDOWN);
      if (dropdown)
      {
        SendMessage(dropdown, CB_ADDSTRING, 0, (LPARAM)"Alpha");
        SendMessage(dropdown, CB_ADDSTRING, 0, (LPARAM)"Bravo");
        SendMessage(dropdown, CB_ADDSTRING, 0, (LPARAM)"Charlie");
        SendMessage(dropdown, CB_SETCURSEL, 1, 0);
      }

      HWND editable_combo = GetDlgItem(hwndDlg, IDC_COMBO_EDITABLE);
      if (editable_combo)
      {
        SendMessage(editable_combo, CB_ADDSTRING, 0, (LPARAM)"Editable alpha");
        SendMessage(editable_combo, CB_ADDSTRING, 0, (LPARAM)"Editable bravo");
        SendMessage(editable_combo, CB_ADDSTRING, 0, (LPARAM)"Editable charlie");
        SendMessage(editable_combo, CB_SETCURSEL, 0, 0);
      }

      HWND slider = GetDlgItem(hwndDlg, IDC_SLIDER1);
      if (slider)
      {
        SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 10));
        SendMessage(slider, TBM_SETTIC, 0, 5);
        SendMessage(slider, TBM_SETPOS, TRUE, 5);
      }

      HWND progress = GetDlgItem(hwndDlg, IDC_PROGRESS1);
      if (progress)
      {
        SendMessage(progress, PBM_SETRANGE, 0, MAKELONG(0, 10));
        SendMessage(progress, PBM_SETPOS, 5, 0);
      }

      HWND listbox = GetDlgItem(hwndDlg, IDC_LISTBOX1);
      if (listbox)
      {
        SendMessage(listbox, LB_ADDSTRING, 0, (LPARAM)"List alpha");
        SendMessage(listbox, LB_ADDSTRING, 0, (LPARAM)"List bravo");
        SendMessage(listbox, LB_ADDSTRING, 0, (LPARAM)"List charlie");
        SendMessage(listbox, LB_SETCURSEL, 1, 0);
      }

      SetDlgItemText(hwndDlg, IDC_EDIT_MULTILINE, "First line\nSecond line\nThird line");

      HWND listview = GetDlgItem(hwndDlg, IDC_LISTVIEW1);
      if (listview)
      {
        LVCOLUMN col = { LVCF_TEXT | LVCF_WIDTH };
        col.pszText = (char *)"Name";
        col.cx = 72;
        ListView_InsertColumn(listview, 0, &col);
        col.pszText = (char *)"State";
        col.cx = 72;
        ListView_InsertColumn(listview, 1, &col);
        LVITEM item = { LVIF_TEXT | LVIF_STATE };
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        item.iItem = 0;
        item.pszText = (char *)"Row alpha";
        ListView_InsertItem(listview, &item);
        ListView_SetItemText(listview, 0, 1, "Ready");
        item.iItem = 1;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        item.pszText = (char *)"Row bravo";
        ListView_InsertItem(listview, &item);
        ListView_SetItemText(listview, 1, 1, "Selected");
        item.iItem = 2;
        item.state = 0;
        item.pszText = (char *)"Row charlie";
        ListView_InsertItem(listview, &item);
        ListView_SetItemText(listview, 2, 1, "Idle");
      }

      HWND tree = GetDlgItem(hwndDlg, IDC_TREE1);
      if (tree)
      {
        TV_INSERTSTRUCT ins = { 0 };
        ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_CHILDREN;
        ins.item.pszText = (char *)"Parent";
        ins.item.cChildren = 1;
        HTREEITEM parent = TreeView_InsertItem(tree, &ins);
        ins.hParent = parent;
        ins.item.mask = TVIF_TEXT;
        ins.item.pszText = (char *)"Child alpha";
        HTREEITEM child = TreeView_InsertItem(tree, &ins);
        ins.item.pszText = (char *)"Child bravo";
        TreeView_InsertItem(tree, &ins);
        TreeView_Expand(tree, parent, TVE_EXPAND);
        TreeView_SelectItem(tree, child);
      }

      HWND tabs = GetDlgItem(hwndDlg, IDC_TAB1);
      if (tabs)
      {
        TCITEM tab = { TCIF_TEXT };
        tab.pszText = (char *)"One";
        TabCtrl_InsertItem(tabs, 0, &tab);
        tab.pszText = (char *)"Two";
        TabCtrl_InsertItem(tabs, 1, &tab);
        tab.pszText = (char *)"Three";
        TabCtrl_InsertItem(tabs, 2, &tab);
        TabCtrl_SetCurSel(tabs, 0);
      }
      set_status(hwndDlg, "AccessKit demo ready");
      }
    return 1;
    case WM_CLOSE:
      DestroyWindow(hwndDlg);
    return 1;
    case WM_DESTROY:
      g_hwnd=NULL;
#ifdef __APPLE__
      SWELL_PostQuitMessage(0);
#elif defined(_WIN32)
      PostQuitMessage(0);
#else
      g_quit = true;
#endif
    break;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED)
        resize.onResize();
    break;
    case WM_HSCROLL:
      if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_SLIDER1))
      {
        char buf[128];
        sprintf(buf, "Slider value: %d", get_slider_pos(hwndDlg));
        set_status(hwndDlg, buf);
        return 1;
      }
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_BUTTON1:
          {
            char buf[512];
            GetDlgItemText(hwndDlg, IDC_EDIT1, buf, sizeof(buf));
            set_status(hwndDlg, buf[0] ? buf : "Button pressed");
          }
        return 1;
        case IDC_CHECK1:
          set_status(hwndDlg, IsDlgButtonChecked(hwndDlg, IDC_CHECK1) ? "Checkbox checked" : "Checkbox unchecked");
        return 1;
        case IDC_COMBO_DROPDOWN:
        case IDC_COMBO_EDITABLE:
          if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == CBN_EDITCHANGE)
          {
            char buf[256];
            GetWindowText((HWND)lParam, buf, sizeof(buf));
            set_status(hwndDlg, buf);
          }
        return 1;
        case IDC_LISTBOX1:
          if (HIWORD(wParam) == LBN_SELCHANGE)
          {
            char buf[256];
            int sel = (int)SendMessage((HWND)lParam, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && SendMessage((HWND)lParam, LB_GETTEXT, sel, (LPARAM)buf) != LB_ERR)
              set_status(hwndDlg, buf);
          }
        return 1;
        case ID_SAMPLE_HELLO:
          set_status(hwndDlg, "Menu hello");
        return 1;
        case ID_SAMPLE_CHECKED:
          set_status(hwndDlg, "Checked menu item");
        return 1;
        case ID_SAMPLE_RADIO:
          set_status(hwndDlg, "Radio menu item");
        return 1;
        case ID_SAMPLE_NESTED:
          set_status(hwndDlg, "Nested menu item");
        return 1;
        case ID_QUIT:
        case IDCANCEL:
          DestroyWindow(hwndDlg);
        return 1;
      }
    break;
  }
  return 0;
}

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
      {
      }
    break;
    case SWELLAPP_LOADED:
      {
        HWND h=CreateDialog(NULL,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainProc);
        ShowWindow(h,SW_SHOW);
      }
    break;
    case SWELLAPP_DESTROY:
      if (g_hwnd) DestroyWindow(g_hwnd);
    break;
    case SWELLAPP_ONCOMMAND:
      // this is to catch commands coming from the system menu etc
      if (g_hwnd && parm1) SendMessage(g_hwnd,WM_COMMAND,parm1,0);
    break;

  }
  return 0;
}



#ifdef _WIN32

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  g_hInstance = hInstance;

  SWELLAppMain(SWELLAPP_ONLOAD,0,0);
  SWELLAppMain(SWELLAPP_LOADED,0,0);

  for(;;)
  {
    MSG msg={0,};
    int vvv = GetMessage(&msg,NULL,0,0);
    if (!vvv) break;

    if (vvv<0)
    {
      Sleep(10);
      continue;
    }
    if (!msg.hwnd)
    {
      DispatchMessage(&msg);
      continue;
    }
    if (SWELLAppMain(SWELLAPP_PROCESSMESSAGE, (INT_PTR) &msg, 0)) continue;

    if (g_hwnd && IsDialogMessage(g_hwnd,&msg)) continue;

    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  SWELLAppMain(SWELLAPP_DESTROY,0,0);

  ExitProcess(0);
  
  return 0;
}

#else

/************** SWELL stuff ********** */

#ifdef __APPLE__
extern "C" {
#endif

const char **g_argv;
int g_argc;

#ifdef __APPLE__
};
#endif


#ifndef __APPLE__

int main(int argc, const char **argv)
{
  g_argc=argc;
  g_argv=argv;
  SWELL_initargs(&argc,(char***)&argv);
  SWELL_Internal_PostMessage_Init();
  SWELL_ExtendedAPI("APPNAME",(void*)"MyApp");
  SWELLAppMain(SWELLAPP_ONLOAD,0,0);
  SWELLAppMain(SWELLAPP_LOADED,0,0);
  while (!g_quit) {
    SWELL_RunMessageLoop();
    Sleep(10);
  }
  SWELLAppMain(SWELLAPP_DESTROY,0,0);
  return 0;
}

#endif


#include "../../swell/swell-dlggen.h"
#include "res.rc_mac_dlg"
#undef BEGIN
#undef END
#include "../../swell/swell-menugen.h"
#include "res.rc_mac_menu"

#endif
