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


#pragma once

#include "swell.h"
#include <stdint.h>

void init_private_xwayland();
void xw_bridge_attach_children(HWND hwnd);
void xw_bridge_detach_children(HWND hwnd);
bool xw_bridge_set_external_gl(HWND hwnd, bool enabled);
HWND xw_bridge_create(HWND parent, void **window, const RECT *rect, const char *bridge_class_name);
bool xw_bridge_forward_key(HWND hwnd, uint32_t keycode, uint32_t keysym, unsigned state, bool down);
bool xw_bridge_key_wanted_by_host(HWND hwnd, uint32_t keycode, unsigned state);
bool xw_bridge_raise_modals();
bool xw_bridge_swell_on_button_event_escape();
bool xw_bridge_popup_blocks_close(HWND hwnd);
bool xw_bridge_close_owned(HWND hwnd);
