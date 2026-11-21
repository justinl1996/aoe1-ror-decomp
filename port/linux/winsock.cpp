// Winsock, for the Linux build: multiplayer is not ported, so these fail.
// gethostname and gethostbyname are also glibc functions with another
// calling convention; assembler labels give these stand-ins the names the
// game's (Winsock) calls use without clashing with glibc's declarations.

#include <windows.h>
#include <winsock.h>

extern "C" {

int WINAPI WSAStartup(WORD, LPWSADATA) {
    return WSASYSNOTREADY;
}

int WINAPI WSACleanup(void) {
    return 0;
}

int WINAPI winsock_gethostname(char* name, int size) __asm__("gethostname");
int WINAPI winsock_gethostname(char* name, int size) {
    if (name != 0 && size > 0)
        name[0] = 0;
    return -1;
}

void* WINAPI winsock_gethostbyname(const char*) __asm__("gethostbyname");
void* WINAPI winsock_gethostbyname(const char*) {
    return 0;
}

}
