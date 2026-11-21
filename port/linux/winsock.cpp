// Winsock, for the Linux build: multiplayer is not ported, so these fail.
// gethostname and gethostbyname are renamed by aoe_linux.h (glibc has
// functions of those names, with another calling convention).

#include <windows.h>
#include <winsock.h>

extern "C" {

int WINAPI WSAStartup(WORD, LPWSADATA) {
    return WSASYSNOTREADY;
}

int WINAPI WSACleanup(void) {
    return 0;
}

int WINAPI gethostname(char* name, int size) {
    if (name != 0 && size > 0)
        name[0] = 0;
    return -1;
}

struct hostent* WINAPI gethostbyname(const char*) {
    return 0;
}

}
