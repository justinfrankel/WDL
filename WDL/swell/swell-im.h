#ifndef SWELL_IM_H
#define SWELL_IM_H

#include "swell.h"
#include "swell-dlggen.h"
#include "swell-internal.h"
#include "../wdlcstring.h"
#include "../wdlutf8.h"

//-----------------------------------------------------------------------------
// Type Declarations
//-----------------------------------------------------------------------------
class __SWELL_ComboBoxInternalState;
struct __SWELL_ComboBoxInternalState_rec;
struct __SWELL_editControlState;

//-----------------------------------------------------------------------------
// Input Method Core Functions
//-----------------------------------------------------------------------------
void im_init(HWND hwnd);
void im_free(HWND hwnd);
bool im_hwnd_match(HWND hwnd, const char* classname);
void im_update_candidates_location(HWND hwnd);


//-----------------------------------------------------------------------------
// Pre-edit Text Handling
//-----------------------------------------------------------------------------
struct ImPreeditPaintResult {
    int sel1;
    int sel2;
    int cursor_pos;
    WDL_FastString title;
};

ImPreeditPaintResult im_preedit_paint(__SWELL_editControlState *es, HWND hwnd, 
                                     int sel1, int sel2, int cursor_pos);

//-----------------------------------------------------------------------------
// Debug and Diagnostics
//-----------------------------------------------------------------------------
void PrintHWNDProperties(HWND hwnd, const char* prefix);
void PrintWindowStyle(HWND hwnd, const char* windowName);
void PrintOSWindowInfo(HWND hwnd, const char* windowName);
static int getTabControlOffset(HWND hwnd);
#endif
