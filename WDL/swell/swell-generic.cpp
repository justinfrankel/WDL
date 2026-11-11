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
#include "swell-platform.h"

#define SWELL_PLATFORM_ADDRESS(ret, name, args, call) name##_gdk,
static const SWELL_Platform gdk_platform = {
  SWELL_PLATFORM_API(SWELL_PLATFORM_ADDRESS)
  SWELL_PLATFORM_GUI_API(SWELL_PLATFORM_ADDRESS)
  SWELL_PLATFORM_BITMAP_API(SWELL_PLATFORM_ADDRESS)
};
#undef SWELL_PLATFORM_ADDRESS

bool SWELL_load_gtk()
{
#ifdef SWELL_SUPPORT_GTK
  return SWELL_load_gtk_gdk();
#else
  return false;
#endif
}

#define SWELL_PLATFORM_WRAPPER(ret, name, args, call) \
  ret name args { return gdk_platform.name call; }
SWELL_PLATFORM_API(SWELL_PLATFORM_WRAPPER)
SWELL_PLATFORM_BITMAP_API(SWELL_PLATFORM_WRAPPER)
#ifdef SWELL_TARGET_GDK
SWELL_PLATFORM_GUI_API(SWELL_PLATFORM_WRAPPER)
#endif
#undef SWELL_PLATFORM_WRAPPER
#endif
