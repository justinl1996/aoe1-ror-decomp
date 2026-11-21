// Stand-ins for the WINMM calls around the sound code
// (workflow/docs/sdl_port_tasks.md): the multimedia timer that refills the
// streamed WAV buffer (Dsutil.c), and the mmio RIFF file functions it reads
// WAV files with. MCI music is in mci.cpp; the mixer volume lines are in
// dsound_memory.cpp.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// timeGetTime: milliseconds from SDL's clock. timeGetTime counted from
// system start; this starts from the wall clock, so values (and the random
// seed the game takes from one) still differ between runs.

#include <time.h>

DWORD surface_time_get_time(void) {
    static Uint32 offset = 0;
    static int started = 0;
    if (started == 0) {
        offset = (Uint32) time(0) * 1000u;
        started = 1;
    }
    return (DWORD) (SDL_GetTicks() + offset);
}

// ---------------------------------------------------------------------------
// Multimedia timer: timeSetEvent on SDL's timer thread. As with WINMM, the
// callback runs on another thread.

enum { TIMER_SLOTS = 16 };

struct TimerSlot {
    SDL_TimerID id;
    LPTIMECALLBACK proc;
    DWORD user;
    Uint32 period; // 0: one-shot
};

// Slots are never freed: SDL_RemoveTimer does not wait for a callback that
// is running, so a slot must stay valid after its timer is killed.
static TimerSlot timer_slots[TIMER_SLOTS];
static SDL_SpinLock timer_lock = 0;

static Uint32 SDLCALL timer_fire(Uint32, void* param) {
    TimerSlot* slot = (TimerSlot*) param;
    SDL_AtomicLock(&timer_lock);
    LPTIMECALLBACK proc = slot->proc;
    DWORD user = slot->user;
    UINT id = (UINT) slot->id;
    Uint32 period = slot->period;
    SDL_AtomicUnlock(&timer_lock);
    if (proc != 0)
        proc(id, 0, user, 0, 0);
    return period;
}

UINT surface_time_set_event(UINT delay, UINT, LPTIMECALLBACK proc, DWORD user, UINT flags) {
    if (proc == 0 || delay == 0)
        return 0;
    SDL_AtomicLock(&timer_lock);
    TimerSlot* slot = 0;
    for (int i = 0; i < TIMER_SLOTS; ++i) {
        if (timer_slots[i].proc == 0) {
            slot = &timer_slots[i];
            break;
        }
    }
    if (slot != 0) {
        slot->proc = proc;
        slot->user = user;
        slot->period = (flags & TIME_PERIODIC) != 0 ? delay : 0;
        slot->id = 0;
    }
    SDL_AtomicUnlock(&timer_lock);
    if (slot == 0)
        return 0;
    // Hold the lock across SDL_AddTimer so the first callback sees the id.
    SDL_AtomicLock(&timer_lock);
    slot->id = SDL_AddTimer(delay, timer_fire, slot);
    UINT id = (UINT) slot->id;
    if (id == 0)
        slot->proc = 0;
    SDL_AtomicUnlock(&timer_lock);
    return id;
}

MMRESULT surface_time_kill_event(UINT id) {
    if (id == 0)
        return MMSYSERR_INVALPARAM;
    SDL_RemoveTimer((SDL_TimerID) id);
    SDL_AtomicLock(&timer_lock);
    for (int i = 0; i < TIMER_SLOTS; ++i) {
        if (timer_slots[i].proc != 0 && (UINT) timer_slots[i].id == id) {
            timer_slots[i].proc = 0;
            timer_slots[i].id = 0;
        }
    }
    SDL_AtomicUnlock(&timer_lock);
    return MMSYSERR_NOERROR;
}

// ---------------------------------------------------------------------------
// mmio: buffered RIFF file I/O over stdio.
//
// Only what Dsutil.c uses: files opened for reading, or created for
// read/write; mmioRead, mmioWrite and mmioSeek; chunk descend, ascend and
// create; and direct buffer access (mmioGetInfo, mmioAdvance,
// mmioSetInfo). The handle's "position" is the logical file position; while
// the caller has the buffer, the position is the buffer's file offset plus
// how far its pchNext has moved.

enum { MMIO_FILE_BUFFER = 8192 };

struct MmioFile {
    FILE* file;
    MMIOINFO info;    // the buffer state handed out by mmioGetInfo
    char buffer[MMIO_FILE_BUFFER];
};

static MmioFile* mmio_file(HMMIO handle) {
    return (MmioFile*) handle;
}

// Writes out what the caller put in the buffer, if anything.
static MMRESULT mmio_flush(MmioFile* mmio) {
    MMIOINFO* info = &mmio->info;
    if ((info->dwFlags & MMIO_DIRTY) != 0) {
        long length = (long) (info->pchNext - info->pchBuffer);
        if (length > 0) {
            if (fseek(mmio->file, info->lBufOffset, SEEK_SET) != 0
                || fwrite(info->pchBuffer, 1, length, mmio->file) != (size_t) length)
                return MMIOERR_CANNOTWRITE;
        }
        info->dwFlags &= ~MMIO_DIRTY;
    }
    return MMSYSERR_NOERROR;
}

// Empties the buffer at the current logical position.
static void mmio_reset_buffer(MmioFile* mmio) {
    MMIOINFO* info = &mmio->info;
    long position = ftell(mmio->file);
    info->pchBuffer = mmio->buffer;
    info->cchBuffer = MMIO_FILE_BUFFER;
    info->pchNext = mmio->buffer;
    info->pchEndRead = mmio->buffer;
    info->pchEndWrite = mmio->buffer + MMIO_FILE_BUFFER;
    info->lBufOffset = position;
    info->lDiskOffset = position;
}

HMMIO surface_mmio_open(LPSTR name, LPMMIOINFO info, DWORD flags) {
    if (info != 0)
        info->wErrorRet = 0;
    if (name == 0)
        return 0;
    const char* mode = "rb";
    if ((flags & MMIO_CREATE) != 0)
        mode = (flags & MMIO_READWRITE) != 0 ? "w+b" : "wb";
    else if ((flags & MMIO_READWRITE) != 0)
        mode = "r+b";
    else if ((flags & MMIO_WRITE) != 0)
        mode = "r+b";
    MmioFile* mmio = new MmioFile;
    if (mmio == 0)
        return 0;
    memset(&mmio->info, 0, sizeof(mmio->info));
    mmio->file = fopen(name, mode);
    if (mmio->file == 0) {
        if (info != 0)
            info->wErrorRet = MMIOERR_CANNOTOPEN;
        delete mmio;
        return 0;
    }
    mmio->info.dwFlags = flags & ~MMIO_DIRTY;
    mmio_reset_buffer(mmio);
    return (HMMIO) mmio;
}

MMRESULT surface_mmio_close(HMMIO handle, UINT) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0)
        return MMSYSERR_INVALHANDLE;
    MMRESULT result = mmio_flush(mmio);
    if (fclose(mmio->file) != 0 && result == MMSYSERR_NOERROR)
        result = MMIOERR_CANNOTCLOSE;
    delete mmio;
    return result;
}

LONG surface_mmio_read(HMMIO handle, HPSTR data, LONG length) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || length < 0)
        return -1;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return -1;
    LONG count = (LONG) fread(data, 1, length, mmio->file);
    mmio_reset_buffer(mmio);
    return count;
}

LONG surface_mmio_write(HMMIO handle, const char* data, LONG length) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || length < 0)
        return -1;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return -1;
    LONG count = (LONG) fwrite(data, 1, length, mmio->file);
    mmio_reset_buffer(mmio);
    return count == length ? count : -1;
}

LONG surface_mmio_seek(HMMIO handle, LONG offset, int origin) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0)
        return -1;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return -1;
    int whence = origin == SEEK_CUR ? SEEK_CUR : (origin == SEEK_END ? SEEK_END : SEEK_SET);
    if (fseek(mmio->file, offset, whence) != 0)
        return -1;
    mmio_reset_buffer(mmio);
    return ftell(mmio->file);
}

MMRESULT surface_mmio_get_info(HMMIO handle, LPMMIOINFO info, UINT) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || info == 0)
        return MMSYSERR_INVALPARAM;
    *info = mmio->info;
    return MMSYSERR_NOERROR;
}

// Takes the buffer back from the caller; the logical position follows its
// pchNext. Anything written into it goes out now.
MMRESULT surface_mmio_set_info(HMMIO handle, LPCMMIOINFO info, UINT) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || info == 0)
        return MMSYSERR_INVALPARAM;
    mmio->info = *info;
    long position = mmio->info.lBufOffset + (long) (mmio->info.pchNext - mmio->info.pchBuffer);
    MMRESULT result = mmio_flush(mmio);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (fseek(mmio->file, position, SEEK_SET) != 0)
        return MMIOERR_CANNOTSEEK;
    mmio_reset_buffer(mmio);
    return MMSYSERR_NOERROR;
}

// Refills (MMIO_READ) or writes out (MMIO_WRITE) the caller's buffer and
// moves it on to the logical position.
MMRESULT surface_mmio_advance(HMMIO handle, LPMMIOINFO info, UINT flags) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || info == 0)
        return MMSYSERR_INVALPARAM;
    mmio->info = *info;
    long position = mmio->info.lBufOffset + (long) (mmio->info.pchNext - mmio->info.pchBuffer);
    MMRESULT result = mmio_flush(mmio);
    if (result != MMSYSERR_NOERROR)
        return result;
    if (fseek(mmio->file, position, SEEK_SET) != 0)
        return MMIOERR_CANNOTSEEK;
    mmio_reset_buffer(mmio);
    if ((flags & MMIO_WRITE) == 0) {
        size_t count = fread(mmio->buffer, 1, MMIO_FILE_BUFFER, mmio->file);
        mmio->info.pchEndRead = mmio->buffer + count;
        if (ferror(mmio->file))
            result = MMIOERR_CANNOTREAD;
    }
    mmio->info.lDiskOffset = ftell(mmio->file);
    *info = mmio->info;
    return result;
}

static int mmio_read_dword(MmioFile* mmio, DWORD* value) {
    return fread(value, 1, 4, mmio->file) == 4;
}

MMRESULT surface_mmio_descend(HMMIO handle, LPMMCKINFO chunk, const MMCKINFO* parent, UINT flags) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || chunk == 0)
        return MMSYSERR_INVALPARAM;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return MMIOERR_CANNOTWRITE;
    FOURCC want_id = chunk->ckid;
    FOURCC want_type = chunk->fccType;
    long end = parent != 0 ? (long) (parent->dwDataOffset + parent->cksize) : 0x7FFFFFFF;
    for (;;) {
        long start = ftell(mmio->file);
        if (start + 8 > end)
            return MMIOERR_CHUNKNOTFOUND;
        MMCKINFO found;
        memset(&found, 0, sizeof(found));
        if (!mmio_read_dword(mmio, &found.ckid) || !mmio_read_dword(mmio, &found.cksize)) {
            mmio_reset_buffer(mmio);
            return MMIOERR_CHUNKNOTFOUND;
        }
        found.dwDataOffset = start + 8;
        int is_list = found.ckid == FOURCC_RIFF || found.ckid == FOURCC_LIST;
        if (is_list && !mmio_read_dword(mmio, &found.fccType)) {
            mmio_reset_buffer(mmio);
            return MMIOERR_CHUNKNOTFOUND;
        }
        int match;
        if ((flags & MMIO_FINDCHUNK) != 0)
            match = found.ckid == want_id;
        else if ((flags & MMIO_FINDRIFF) != 0)
            match = found.ckid == FOURCC_RIFF && found.fccType == want_type;
        else if ((flags & MMIO_FINDLIST) != 0)
            match = found.ckid == FOURCC_LIST && found.fccType == want_type;
        else
            match = 1;
        if (match) {
            *chunk = found;
            mmio_reset_buffer(mmio);
            return MMSYSERR_NOERROR;
        }
        long next = (long) found.dwDataOffset + (long) found.cksize + (long) (found.cksize & 1);
        if (fseek(mmio->file, next, SEEK_SET) != 0) {
            mmio_reset_buffer(mmio);
            return MMIOERR_CHUNKNOTFOUND;
        }
    }
}

MMRESULT surface_mmio_ascend(HMMIO handle, LPMMCKINFO chunk, UINT) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || chunk == 0)
        return MMSYSERR_INVALPARAM;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return MMIOERR_CANNOTWRITE;
    if ((chunk->dwFlags & MMIO_DIRTY) != 0) {
        // A chunk being written: its size is how far writing got.
        long position = ftell(mmio->file);
        DWORD size = (DWORD) (position - (long) chunk->dwDataOffset);
        if ((size & 1) != 0) {
            fputc(0, mmio->file);
        }
        if (size != chunk->cksize) {
            chunk->cksize = size;
            if (fseek(mmio->file, (long) chunk->dwDataOffset - 4, SEEK_SET) != 0
                || fwrite(&size, 1, 4, mmio->file) != 4)
                return MMIOERR_CANNOTWRITE;
        }
        chunk->dwFlags &= ~MMIO_DIRTY;
    }
    long next = (long) chunk->dwDataOffset + (long) chunk->cksize + (long) (chunk->cksize & 1);
    if (fseek(mmio->file, next, SEEK_SET) != 0)
        return MMIOERR_CANNOTSEEK;
    mmio_reset_buffer(mmio);
    return MMSYSERR_NOERROR;
}

MMRESULT surface_mmio_create_chunk(HMMIO handle, LPMMCKINFO chunk, UINT flags) {
    MmioFile* mmio = mmio_file(handle);
    if (mmio == 0 || chunk == 0)
        return MMSYSERR_INVALPARAM;
    if (mmio_flush(mmio) != MMSYSERR_NOERROR)
        return MMIOERR_CANNOTWRITE;
    if ((flags & MMIO_CREATERIFF) != 0)
        chunk->ckid = FOURCC_RIFF;
    else if ((flags & MMIO_CREATELIST) != 0)
        chunk->ckid = FOURCC_LIST;
    int is_list = chunk->ckid == FOURCC_RIFF || chunk->ckid == FOURCC_LIST;
    long start = ftell(mmio->file);
    if (fwrite(&chunk->ckid, 1, 4, mmio->file) != 4 || fwrite(&chunk->cksize, 1, 4, mmio->file) != 4)
        return MMIOERR_CANNOTWRITE;
    if (is_list && fwrite(&chunk->fccType, 1, 4, mmio->file) != 4)
        return MMIOERR_CANNOTWRITE;
    chunk->dwDataOffset = start + 8;
    chunk->dwFlags = MMIO_DIRTY;
    mmio_reset_buffer(mmio);
    return MMSYSERR_NOERROR;
}
