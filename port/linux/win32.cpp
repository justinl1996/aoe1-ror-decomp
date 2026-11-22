// The Win32 calls the game still makes, for the Linux build. The game's
// window, input, display, text, sound, settings and strings already go
// through port/sdl; what is left here is of three kinds:
//
// - working stand-ins for what the game needs: files and file mapping (the
//   DRS archives), Win16-style file calls, global and local memory, string
//   helpers, time and the startup memory check;
// - out-of-scope features that fail cleanly: multiplayer (DirectPlay and
//   the lobby's registry), intro videos, WinHelp, launching programs, the
//   CD check;
// - calls on code paths that do not run with the SDL layer (window DCs and
//   the GDI bitmap path in Dib.cpp and drawarea.cpp), which fail.
//
// Signatures are Wine's (its headers declare the Win32 API for this build).

#include <windows.h>
#include <vfw.h>
#include <initguid.h>
#include <dplay.h>
#include <dplobby.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "include/msvcrt.h"

#undef open

#ifdef __EMSCRIPTEN__
extern "C" void web_note_file_write(void);  // port/web/main_loop.cpp
#define NOTE_WRITE() web_note_file_write()
#else
#define NOTE_WRITE() ((void) 0)
#endif

extern "C" {

const GUID GUID_NULL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

// Says once per name that the game asked for something the Linux build
// does not do.
static void unsupported(const char* name) {
    static const char* reported[64];
    for (int i = 0; i < 64; ++i) {
        if (reported[i] == name)
            return;
        if (reported[i] == 0) {
            reported[i] = name;
            fprintf(stderr, "empiresx: %s is not supported on Linux\n", name);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Handles: files and file mappings.

enum { HANDLE_FILE = 1, HANDLE_MAPPING };

struct LinuxHandle {
    int kind;
    int fd;
    long size;
};

static LinuxHandle* our_handle(HANDLE handle) {
    if (handle == 0 || handle == INVALID_HANDLE_VALUE)
        return 0;
    return (LinuxHandle*) handle;
}

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE) {
    char real[PATH_MAX];
    int flags;
    if ((access & GENERIC_WRITE) != 0)
        flags = (access & GENERIC_READ) != 0 ? O_RDWR : O_WRONLY;
    else
        flags = O_RDONLY;
    switch (disposition) {
        case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: flags |= O_CREAT; break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
        default: break;  // OPEN_EXISTING
    }
    if ((access & GENERIC_WRITE) != 0)
        NOTE_WRITE();
    int fd = open(linux_resolve_path(name, real, sizeof(real)), flags, 0666);
    if (fd < 0)
        return INVALID_HANDLE_VALUE;
    LinuxHandle* handle = (LinuxHandle*) calloc(1, sizeof(LinuxHandle));
    handle->kind = HANDLE_FILE;
    handle->fd = fd;
    return (HANDLE) handle;
}

HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR) {
    LinuxHandle* source = our_handle(file);
    if (source == 0 || source->kind != HANDLE_FILE)
        return 0;
    struct stat info;
    if (fstat(source->fd, &info) != 0 || info.st_size == 0)
        return 0;
    LinuxHandle* mapping = (LinuxHandle*) calloc(1, sizeof(LinuxHandle));
    mapping->kind = HANDLE_MAPPING;
    mapping->fd = dup(source->fd);  // the game closes the file at once
    mapping->size = (long) info.st_size;
    return (HANDLE) mapping;
}

// The views, for UnmapViewOfFile.
enum { MAX_VIEWS = 64 };
static struct {
    void* address;
    size_t size;
} views[MAX_VIEWS];

LPVOID WINAPI MapViewOfFile(HANDLE mapping, DWORD access, DWORD, DWORD, SIZE_T) {
    LinuxHandle* source = our_handle(mapping);
    if (source == 0 || source->kind != HANDLE_MAPPING)
        return 0;
    int protection = (access & FILE_MAP_WRITE) != 0 ? PROT_READ | PROT_WRITE : PROT_READ;
    void* address = mmap(0, (size_t) source->size, protection, MAP_PRIVATE, source->fd, 0);
    if (address == MAP_FAILED)
        return 0;
    for (int i = 0; i < MAX_VIEWS; ++i)
        if (views[i].address == 0) {
            views[i].address = address;
            views[i].size = (size_t) source->size;
            break;
        }
    return address;
}

BOOL WINAPI UnmapViewOfFile(LPCVOID address) {
    for (int i = 0; i < MAX_VIEWS; ++i)
        if (views[i].address == address && address != 0) {
            munmap(views[i].address, views[i].size);
            views[i].address = 0;
            return TRUE;
        }
    return FALSE;
}

BOOL WINAPI CloseHandle(HANDLE handle) {
    LinuxHandle* ours = our_handle(handle);
    if (ours == 0)
        return FALSE;
    close(ours->fd);
    free(ours);
    return TRUE;
}

// Win16-style file calls: the handle is a file descriptor.
HFILE WINAPI OpenFile(LPCSTR name, OFSTRUCT* info, UINT style) {
    char real[PATH_MAX];
    linux_resolve_path(name, real, sizeof(real));
    if (info != 0) {
        memset(info, 0, sizeof(*info));
        info->cBytes = sizeof(*info);
        snprintf(info->szPathName, sizeof(info->szPathName), "%s", name);
    }
    if ((style & (OF_DELETE | OF_WRITE | OF_READWRITE | OF_CREATE)) != 0)
        NOTE_WRITE();
    if ((style & OF_DELETE) != 0)
        return unlink(real) == 0 ? 1 : HFILE_ERROR;
    int flags = O_RDONLY;
    if ((style & OF_READWRITE) != 0)
        flags = O_RDWR;
    else if ((style & OF_WRITE) != 0)
        flags = O_WRONLY;
    if ((style & OF_CREATE) != 0)
        flags |= O_CREAT | O_TRUNC;
    int fd = open(real, flags, 0666);
    if (fd < 0)
        return HFILE_ERROR;
    if ((style & OF_EXIST) != 0) {
        close(fd);
        return 1;
    }
    return fd;
}

UINT WINAPI _lread(HFILE file, LPVOID buffer, UINT count) {
    ssize_t n = read(file, buffer, count);
    return n < 0 ? (UINT) HFILE_ERROR : (UINT) n;
}

LONG WINAPI _hread(HFILE file, LPVOID buffer, LONG count) {
    ssize_t n = read(file, buffer, (size_t) count);
    return n < 0 ? HFILE_ERROR : (LONG) n;
}

UINT WINAPI _lwrite(HFILE file, LPCSTR buffer, UINT count) {
    ssize_t n = write(file, buffer, count);
    return n < 0 ? (UINT) HFILE_ERROR : (UINT) n;
}

LONG WINAPI _hwrite(HFILE file, LPCSTR buffer, LONG count) {
    ssize_t n = write(file, buffer, (size_t) count);
    return n < 0 ? HFILE_ERROR : (LONG) n;
}

LONG WINAPI _llseek(HFILE file, LONG offset, INT origin) {
    off_t position = lseek(file, offset, origin);
    return position < 0 ? HFILE_ERROR : (LONG) position;
}

HFILE WINAPI _lclose(HFILE file) {
    return close(file) == 0 ? 0 : HFILE_ERROR;
}

UINT WINAPI GetCurrentDirectoryA(UINT size, LPSTR buffer) {
    char path[PATH_MAX];
    if (getcwd(path, sizeof(path)) == 0)
        return 0;
    UINT length = (UINT) strlen(path);
    if (buffer == 0 || size <= length)
        return length + 1;
    strcpy(buffer, path);
    return length;
}

DWORD WINAPI GetTempPathA(DWORD size, LPSTR buffer) {
    const char* folder = getenv("TMPDIR");
    if (folder == 0 || *folder == 0)
        folder = "/tmp";
    DWORD length = (DWORD) strlen(folder) + 1;
    if (buffer == 0 || size <= length)
        return length + 1;
    snprintf(buffer, size, "%s/", folder);
    return length;
}

UINT WINAPI GetTempFileNameA(LPCSTR folder, LPCSTR prefix, UINT unique, LPSTR out) {
    UINT number = unique != 0 ? unique : (UINT) (time(0) & 0xFFFF);
    for (int attempt = 0; attempt < 0x10000; ++attempt, ++number) {
        sprintf(out, "%s%s%.3s%04X.tmp", folder, (folder[0] != 0 && folder[strlen(folder) - 1] != '/' &&
                                                  folder[strlen(folder) - 1] != '\\') ? "/" : "",
                prefix != 0 ? prefix : "", number & 0xFFFF);
        if (unique != 0)
            return unique;
        char real[PATH_MAX];
        int fd = open(linux_resolve_path(out, real, sizeof(real)), O_CREAT | O_EXCL | O_WRONLY, 0666);
        if (fd >= 0) {
            close(fd);
            return number & 0xFFFF;
        }
    }
    return 0;
}

// The CD check: there are no drives. With GAMECD the game does not ask.
UINT WINAPI GetDriveTypeA(LPCSTR) {
    return DRIVE_NO_ROOT_DIR;
}

BOOL WINAPI GetVolumeInformationA(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD) {
    return FALSE;
}

// ---------------------------------------------------------------------------
// Memory: fixed blocks, so a handle is its block's address.

struct MemoryHeader {
    size_t size;
    size_t pad;  // keeps the block 8-byte aligned
};

static void* memory_alloc(size_t size, int zero) {
    MemoryHeader* header = (MemoryHeader*) (zero ? calloc(1, sizeof(MemoryHeader) + size) : malloc(sizeof(MemoryHeader) + size));
    if (header == 0)
        return 0;
    header->size = size;
    return header + 1;
}

static void memory_free(void* block) {
    if (block != 0)
        free((MemoryHeader*) block - 1);
}

static void* memory_realloc(void* block, size_t size, int zero) {
    if (block == 0)
        return memory_alloc(size, zero);
    MemoryHeader* header = (MemoryHeader*) block - 1;
    size_t old_size = header->size;
    header = (MemoryHeader*) realloc(header, sizeof(MemoryHeader) + size);
    if (header == 0)
        return 0;
    if (zero && size > old_size)
        memset((char*) (header + 1) + old_size, 0, size - old_size);
    header->size = size;
    return header + 1;
}

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size) {
    return (HGLOBAL) memory_alloc(size, (flags & GMEM_ZEROINIT) != 0);
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL block, SIZE_T size, UINT flags) {
    return (HGLOBAL) memory_realloc(block, size, (flags & GMEM_ZEROINIT) != 0);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL block) {
    memory_free(block);
    return 0;
}

LPVOID WINAPI GlobalLock(HGLOBAL block) {
    return block;
}

BOOL WINAPI GlobalUnlock(HGLOBAL) {
    return FALSE;  // the lock count reached zero
}

HGLOBAL WINAPI GlobalHandle(LPCVOID block) {
    return (HGLOBAL) block;
}

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T size) {
    return (HLOCAL) memory_alloc(size, (flags & LMEM_ZEROINIT) != 0);
}

HLOCAL WINAPI LocalFree(HLOCAL block) {
    memory_free(block);
    return 0;
}

// The game decommits pages of its mapped resource files when it is done
// with a resource (RESFILE_Decommit_Mapped_Memory). Windows refuses that
// for a mapped view and nothing is released; so here.
BOOL WINAPI VirtualFree(LPVOID, SIZE_T, DWORD) {
    return FALSE;
}

// The game refuses to start with too little memory; report what there is,
// as Windows 9x did, capped to what a DWORD holds.
VOID WINAPI GlobalMemoryStatus(LPMEMORYSTATUS status) {
    unsigned long long page = (unsigned long long) sysconf(_SC_PAGESIZE);
    unsigned long long total = page * (unsigned long long) sysconf(_SC_PHYS_PAGES);
    unsigned long long available = page * (unsigned long long) sysconf(_SC_AVPHYS_PAGES);
    const unsigned long long cap = 0x7FFFFFFFull;
    memset(status, 0, sizeof(*status));
    status->dwLength = sizeof(*status);
    status->dwMemoryLoad = total != 0 ? (DWORD) (100 - available * 100 / total) : 0;
    status->dwTotalPhys = (SIZE_T) (total < cap ? total : cap);
    status->dwAvailPhys = (SIZE_T) (available < cap ? available : cap);
    status->dwTotalPageFile = (SIZE_T) cap;
    status->dwAvailPageFile = (SIZE_T) cap;
    status->dwTotalVirtual = (SIZE_T) cap;
    status->dwAvailVirtual = (SIZE_T) cap;
}

// ---------------------------------------------------------------------------
// Modules and resources: the executable has no Windows resources.

HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
    static int module_token;
    return name == 0 ? (HMODULE) &module_token : 0;
}

HRSRC WINAPI FindResourceA(HMODULE, LPCSTR, LPCSTR) {
    return 0;
}

HGLOBAL WINAPI LoadResource(HMODULE, HRSRC) {
    return 0;
}

LPVOID WINAPI LockResource(HGLOBAL) {
    return 0;
}

// ---------------------------------------------------------------------------
// Strings and numbers

// Upper case in Windows-1252: ASCII letters and the accented ones.
static unsigned char upper_1252(unsigned char c) {
    if (c >= 'a' && c <= 'z')
        return (unsigned char) (c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (unsigned char) (c - 32);
    if (c == 0x9A || c == 0x9C || c == 0x9E)  // š œ ž
        return (unsigned char) (c - 16);
    if (c == 0xFF)  // ÿ
        return 0x9F;
    return c;
}

LPSTR WINAPI CharUpperA(LPSTR text) {
    // A pointer below 0x10000 is a character, as in Windows.
    if (((ULONG_PTR) text >> 16) == 0)
        return (LPSTR) (ULONG_PTR) upper_1252((unsigned char) (ULONG_PTR) text);
    for (unsigned char* p = (unsigned char*) text; *p != 0; ++p)
        *p = upper_1252(*p);
    return text;
}

// Ordinal comparison (with NORM_IGNORECASE, of upper-cased characters).
// Windows' comparison was linguistic; for the game's lists of names the
// order is the same.
INT WINAPI CompareStringA(LCID, DWORD flags, LPCSTR a, INT a_length, LPCSTR b, INT b_length) {
    if (a == 0 || b == 0)
        return 0;
    if (a_length < 0)
        a_length = (INT) strlen(a);
    if (b_length < 0)
        b_length = (INT) strlen(b);
    INT n = a_length < b_length ? a_length : b_length;
    for (INT i = 0; i < n; ++i) {
        unsigned char x = (unsigned char) a[i];
        unsigned char y = (unsigned char) b[i];
        if ((flags & NORM_IGNORECASE) != 0) {
            x = upper_1252(x);
            y = upper_1252(y);
        }
        if (x != y)
            return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    if (a_length == b_length)
        return CSTR_EQUAL;
    return a_length < b_length ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

BOOL WINAPI IsDBCSLeadByte(BYTE) {
    return FALSE;
}

INT WINAPI MulDiv(INT a, INT b, INT c) {
    if (c == 0)
        return -1;
    long long product = (long long) a * b;
    // Rounded half away from zero, as Windows does.
    long long half = (c < 0 ? -c : c) / 2;
    long long result = (product < 0) != (c < 0) ? (product - half) / c : (product + half) / c;
    if (result > INT_MAX || result < INT_MIN)
        return -1;
    return (INT) result;
}

BOOL WINAPI SetRect(LPRECT rect, INT left, INT top, INT right, INT bottom) {
    if (rect == 0)
        return FALSE;
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
    return TRUE;
}

BOOL WINAPI SetRectEmpty(LPRECT rect) {
    return SetRect(rect, 0, 0, 0, 0);
}

DWORD WINAPI GetTickCount(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (DWORD) (now.tv_sec * 1000ull + now.tv_nsec / 1000000);
}

VOID WINAPI OutputDebugStringA(LPCSTR text) {
    if (getenv("AOE_DEBUG_OUTPUT") != 0 && text != 0)
        fputs(text, stderr);
}

// ---------------------------------------------------------------------------
// Windows other than the game's (the SDL layer owns that one)

// The arrow, hourglass and other system cursors are told apart by these
// values (port/sdl/window.cpp compares them).
HCURSOR WINAPI LoadCursorA(HINSTANCE instance, LPCSTR name) {
    if (instance != 0)
        return 0;  // a cursor resource: there are none
    return (HCURSOR) name;
}

LRESULT WINAPI CallWindowProcA(WNDPROC proc, HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return proc != 0 ? proc(wnd, msg, wparam, lparam) : 0;
}

BOOL WINAPI GetClientRect(HWND, LPRECT rect) {
    SetRectEmpty(rect);
    return FALSE;
}

LONG WINAPI GetWindowLongA(HWND, INT) {
    return 0;
}

LONG WINAPI SetWindowLongA(HWND, INT, LONG) {
    return 0;
}

HWND WINAPI GetParent(HWND) {
    return 0;
}

BOOL WINAPI MoveWindow(HWND, INT, INT, INT, INT, BOOL) {
    return FALSE;
}

LRESULT WINAPI SendMessageA(HWND, UINT, WPARAM, LPARAM) {
    return 0;
}

HWND WINAPI FindWindowA(LPCSTR, LPCSTR) {
    return 0;
}

INT WINAPI GetWindowTextA(HWND, LPSTR text, INT size) {
    if (text != 0 && size > 0)
        text[0] = 0;
    return 0;
}

BOOL WINAPI WinHelpA(HWND, LPCSTR, UINT, ULONG_PTR) {
    unsupported("Help (WinHelp)");
    return FALSE;
}

UINT WINAPI WinExec(LPCSTR command, UINT) {
    unsupported("Running other programs (WinExec)");
    (void) command;
    return 0;  // out of memory: the call failed
}

// ---------------------------------------------------------------------------
// GDI calls on paths the SDL layer does not take

HDC WINAPI GetDC(HWND) {
    return 0;
}

INT WINAPI ReleaseDC(HWND, HDC) {
    return 0;
}

HDC WINAPI CreateCompatibleDC(HDC) {
    return 0;
}

BOOL WINAPI DeleteDC(HDC) {
    return FALSE;
}

HGDIOBJ WINAPI GetStockObject(INT) {
    return 0;
}

INT WINAPI GetObjectA(HGDIOBJ, INT, LPVOID) {
    return 0;
}

INT WINAPI GetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT) {
    return 0;
}

INT WINAPI SetStretchBltMode(HDC, INT) {
    return 0;
}

INT WINAPI StretchDIBits(HDC, INT, INT, INT, INT, INT, INT, INT, INT, const VOID*, const BITMAPINFO*, UINT, DWORD) {
    return 0;
}

// ---------------------------------------------------------------------------
// Out of scope: the registry (the multiplayer lobby's keys), intro videos,
// DirectPlay.

LSTATUS WINAPI RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY key, LPDWORD) {
    if (key != 0)
        *key = 0;
    return ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) {
    return ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI RegCloseKey(HKEY) {
    return ERROR_SUCCESS;
}

// No video codec: the game skips its videos (TRIBE_Game::start_video).
BOOL VFWAPI ICInfo(DWORD, DWORD, ICINFO*) {
    return FALSE;
}

HWND VFWAPIV MCIWndCreateA(HWND, HINSTANCE, DWORD, LPCSTR) {
    return 0;
}

HRESULT WINAPI DirectPlayCreate(LPGUID, LPDIRECTPLAY* out, IUnknown*) {
    unsupported("Multiplayer (DirectPlay)");
    if (out != 0)
        *out = 0;
    return DPERR_UNAVAILABLE;
}

HRESULT WINAPI DirectPlayEnumerateA(LPDPENUMDPCALLBACKA, LPVOID) {
    unsupported("Multiplayer (DirectPlay)");
    return DPERR_UNAVAILABLE;
}

HRESULT WINAPI DirectPlayLobbyCreateA(LPGUID, LPDIRECTPLAYLOBBYA* out, IUnknown*, LPVOID, DWORD) {
    unsupported("Multiplayer (DirectPlay lobby)");
    if (out != 0)
        *out = 0;
    return DPERR_UNAVAILABLE;
}

}  // extern "C"
