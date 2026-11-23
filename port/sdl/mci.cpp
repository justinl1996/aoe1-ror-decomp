// Music without MCI (workflow/docs/sdl_port_tasks.md): a stand-in for the
// mciSendCommandA calls of music.cpp and Cdaudio.cpp, playing WAV files
// through the SDL mixer (dsound_memory.cpp).
//
// The game plays music from the CD's audio tracks (the "cdaudio" device),
// or MIDI files through the "sequencer" device. Neither exists with SDL, so:
//
// - CD audio plays tracks ripped to WAV files, music/trackNN.wav next to
//   the executable (track02.wav is the first music track; track 1 is the
//   CD's data track). The device opens when at least one exists, and the
//   disc has as many tracks as the highest number found.
// - A MIDI file plays a WAV rendering of it: the same name with .wav in the
//   same folder, or in music/. Without one, the file goes to the system's
//   own MCI sequencer on Windows, as before.
//
// The commands and flags the game uses behave as MCI's do, notifications
// included: a play with MCI_NOTIFY posts MM_MCINOTIFY to its window when it
// finishes, or when a later command interrupts it (superseded or aborted),
// so the game's count of pending notifications stays right.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <stdio.h>
#include <string.h>

enum {
    MCI_OWN_ID_BASE = 0x10000, // our device ids; the system's are small
    MCI_DEVICES = 8,
    MCI_READ_BYTES = 8192,
};

struct MciDevice {
    int used;
    int cd;                 // cdaudio, else a sequencer
    MCIDEVICEID id;
    DWORD time_format;
    char path[MAX_PATH];    // sequencer: the WAV file
    int track_count;        // cdaudio
    // Where playing starts next (after open, seek, pause or stop).
    int track;              // cdaudio track; 0 for a sequencer
    Uint32 offset_ms;
    // Set while playing.
    int playing;
    int paused;
    int end_track;          // cdaudio: stop at the start of this track
    HWND notify_wnd;
    int notify_pending;
};

static MciDevice mci_devices[MCI_DEVICES];
static MCIDEVICEID mci_next_id = MCI_OWN_ID_BASE;

static MciDevice* mci_find(MCIDEVICEID id) {
    for (int i = 0; i < MCI_DEVICES; ++i)
        if (mci_devices[i].used != 0 && mci_devices[i].id == id)
            return &mci_devices[i];
    return 0;
}

static void mci_notify(MciDevice* device, UINT result) {
    if (device->notify_pending != 0) {
        device->notify_pending = 0;
        if (device->notify_wnd != 0)
            surface_post_message(device->notify_wnd, MM_MCINOTIFY, result, (LPARAM) device->id);
    }
}

// ---------------------------------------------------------------------------
// WAV files

struct WavFile {
    SDL_RWops* file;
    SDL_AudioFormat format;
    int channels;
    int rate;
    int block;          // bytes per frame
    Uint32 data_left;   // bytes of the data chunk still to read
};

static Uint32 wav_read32(SDL_RWops* file) {
    Uint32 value = 0;
    if (SDL_RWread(file, &value, 4, 1) != 1)
        return 0;
    return SDL_SwapLE32(value);
}

// Opens a PCM (8, 16 or 32-bit) or 32-bit float WAV file and skips offset_ms
// into its data.
static int wav_open(WavFile* wav, const char* path, Uint32 offset_ms) {
    memset(wav, 0, sizeof(*wav));
    SDL_RWops* file = SDL_RWFromFile(path, "rb");
    if (file == 0)
        return 0;
    if (wav_read32(file) != 0x46464952 /* RIFF */) {
        SDL_RWclose(file);
        return 0;
    }
    wav_read32(file);
    if (wav_read32(file) != 0x45564157 /* WAVE */) {
        SDL_RWclose(file);
        return 0;
    }
    int have_format = 0;
    for (;;) {
        Uint32 id = wav_read32(file);
        Uint32 size = wav_read32(file);
        if (id == 0) {
            SDL_RWclose(file);
            return 0;
        }
        Sint64 next = SDL_RWtell(file) + size + (size & 1);
        if (id == 0x20746D66 /* fmt  */ && size >= 16) {
            Uint16 fields[8];
            memset(fields, 0, sizeof(fields));
            SDL_RWread(file, fields, 2, 8);
            Uint16 tag = SDL_SwapLE16(fields[0]);
            wav->channels = SDL_SwapLE16(fields[1]);
            wav->rate = (int) (SDL_SwapLE16(fields[2]) | ((Uint32) SDL_SwapLE16(fields[3]) << 16));
            wav->block = SDL_SwapLE16(fields[6]);
            int bits = SDL_SwapLE16(fields[7]);
            if (tag == 0xFFFE && size >= 40) {
                // WAVE_FORMAT_EXTENSIBLE: the real tag starts the sub-format.
                Uint16 extensible[5];
                SDL_RWread(file, extensible, 2, 5);
                tag = SDL_SwapLE16(extensible[4]);
            }
            if (tag == 1 && bits == 8)
                wav->format = AUDIO_U8;
            else if (tag == 1 && bits == 16)
                wav->format = AUDIO_S16LSB;
            else if (tag == 1 && bits == 32)
                wav->format = AUDIO_S32LSB;
            else if (tag == 3 && bits == 32)
                wav->format = AUDIO_F32LSB;
            else
                wav->format = 0;
            have_format = wav->format != 0 && wav->channels > 0 && wav->rate > 0 && wav->block > 0;
        } else if (id == 0x61746164 /* data */) {
            if (have_format == 0)
                break;
            wav->file = file;
            wav->data_left = size;
            Uint32 skip = (Uint32) (((Uint64) offset_ms * wav->rate) / 1000) * wav->block;
            if (skip > size)
                skip = size;
            if (skip > 0)
                SDL_RWseek(file, skip, RW_SEEK_CUR);
            wav->data_left -= skip;
            return 1;
        }
        if (SDL_RWseek(file, next, RW_SEEK_SET) < 0)
            break;
    }
    SDL_RWclose(file);
    return 0;
}

static void wav_close(WavFile* wav) {
    if (wav->file != 0)
        SDL_RWclose(wav->file);
    wav->file = 0;
}

static void music_dir_path(char* out, size_t size, const char* name) {
    char* base = port_base_path();
    SDL_snprintf(out, size, "%smusic%c%s", base != 0 ? base : "", base != 0 ? base[SDL_strlen(base) - 1] : '/', name);
    SDL_free(base);
}

static void cd_track_path(char* out, size_t size, int track) {
    char name[32];
    SDL_snprintf(name, sizeof(name), "track%02d.wav", track);
    music_dir_path(out, size, name);
}

static int file_exists(const char* path) {
    SDL_RWops* file = SDL_RWFromFile(path, "rb");
    if (file == 0)
        return 0;
    SDL_RWclose(file);
    return 1;
}

// ---------------------------------------------------------------------------
// The player: one device plays at a time, on the audio thread.

static MciDevice* player_device = 0;
static WavFile player_wav;
static SDL_AudioStream* player_stream = 0;
static int player_track = 0;
static Uint32 player_start_ms = 0;     // position of the first frame played
static Uint64 player_frames = 0;       // output frames played since then
static int player_flushed = 0;

static void player_close_source(void) {
    wav_close(&player_wav);
    if (player_stream != 0) {
        SDL_FreeAudioStream(player_stream);
        player_stream = 0;
    }
}

// Opens the device's current track (or file) at offset_ms. CD tracks
// without a file are skipped.
static int player_open_source(MciDevice* device, int track, Uint32 offset_ms) {
    player_close_source();
    char path[MAX_PATH];
    for (;;) {
        if (device->cd != 0) {
            if (track >= device->end_track)
                return 0;
            cd_track_path(path, sizeof(path), track);
        } else {
            SDL_strlcpy(path, device->path, sizeof(path));
        }
        if (wav_open(&player_wav, path, offset_ms) != 0)
            break;
        if (device->cd == 0)
            return 0;
        ++track;
        offset_ms = 0;
    }
    player_stream = SDL_NewAudioStream(
        player_wav.format, (Uint8) player_wav.channels, player_wav.rate, AUDIO_S16SYS, 2, MIX_OUTPUT_RATE
    );
    if (player_stream == 0) {
        wav_close(&player_wav);
        return 0;
    }
    player_track = track;
    player_start_ms = offset_ms;
    player_frames = 0;
    player_flushed = 0;
    return 1;
}

static Uint32 player_position_ms(void) {
    return player_start_ms + (Uint32) (player_frames * 1000 / MIX_OUTPUT_RATE);
}

// The audio thread: fill frames of the music. Called with the device locked.
static int player_fill(short* out, int frames) {
    MciDevice* device = player_device;
    if (device == 0)
        return 0;
    int want = frames * 4;
    int got = 0;
    Uint8 chunk[MCI_READ_BYTES];
    while (got < want) {
        int n = player_stream != 0 ? SDL_AudioStreamGet(player_stream, (Uint8*) out + got, want - got) : 0;
        if (n > 0) {
            got += n;
            continue;
        }
        if (player_stream != 0 && player_wav.data_left > 0) {
            Uint32 read = player_wav.data_left < sizeof(chunk) ? player_wav.data_left : (Uint32) sizeof(chunk);
            read -= read % player_wav.block;
            size_t count = read > 0 ? SDL_RWread(player_wav.file, chunk, 1, read) : 0;
            if (count > 0) {
                player_wav.data_left -= (Uint32) count;
                SDL_AudioStreamPut(player_stream, chunk, (int) count);
                continue;
            }
            player_wav.data_left = 0; // a short file
        }
        if (player_stream != 0 && player_flushed == 0) {
            SDL_AudioStreamFlush(player_stream);
            player_flushed = 1;
            continue;
        }
        // This source is done: the next track, or the end.
        if (device->cd != 0 && player_open_source(device, player_track + 1, 0) != 0)
            continue;
        player_close_source();
        device->playing = 0;
        device->track = device->cd != 0 ? device->end_track : 0;
        device->offset_ms = 0;
        player_device = 0;
        mci_notify(device, MCI_NOTIFY_SUCCESSFUL);
        break;
    }
    if (got < want)
        memset((Uint8*) out + got, 0, want - got);
    player_frames += (Uint64) got / 4;
    return got > 0;
}

// Stops the device if it is playing and keeps its position. Call with the
// audio device locked.
static void player_halt(MciDevice* device) {
    if (player_device == device && device != 0) {
        device->track = player_track;
        device->offset_ms = player_position_ms();
        player_close_source();
        player_device = 0;
    }
    if (device != 0)
        device->playing = 0;
}

// ---------------------------------------------------------------------------
// Commands

static Uint32 tmsf_offset_ms(DWORD tmsf) {
    return (MCI_TMSF_MINUTE(tmsf) * 60 + MCI_TMSF_SECOND(tmsf)) * 1000 + MCI_TMSF_FRAME(tmsf) * 1000 / 75;
}

static int device_type_of(DWORD flags, const MCI_OPEN_PARMSA* open) {
    if ((flags & MCI_OPEN_TYPE) == 0)
        return 0;
    if ((flags & MCI_OPEN_TYPE_ID) != 0)
        return LOWORD((uintptr_t) open->lpstrDeviceType);
    if (open->lpstrDeviceType == 0)
        return 0;
    if (SDL_strcasecmp(open->lpstrDeviceType, "cdaudio") == 0)
        return MCI_DEVTYPE_CD_AUDIO;
    if (SDL_strcasecmp(open->lpstrDeviceType, "sequencer") == 0)
        return MCI_DEVTYPE_SEQUENCER;
    return 0;
}

// The WAV that stands for a MIDI file, if there is one.
static int midi_rendering(const char* midi, char* out, size_t size) {
    if (midi == 0 || *midi == 0)
        return 0;
    SDL_strlcpy(out, midi, size);
    char* dot = SDL_strrchr(out, '.');
    char* slash = SDL_strrchr(out, '\\');
    char* slash2 = SDL_strrchr(out, '/');
    if (slash2 > slash)
        slash = slash2;
    if (dot != 0 && (slash == 0 || dot > slash))
        *dot = 0;
    SDL_strlcat(out, ".wav", size);
    if (file_exists(out))
        return 1;
    char name[MAX_PATH];
    SDL_strlcpy(name, slash != 0 ? slash + 1 : out, sizeof(name));
    music_dir_path(out, size, name);
    return file_exists(out);
}

static MCIERROR mci_open(DWORD flags, MCI_OPEN_PARMSA* open, int* handled) {
    int type = device_type_of(flags, open);
    MciDevice device;
    memset(&device, 0, sizeof(device));
    if (type == MCI_DEVTYPE_CD_AUDIO) {
        *handled = 1;
        if (mix_audio_open() == 0)
            return MCIERR_HARDWARE;
        char path[MAX_PATH];
        for (int track = 1; track <= 99; ++track) {
            cd_track_path(path, sizeof(path), track);
            if (file_exists(path))
                device.track_count = track;
        }
        if (device.track_count == 0)
            return MCIERR_DEVICE_NOT_READY;
        device.cd = 1;
        device.track = 1;
        device.time_format = MCI_FORMAT_MSF;
    } else if (type == MCI_DEVTYPE_SEQUENCER && (flags & MCI_OPEN_ELEMENT) != 0) {
        if (mix_audio_open() == 0 || midi_rendering(open->lpstrElementName, device.path, sizeof(device.path)) == 0)
            return 0; // not ours: the system's sequencer, where there is one
        *handled = 1;
        WavFile probe;
        if (wav_open(&probe, device.path, 0) == 0)
            return MCIERR_INVALID_FILE;
        wav_close(&probe);
        device.time_format = MCI_FORMAT_MILLISECONDS;
    } else {
        return 0;
    }
    for (int i = 0; i < MCI_DEVICES; ++i) {
        if (mci_devices[i].used == 0) {
            device.used = 1;
            device.id = mci_next_id++;
            mci_devices[i] = device;
            open->wDeviceID = device.id;
            return 0;
        }
    }
    return MCIERR_OUT_OF_MEMORY;
}

static MCIERROR mci_play(MciDevice* device, DWORD flags, MCI_PLAY_PARMS* play) {
    if (play == 0 && (flags & (MCI_FROM | MCI_TO | MCI_NOTIFY)) != 0)
        return MCIERR_NULL_PARAMETER_BLOCK;
    mix_audio_lock();
    // A device that is playing again restarts from where it is.
    player_halt(device);
    if (player_device != 0) {
        // Another device was playing: it stops, as a CD drive would when
        // the game moved on.
        MciDevice* other = player_device;
        player_halt(other);
        mci_notify(other, MCI_NOTIFY_ABORTED);
    }
    mci_notify(device, (flags & MCI_NOTIFY) != 0 ? MCI_NOTIFY_SUPERSEDED : MCI_NOTIFY_ABORTED);
    int track = device->track;
    Uint32 offset = device->offset_ms;
    if ((flags & MCI_FROM) != 0) {
        if (device->cd != 0) {
            track = MCI_TMSF_TRACK(play->dwFrom);
            offset = tmsf_offset_ms(play->dwFrom);
        } else {
            track = 0;
            offset = play->dwFrom;
        }
    }
    device->end_track = device->track_count + 1;
    if (device->cd != 0 && (flags & MCI_TO) != 0) {
        device->end_track = MCI_TMSF_TRACK(play->dwTo);
        if (tmsf_offset_ms(play->dwTo) != 0)
            ++device->end_track;
    }
    if (device->cd != 0 && track < 1)
        track = 1;
    device->paused = 0;
    MCIERROR result = 0;
    if (player_open_source(device, track, offset) != 0) {
        player_device = device;
        device->playing = 1;
    } else {
        // Nothing left to play: finished at once.
        device->playing = 0;
    }
    if ((flags & MCI_NOTIFY) != 0) {
        device->notify_wnd = (HWND) play->dwCallback;
        device->notify_pending = 1;
        if (device->playing == 0)
            mci_notify(device, MCI_NOTIFY_SUCCESSFUL);
    }
    mix_audio_unlock();
    return result;
}

static MCIERROR mci_status(MciDevice* device, DWORD flags, MCI_STATUS_PARMS* status) {
    if (status == 0)
        return MCIERR_NULL_PARAMETER_BLOCK;
    if ((flags & MCI_STATUS_ITEM) == 0)
        return MCIERR_MISSING_PARAMETER;
    mix_audio_lock();
    int track = device->track;
    Uint32 offset = device->offset_ms;
    if (player_device == device) {
        track = player_track;
        offset = player_position_ms();
    }
    MCIERROR result = 0;
    switch (status->dwItem) {
        case MCI_STATUS_NUMBER_OF_TRACKS:
            status->dwReturn = device->cd != 0 ? device->track_count : 1;
            break;
        case MCI_STATUS_CURRENT_TRACK:
            status->dwReturn = device->cd != 0 ? track : 1;
            break;
        case MCI_STATUS_POSITION:
            if (device->cd != 0 && device->time_format == MCI_FORMAT_TMSF) {
                Uint32 seconds = offset / 1000;
                status->dwReturn =
                    MCI_MAKE_TMSF(track, seconds / 60, seconds % 60, (offset % 1000) * 75 / 1000);
            } else {
                status->dwReturn = offset;
            }
            break;
        case MCI_STATUS_MODE:
            status->dwReturn =
                device->playing != 0 ? MCI_MODE_PLAY : (device->paused != 0 ? MCI_MODE_PAUSE : MCI_MODE_STOP);
            break;
        case MCI_STATUS_READY:
            status->dwReturn = TRUE;
            break;
        default:
            result = MCIERR_UNSUPPORTED_FUNCTION;
            break;
    }
    mix_audio_unlock();
    return result;
}

static MCIERROR mci_own_command(MciDevice* device, UINT msg, DWORD flags, DWORD_PTR params) {
    switch (msg) {
        case MCI_CLOSE:
            mix_audio_lock();
            player_halt(device);
            mci_notify(device, MCI_NOTIFY_ABORTED);
            device->used = 0;
            mix_audio_unlock();
            return 0;
        case MCI_PLAY:
            return mci_play(device, flags, (MCI_PLAY_PARMS*) params);
        case MCI_RESUME:
            if (device->paused == 0)
                return 0;
            return mci_play(device, 0, 0);
        case MCI_PAUSE:
        case MCI_STOP:
            mix_audio_lock();
            device->paused = msg == MCI_PAUSE && device->playing != 0;
            player_halt(device);
            mci_notify(device, MCI_NOTIFY_ABORTED);
            mix_audio_unlock();
            return 0;
        case MCI_SEEK: {
            MCI_SEEK_PARMS* seek = (MCI_SEEK_PARMS*) params;
            mix_audio_lock();
            player_halt(device);
            mci_notify(device, MCI_NOTIFY_ABORTED);
            device->paused = 0;
            if ((flags & MCI_SEEK_TO_START) != 0 || seek == 0) {
                device->track = 1;
                device->offset_ms = 0;
            } else if ((flags & MCI_TO) != 0) {
                if (device->cd != 0) {
                    device->track = MCI_TMSF_TRACK(seek->dwTo);
                    device->offset_ms = tmsf_offset_ms(seek->dwTo);
                } else {
                    device->offset_ms = seek->dwTo;
                }
            }
            if (device->cd == 0)
                device->track = 0;
            mix_audio_unlock();
            return 0;
        }
        case MCI_SET: {
            MCI_SET_PARMS* set = (MCI_SET_PARMS*) params;
            if ((flags & MCI_SET_TIME_FORMAT) != 0 && set != 0)
                device->time_format = set->dwTimeFormat;
            return 0;
        }
        case MCI_STATUS:
            return mci_status(device, flags, (MCI_STATUS_PARMS*) params);
    }
    return MCIERR_UNSUPPORTED_FUNCTION;
}

extern "C" MCIERROR WINAPI surface_mci_send_command(MCIDEVICEID id, UINT msg, DWORD flags, DWORD_PTR params) {
    static int source_set = 0;
    if (source_set == 0 && mix_audio_open() != 0) {
        mix_set_music_source(player_fill);
        source_set = 1;
    }
    if (msg == MCI_OPEN) {
        MCI_OPEN_PARMSA* open = (MCI_OPEN_PARMSA*) params;
        if (open == 0)
            return MCIERR_NULL_PARAMETER_BLOCK;
        int handled = 0;
        MCIERROR result = mci_open(flags, open, &handled);
        if (handled != 0)
            return result;
    } else {
        MciDevice* device = mci_find(id);
        if (device != 0)
            return mci_own_command(device, msg, flags, params);
    }
#ifdef _WIN32
    return mciSendCommandA(id, msg, flags, params);
#else
    return MCIERR_DEVICE_NOT_INSTALLED;
#endif
}

extern "C" BOOL surface_mci_get_error_string(MCIERROR error, LPSTR text, UINT length) {
#ifdef _WIN32
    return mciGetErrorStringA(error, text, length);
#else
    if (text == 0 || length == 0)
        return FALSE;
    SDL_snprintf(text, length, "MCI error %lu", (unsigned long) error);
    return TRUE;
#endif
}
