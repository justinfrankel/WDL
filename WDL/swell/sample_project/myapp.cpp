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
      resize.init_item(IDC_SLIDER1, 1, 0, 1, 0);
      resize.init_item(IDCANCEL,0,1,0,1);

      SetDlgItemText(hwndDlg, IDC_EDIT1, "Editable AccessKit text");
      CheckDlgButton(hwndDlg, IDC_CHECK1, 0);

      HWND slider = GetDlgItem(hwndDlg, IDC_SLIDER1);
      if (slider)
      {
        SendMessage(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 10));
        SendMessage(slider, TBM_SETTIC, 0, 5);
        SendMessage(slider, TBM_SETPOS, TRUE, 5);
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
