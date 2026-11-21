/*
 * The Microsoft C runtime's extras the game uses, for the Linux build
 * (implemented in port/linux/crt.cpp; included by aoe_linux.h).
 *
 * File names: the game writes Windows paths (backslashes, any letter
 * case, sometimes a drive letter). Every call here that takes a file name
 * resolves it first (linux_resolve_path): '\' becomes '/', a drive letter
 * is dropped, and each path component matches an existing file or folder
 * case-insensitively. fopen, open, unlink, remove, rename and access are
 * redirected here too.
 *
 * Multibyte strings (_mbs*): the game is English only (Windows-1252), so
 * they are the single-byte string functions.
 */
#ifndef AOE_MSVCRT_H
#define AOE_MSVCRT_H

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Path resolution. Returns out. */
char* linux_resolve_path(const char* path, char* out, size_t size);

/* Low-level I/O (io.h). */
#define O_BINARY 0
#define O_TEXT 0
#define _O_BINARY 0
#define _O_TEXT 0
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _O_EXCL O_EXCL
#define _S_IREAD 0400
#define _S_IWRITE 0200
#define S_IREAD 0400
#define S_IWRITE 0200

int linux_open(const char* path, int flags, ...);
FILE* linux_fopen(const char* path, const char* mode);
int linux_unlink(const char* path);
int linux_remove(const char* path);
int linux_rename(const char* from, const char* to);
int linux_access(const char* path, int mode);
long linux_tell(int fd);
long linux_filelength(int fd);

#define _open linux_open
#define open(...) linux_open(__VA_ARGS__)
#define fopen(path, mode) linux_fopen(path, mode)
#define _unlink linux_unlink
#define unlink(path) linux_unlink(path)
#define remove(path) linux_remove(path)
#define rename(from, to) linux_rename(from, to)
#define _access linux_access
#define access(path, mode) linux_access(path, mode)
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _tell linux_tell
#define tell linux_tell
#define _filelength linux_filelength
#define filelength linux_filelength

/* struct _stat and _stat(). */
/* glibc defines st_atime, st_mtime and st_ctime as macros for the
   tv_sec of these timespec fields. */
struct linux_stat_buf {
    unsigned int st_dev;
    unsigned short st_ino;
    unsigned short st_mode;
    short st_nlink;
    short st_uid;
    short st_gid;
    unsigned int st_rdev;
    long st_size;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};
/* `struct _stat buf; _stat(path, &buf);` names a struct and a function
   alike; so do these. */
int linux_stat_buf(const char* path, struct linux_stat_buf* buf);
#define _stat linux_stat_buf

/* _findfirst. */
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
struct _finddata_t {
    unsigned attrib;
    time_t time_create;
    time_t time_access;
    time_t time_write;
    unsigned long size;
    char name[260];
};
long _findfirst(const char* pattern, struct _finddata_t* data);
int _findnext(long handle, struct _finddata_t* data);
int _findclose(long handle);

/* Folders (direct.h). */
char* _getcwd(char* buffer, int size);
int _chdir(const char* path);
int _mkdir(const char* path);
int _rmdir(const char* path);
int _chdrive(int drive);
int _getdrive(void);
void _splitpath(const char* path, char* drive, char* dir, char* name, char* ext);
void _makepath(char* path, const char* drive, const char* dir, const char* name, const char* ext);
extern char* _pgmptr;

/* Strings. */
#define stricmp strcasecmp
#define _stricmp strcasecmp
#define strcmpi strcasecmp
#define _strcmpi strcasecmp
#define strnicmp strncasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
char* _strupr(char* s);
char* _strlwr(char* s);
char* _strrev(char* s);
#define strupr _strupr
#define strlwr _strlwr
#define strrev _strrev
char* _itoa(int value, char* buffer, int radix);
char* _ltoa(long value, char* buffer, int radix);
char* _ultoa(unsigned long value, char* buffer, int radix);
#define itoa _itoa
#define ltoa _ltoa
#define ultoa _ultoa

static inline unsigned int _rotl(unsigned int value, int shift) {
    shift &= 31;
    return shift == 0 ? value : (value << shift) | (value >> (32 - shift));
}
static inline unsigned int _rotr(unsigned int value, int shift) {
    shift &= 31;
    return shift == 0 ? value : (value >> shift) | (value << (32 - shift));
}

/* Multibyte strings (single-byte here). */
size_t _mbslen(const unsigned char* s);
unsigned char* _mbsinc(const unsigned char* p);
unsigned char* _mbsninc(const unsigned char* p, size_t count);
unsigned char* _mbsdec(const unsigned char* start, const unsigned char* p);
unsigned char* _mbsncpy(unsigned char* dst, const unsigned char* src, size_t count);
unsigned char* _mbsncat(unsigned char* dst, const unsigned char* src, size_t count);
unsigned char* _mbscpy(unsigned char* dst, const unsigned char* src);
unsigned char* _mbscat(unsigned char* dst, const unsigned char* src);
unsigned char* _mbsstr(const unsigned char* s, const unsigned char* find);
unsigned char* _mbschr(const unsigned char* s, unsigned int c);
unsigned char* _mbsrchr(const unsigned char* s, unsigned int c);
int _mbscmp(const unsigned char* a, const unsigned char* b);
int _mbsicmp(const unsigned char* a, const unsigned char* b);
unsigned char* _mbsupr(unsigned char* s);
unsigned char* _mbslwr(unsigned char* s);
int _ismbcspace(unsigned int c);
int _ismbblead(unsigned int c);

#ifdef __cplusplus
}
#endif

#endif
