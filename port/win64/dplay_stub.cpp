// 64-bit Windows DirectPlay stubs: legacy DirectPlay (dplayx.dll) is 32-bit only.
// Single-player runs natively; DirectPlay calls fail cleanly.
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>

extern "C" {

HRESULT WINAPI DirectPlayCreate(LPGUID, LPDIRECTPLAY* out, IUnknown*) {
    if (out != 0)
        *out = 0;
    return E_FAIL;
}

HRESULT WINAPI DirectPlayEnumerateA(LPDPENUMDPCALLBACKA, LPVOID) {
    return E_FAIL;
}

HRESULT WINAPI DirectPlayLobbyCreateA(LPGUID, LPDIRECTPLAYLOBBYA* out, IUnknown*, LPVOID, DWORD) {
    if (out != 0)
        *out = 0;
    return E_FAIL;
}

}
