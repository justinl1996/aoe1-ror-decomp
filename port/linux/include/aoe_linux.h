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

#include <unistd.h>

/* Winsock's gethostname and gethostbyname have glibc namesakes with another
   calling convention. Under their own names the game's calls reach
   port/linux/winsock.cpp, and libraries calling glibc's (libxcb does) are
   not handed the Winsock ones. */
#define gethostname aoe_winsock_gethostname
#define gethostbyname aoe_winsock_gethostbyname

/* Game headers forward-declare FILE as Microsoft's struct _iobuf. */
#ifdef __APPLE__
#define _iobuf __sFILE
#else
#define _iobuf _IO_FILE
#endif

/* The game uses no min/max macros; without them the C++ library's
   headers stay usable. */
#define NOMINMAX

/* On 64-bit Unix/Wasm64, use the native ABI instead of Wine's ms_abi for __stdcall/__cdecl. */
#if defined(__LP64__) || defined(__aarch64__) || defined(__wasm64__)
#define WINE_UNIX_LIB 1
#endif

/* The build defines NO_STRICT (handles are plain pointers, as in the
   Windows build). Wine's winnls.h then leaves out one callback type it
   uses itself. */
#include <windef.h>
typedef FARPROC LOCALE_ENUMPROCEX;

/* Wine's headers expect windows.h before any other Win32 header. */
#include <windows.h>

/* Wine's winsock.h leaves gethostname out on Unix. */
#ifdef __cplusplus
extern "C"
#endif
int WINAPI gethostname(char* name, int size);

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
