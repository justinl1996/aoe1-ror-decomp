#include "../include/getdxver.h"
#include <windows.h>

// The game refused to start without DirectX 5.01 (unless run with
// NODXCHECK), and found the version by loading DirectDraw, DirectInput and
// DirectPlay. The SDL build uses none of the system's DirectX (its
// DirectDraw and DirectSound are port/sdl's), so the answer is fixed:
// DirectX 5.01 on Windows NT.
void GetDXVersion(unsigned long* pdwDXVersion, unsigned long* pdwDXPlatform) {
    *pdwDXVersion = 0x501;
    *pdwDXPlatform = VER_PLATFORM_WIN32_NT;
}
