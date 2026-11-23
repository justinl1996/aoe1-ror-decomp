#ifndef AOE1_RGE_FIO_H
#define AOE1_RGE_FIO_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>

int __cdecl rge_fake_open(int file_handle, int fake_size);
int __cdecl rge_open(char* filename, int flag);
int __cdecl rge_open(char* filename, int flag, int pmode);
int __cdecl rge_fake_close(int handle);
int __cdecl rge_close(int handle);
void __cdecl rge_read_uncompressed(int handle, void* data, int size);
void __cdecl rge_write_uncompressed(int handle, void* data, int size);
void __cdecl rge_read(int handle, void* data, int size);
int __cdecl rge_buffer_full(unsigned char* out_buf_ofs, int out_buf_size);
void __cdecl rge_write(int handle, void* data, int size);

#if defined(__cplusplus) && (defined(__LP64__) || defined(_WIN64) || defined(__wasm64__))
/* Pointer arrays in empires.dat / savegames store 32-bit non-zero/zero presence flags. */
template <typename T>
inline void rge_read(int handle, T** data, int size) {
    if (size < 4) {
        rge_read(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t flag = 0;
        rge_read(handle, (void*) &flag, 4);
        data[i] = (T*) (uintptr_t) flag;
    }
}

template <typename T>
inline void rge_write(int handle, T** data, int size) {
    if (size < 4) {
        rge_write(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t flag = (data[i] != 0) ? 1u : 0u;
        rge_write(handle, (void*) &flag, 4);
    }
}
#endif

#if defined(__cplusplus) && (defined(__LP64__) || defined(__wasm64__))
/* On LP64, long is 64-bit, whereas empires.dat / savegames store 32-bit little-endian longs. */
inline void rge_read(int handle, long* data, int size) {
    if (size < 4) {
        rge_read(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        int32_t tmp = 0;
        rge_read(handle, (void*) &tmp, 4);
        data[i] = (long) tmp;
    }
}

inline void rge_read(int handle, unsigned long* data, int size) {
    if (size < 4) {
        rge_read(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t tmp = 0;
        rge_read(handle, (void*) &tmp, 4);
        data[i] = (unsigned long) tmp;
    }
}

inline void rge_write(int handle, long* data, int size) {
    if (size < 4) {
        rge_write(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        int32_t tmp = (int32_t) data[i];
        rge_write(handle, (void*) &tmp, 4);
    }
}

inline void rge_write(int handle, unsigned long* data, int size) {
    if (size < 4) {
        rge_write(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t tmp = (uint32_t) data[i];
        rge_write(handle, (void*) &tmp, 4);
    }
}

inline void rge_read_uncompressed(int handle, long* data, int size) {
    if (size < 4) {
        rge_read_uncompressed(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        int32_t tmp = 0;
        rge_read_uncompressed(handle, (void*) &tmp, 4);
        data[i] = (long) tmp;
    }
}

inline void rge_read_uncompressed(int handle, unsigned long* data, int size) {
    if (size < 4) {
        rge_read_uncompressed(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t tmp = 0;
        rge_read_uncompressed(handle, (void*) &tmp, 4);
        data[i] = (unsigned long) tmp;
    }
}

inline void rge_write_uncompressed(int handle, long* data, int size) {
    if (size < 4) {
        rge_write_uncompressed(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        int32_t tmp = (int32_t) data[i];
        rge_write_uncompressed(handle, (void*) &tmp, 4);
    }
}

inline void rge_write_uncompressed(int handle, unsigned long* data, int size) {
    if (size < 4) {
        rge_write_uncompressed(handle, (void*) data, size);
        return;
    }
    int count = size / 4;
    for (int i = 0; i < count; ++i) {
        uint32_t tmp = (uint32_t) data[i];
        rge_write_uncompressed(handle, (void*) &tmp, 4);
    }
}
#endif

extern int rge_write_error;

#endif // AOE1_RGE_FIO_H
