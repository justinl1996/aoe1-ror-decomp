// The Microsoft C runtime's extras for the Linux build (declared in
// port/linux/include/msvcrt.h): Windows file names on a case-sensitive file
// system, _findfirst, folders, and the string functions glibc lacks.

#include "include/msvcrt.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>

// fopen, open and the rest are macros for the linux_ functions; here they
// mean glibc's.
#undef fopen
#undef open
#undef unlink
#undef remove
#undef rename
#undef access

char* _pgmptr = (char*) "empiresx";

#ifdef __EMSCRIPTEN__
extern "C" void web_note_file_write(void);  // port/web/main_loop.cpp
#define NOTE_WRITE() web_note_file_write()
#else
#define NOTE_WRITE() ((void) 0)
#endif

// ---------------------------------------------------------------------------
// File names

// Finds name in folder ignoring letter case; copies the real name to out.
static int match_in_folder(const char* folder, const char* name, char* out, size_t size) {
    DIR* dir = opendir(folder[0] != 0 ? folder : ".");
    if (dir == 0)
        return 0;
    int found = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != 0) {
        if (strcasecmp(entry->d_name, name) == 0) {
            snprintf(out, size, "%s", entry->d_name);
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

char* linux_resolve_path(const char* path, char* out, size_t size) {
    char work[PATH_MAX];
    if (path == 0) {
        out[0] = 0;
        return out;
    }
    // Backslashes, a drive letter, doubled separators.
    size_t n = 0;
    const char* p = path;
    int dropped_drive = 0;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        p += 2;
        dropped_drive = 1;
    }
    for (; *p != 0 && n < sizeof(work) - 1; ++p) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/' && n > 0 && work[n - 1] == '/')
            continue;
        work[n++] = c;
    }
    work[n] = 0;
    // "C:\file" meant the root of a Windows drive: here it is relative.
    char* rest = work;
    if (dropped_drive)
        while (*rest == '/')
            ++rest;

    // Each component as it is, or else its case-insensitive match.
    char result[PATH_MAX];
    size_t length = 0;
    result[0] = 0;
    if (*rest == '/') {
        result[0] = '/';
        result[1] = 0;
        length = 1;
        ++rest;
    }
    int exists = 1;
    while (*rest != 0) {
        char* slash = strchr(rest, '/');
        size_t part = slash != 0 ? (size_t) (slash - rest) : strlen(rest);
        char name[NAME_MAX + 1];
        if (part > NAME_MAX)
            part = NAME_MAX;
        memcpy(name, rest, part);
        name[part] = 0;
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s%s", result, name);
        struct stat info;
        if (exists && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && lstat(candidate, &info) != 0) {
            char real[NAME_MAX + 1];
            if (match_in_folder(length > 0 ? result : ".", name, real, sizeof(real)))
                snprintf(name, sizeof(name), "%s", real);
            else
                exists = 0;  // a new file or folder: the rest stays as given
        }
        length = strlen(result);
        snprintf(result + length, sizeof(result) - length, "%s%s", name, slash != 0 ? "/" : "");
        length = strlen(result);
        if (slash == 0)
            break;
        rest = slash + 1;
    }
    snprintf(out, size, "%s", result);
    return out;
}

int linux_open(const char* path, int flags, ...) {
    char real[PATH_MAX];
    int mode = 0644;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        int requested = va_arg(args, int);
        va_end(args);
        // _S_IREAD | _S_IWRITE: readable and writable by everyone the
        // umask allows.
        mode = (requested & 0200) != 0 ? 0666 : 0444;
    }
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0)
        NOTE_WRITE();
    return open(linux_resolve_path(path, real, sizeof(real)), flags, mode);
}

FILE* linux_fopen(const char* path, const char* mode) {
    char real[PATH_MAX];
    char clean[8];
    size_t n = 0;
    // Microsoft's 't' (text mode) has no meaning here.
    for (const char* m = mode; *m != 0 && n < sizeof(clean) - 1; ++m)
        if (*m != 't')
            clean[n++] = *m;
    clean[n] = 0;
    if (strpbrk(clean, "wa+") != 0)
        NOTE_WRITE();
    return fopen(linux_resolve_path(path, real, sizeof(real)), clean);
}

int linux_unlink(const char* path) {
    NOTE_WRITE();
    char real[PATH_MAX];
    return unlink(linux_resolve_path(path, real, sizeof(real)));
}

int linux_remove(const char* path) {
    NOTE_WRITE();
    char real[PATH_MAX];
    return remove(linux_resolve_path(path, real, sizeof(real)));
}

int linux_rename(const char* from, const char* to) {
    NOTE_WRITE();
    char real_from[PATH_MAX];
    char real_to[PATH_MAX];
    return rename(linux_resolve_path(from, real_from, sizeof(real_from)),
                  linux_resolve_path(to, real_to, sizeof(real_to)));
}

int linux_access(const char* path, int mode) {
    char real[PATH_MAX];
    // Microsoft's mode 0 is "exists", 2 write, 4 read, 6 both: as POSIX.
    return access(linux_resolve_path(path, real, sizeof(real)), mode);
}

long linux_tell(int fd) {
    return (long) lseek(fd, 0, SEEK_CUR);
}

long linux_filelength(int fd) {
    struct stat info;
    if (fstat(fd, &info) != 0)
        return -1;
    return (long) info.st_size;
}

int linux_stat_buf(const char* path, struct linux_stat_buf* buf) {
    char real[PATH_MAX];
    struct stat info;
    if (stat(linux_resolve_path(path, real, sizeof(real)), &info) != 0)
        return -1;
    memset(buf, 0, sizeof(*buf));
    buf->st_dev = (unsigned int) info.st_dev;
    buf->st_ino = (unsigned short) info.st_ino;
    // Microsoft's mode bits: folder, regular file, read, write.
    buf->st_mode = (unsigned short) ((S_ISDIR(info.st_mode) ? 0040000 : 0) | (S_ISREG(info.st_mode) ? 0100000 : 0) |
                                     (info.st_mode & 0700));
    buf->st_nlink = (short) info.st_nlink;
    buf->st_size = (long) info.st_size;
#ifdef __APPLE__
    buf->st_atimespec = info.st_atimespec;
    buf->st_mtimespec = info.st_mtimespec;
    buf->st_ctimespec = info.st_ctimespec;
#else
    buf->st_atim = info.st_atim;
    buf->st_mtim = info.st_mtim;
    buf->st_ctim = info.st_ctim;
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// _findfirst

enum { FIND_HANDLES = 32 };

struct FindState {
    int used;
    char folder[PATH_MAX];
    char pattern[NAME_MAX + 1];
    DIR* dir;
};

static FindState find_states[FIND_HANDLES];

static int find_fill(FindState* state, struct _finddata_t* data) {
    struct dirent* entry;
    while ((entry = readdir(state->dir)) != 0) {
        if (fnmatch(state->pattern, entry->d_name, FNM_CASEFOLD) != 0)
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", state->folder, entry->d_name);
        struct stat info;
        if (stat(full, &info) != 0)
            continue;
        memset(data, 0, sizeof(*data));
        snprintf(data->name, sizeof(data->name), "%s", entry->d_name);
        data->attrib = S_ISDIR(info.st_mode) ? _A_SUBDIR : _A_ARCH;
        if ((info.st_mode & 0200) == 0)
            data->attrib |= _A_RDONLY;
        data->size = (unsigned long) info.st_size;
        data->time_create = info.st_ctime;
        data->time_access = info.st_atime;
        data->time_write = info.st_mtime;
        return 1;
    }
    return 0;
}

long _findfirst(const char* pattern, struct _finddata_t* data) {
    char real[PATH_MAX];
    linux_resolve_path(pattern, real, sizeof(real));
    int slot = -1;
    for (int i = 0; i < FIND_HANDLES; ++i)
        if (find_states[i].used == 0) {
            slot = i;
            break;
        }
    if (slot < 0) {
        errno = ENOMEM;
        return -1;
    }
    FindState* state = &find_states[slot];
    char* slash = strrchr(real, '/');
    if (slash != 0) {
        *slash = 0;
        snprintf(state->folder, sizeof(state->folder), "%s", real[0] != 0 ? real : "/");
        snprintf(state->pattern, sizeof(state->pattern), "%s", slash + 1);
    } else {
        snprintf(state->folder, sizeof(state->folder), ".");
        snprintf(state->pattern, sizeof(state->pattern), "%s", real);
    }
    // "*.*" matched every name in Windows, with or without a dot.
    if (strcmp(state->pattern, "*.*") == 0)
        snprintf(state->pattern, sizeof(state->pattern), "*");
    state->dir = opendir(state->folder);
    if (state->dir == 0) {
        errno = ENOENT;
        return -1;
    }
    state->used = 1;
    if (find_fill(state, data) == 0) {
        closedir(state->dir);
        state->used = 0;
        errno = ENOENT;
        return -1;
    }
    return slot;
}

int _findnext(long handle, struct _finddata_t* data) {
    if (handle < 0 || handle >= FIND_HANDLES || find_states[handle].used == 0)
        return -1;
    return find_fill(&find_states[handle], data) != 0 ? 0 : -1;
}

int _findclose(long handle) {
    if (handle < 0 || handle >= FIND_HANDLES || find_states[handle].used == 0)
        return -1;
    closedir(find_states[handle].dir);
    find_states[handle].used = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Folders

char* _getcwd(char* buffer, int size) {
    return getcwd(buffer, (size_t) size);
}

int _chdir(const char* path) {
    char real[PATH_MAX];
    return chdir(linux_resolve_path(path, real, sizeof(real)));
}

int _mkdir(const char* path) {
    NOTE_WRITE();
    char real[PATH_MAX];
    return mkdir(linux_resolve_path(path, real, sizeof(real)), 0777);
}

int _rmdir(const char* path) {
    NOTE_WRITE();
    char real[PATH_MAX];
    return rmdir(linux_resolve_path(path, real, sizeof(real)));
}

// There are no drives: everything is on "C:".
int _chdrive(int) {
    return 0;
}

int _getdrive(void) {
    return 3;
}

void _splitpath(const char* path, char* drive, char* dir, char* name, char* ext) {
    if (drive != 0)
        drive[0] = 0;
    if (path != 0 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        if (drive != 0) {
            drive[0] = path[0];
            drive[1] = ':';
            drive[2] = 0;
        }
        path += 2;
    }
    const char* last_slash = 0;
    for (const char* p = path; *p != 0; ++p)
        if (*p == '/' || *p == '\\')
            last_slash = p;
    const char* base = last_slash != 0 ? last_slash + 1 : path;
    if (dir != 0) {
        size_t n = (size_t) (base - path);
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    const char* dot = strrchr(base, '.');
    if (dot == 0)
        dot = base + strlen(base);
    if (name != 0) {
        size_t n = (size_t) (dot - base);
        memcpy(name, base, n);
        name[n] = 0;
    }
    if (ext != 0)
        strcpy(ext, dot);
}

void _makepath(char* path, const char* drive, const char* dir, const char* name, const char* ext) {
    path[0] = 0;
    if (drive != 0 && drive[0] != 0)
        strcat(path, drive);
    if (dir != 0 && dir[0] != 0) {
        strcat(path, dir);
        char last = dir[strlen(dir) - 1];
        if (last != '/' && last != '\\')
            strcat(path, "\\");
    }
    if (name != 0)
        strcat(path, name);
    if (ext != 0 && ext[0] != 0) {
        if (ext[0] != '.')
            strcat(path, ".");
        strcat(path, ext);
    }
}

// ---------------------------------------------------------------------------
// rand: the Microsoft C runtime's linear congruential generator.

static unsigned int msvc_rand_state = 1;

int linux_msvc_rand(void) {
    msvc_rand_state = msvc_rand_state * 214013u + 2531011u;
    return (int) ((msvc_rand_state >> 16) & 0x7FFF);
}

void linux_msvc_srand(unsigned int seed) {
    msvc_rand_state = seed;
}

// ---------------------------------------------------------------------------
// Strings

char* _strupr(char* s) {
    for (char* p = s; *p != 0; ++p)
        *p = (char) toupper((unsigned char) *p);
    return s;
}

char* _strlwr(char* s) {
    for (char* p = s; *p != 0; ++p)
        *p = (char) tolower((unsigned char) *p);
    return s;
}

char* _strrev(char* s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; ++i) {
        char c = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = c;
    }
    return s;
}

static char* unsigned_to_text(unsigned long value, char* buffer, int radix, int negative) {
    char digits[40];
    int n = 0;
    if (radix < 2 || radix > 36)
        radix = 10;
    do {
        int d = (int) (value % (unsigned long) radix);
        digits[n++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
        value /= (unsigned long) radix;
    } while (value != 0);
    char* out = buffer;
    if (negative)
        *out++ = '-';
    while (n > 0)
        *out++ = digits[--n];
    *out = 0;
    return buffer;
}

// Microsoft's itoa: a minus sign only in base 10; other bases show the
// two's complement.
char* _itoa(int value, char* buffer, int radix) {
    if (radix == 10 && value < 0)
        return unsigned_to_text((unsigned long) (-(long) value), buffer, radix, 1);
    return unsigned_to_text((unsigned int) value, buffer, radix, 0);
}

char* _ltoa(long value, char* buffer, int radix) {
    if (radix == 10 && value < 0)
        return unsigned_to_text((unsigned long) (-value), buffer, radix, 1);
    return unsigned_to_text((unsigned long) value, buffer, radix, 0);
}

char* _ultoa(unsigned long value, char* buffer, int radix) {
    return unsigned_to_text(value, buffer, radix, 0);
}

// ---------------------------------------------------------------------------
// Multibyte strings: single-byte (Windows-1252) text.

size_t _mbslen(const unsigned char* s) {
    return strlen((const char*) s);
}

unsigned char* _mbsinc(const unsigned char* p) {
    return (unsigned char*) p + 1;
}

unsigned char* _mbsninc(const unsigned char* p, size_t count) {
    if (p == 0)
        return 0;
    size_t length = strlen((const char*) p);
    return (unsigned char*) p + (count < length ? count : length);
}

unsigned char* _mbsdec(const unsigned char* start, const unsigned char* p) {
    if (start == 0 || p == 0 || p <= start)
        return 0;
    return (unsigned char*) p - 1;
}

unsigned char* _mbsncpy(unsigned char* dst, const unsigned char* src, size_t count) {
    return (unsigned char*) strncpy((char*) dst, (const char*) src, count);
}

unsigned char* _mbsncat(unsigned char* dst, const unsigned char* src, size_t count) {
    return (unsigned char*) strncat((char*) dst, (const char*) src, count);
}

unsigned char* _mbscpy(unsigned char* dst, const unsigned char* src) {
    return (unsigned char*) strcpy((char*) dst, (const char*) src);
}

unsigned char* _mbscat(unsigned char* dst, const unsigned char* src) {
    return (unsigned char*) strcat((char*) dst, (const char*) src);
}

unsigned char* _mbsstr(const unsigned char* s, const unsigned char* find) {
    return (unsigned char*) strstr((const char*) s, (const char*) find);
}

unsigned char* _mbschr(const unsigned char* s, unsigned int c) {
    return (unsigned char*) strchr((const char*) s, (int) c);
}

unsigned char* _mbsrchr(const unsigned char* s, unsigned int c) {
    return (unsigned char*) strrchr((const char*) s, (int) c);
}

int _mbscmp(const unsigned char* a, const unsigned char* b) {
    return strcmp((const char*) a, (const char*) b);
}

int _mbsicmp(const unsigned char* a, const unsigned char* b) {
    return strcasecmp((const char*) a, (const char*) b);
}

unsigned char* _mbsupr(unsigned char* s) {
    return (unsigned char*) _strupr((char*) s);
}

unsigned char* _mbslwr(unsigned char* s) {
    return (unsigned char*) _strlwr((char*) s);
}

int _ismbcspace(unsigned int c) {
    return c < 0x100 && isspace((int) c);
}

int _ismbblead(unsigned int) {
    return 0;
}
