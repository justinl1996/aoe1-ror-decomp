/*
 * Forced first include of every source in the Linux build
 * (tools/build_linux.py). The game was written against Visual C++ and the
 * Win32 headers; on Linux the Win32 declarations come from Wine's headers
 * (winelib, used for their declarations only) and the C runtime is glibc's,
 * with the Microsoft runtime's extras in msvcrt.h.
 */
#ifndef AOE_LINUX_H
#define AOE_LINUX_H

#define AOE_LINUX 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game headers forward-declare FILE as Microsoft's struct _iobuf. */
#define _iobuf _IO_FILE

/* The game uses no min/max macros; without them the C++ library's
   headers stay usable. */
#define NOMINMAX

/* The build defines NO_STRICT (handles are plain pointers, as in the
   Windows build). Wine's winnls.h then leaves out one callback type it
   uses itself. */
#include <windef.h>
typedef FARPROC LOCALE_ENUMPROCEX;

/* Wine's headers expect windows.h before any other Win32 header. */
#include <windows.h>

/* Struct tags the VC6 headers had and Wine's lack. */
#define tagBITMAPINFOHEADER BITMAPINFOHEADER

#include "msvcrt.h"

#ifdef __cplusplus
/* VC6's NO_STRICT signature, which the game's code was written for. */
static inline LRESULT CallWindowProcA(FARPROC proc, HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return CallWindowProcA((WNDPROC) proc, wnd, msg, wparam, lparam);
}
#endif

#endif
