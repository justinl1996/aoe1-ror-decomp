// In-memory DirectSound mixed by SDL (workflow/docs/sdl_port_tasks.md,
// Task 5).
//
// The game uses a small part of DirectSound: one device, a primary buffer it
// only plays and formats, static secondary buffers for its sounds (with
// volume, pan and frequency), duplicates of them, and one looping buffer
// that Dsutil.c refills from a timer thread for streamed music. This file
// implements IDirectSound and IDirectSoundBuffer, as the VC98 dsound.h
// declares them, over plain memory, and mixes every playing buffer in one
// SDL audio callback. Only surface_DirectSoundCreate replaces
// DirectSoundCreate; the game's call sites (including the IDirectSound*_
// C macros in Dsutil.c, which call through the same vtables) stay as they
// are.
//
// The audio thread reads buffer memory and advances play cursors; the game
// thread and the stream timer thread change buffers. Every change to the
// list of playing buffers and to a buffer's playback state happens with the
// SDL audio device locked. Buffer memory is written without the lock, as
// with DirectSound.

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <string.h>

#include "../../include/surface.h"
#include "sdl_internal.h"


enum {
    MIX_RATE = 44100,
    MIX_SAMPLES = 512,     // per callback: about 12 ms
    GAIN_ONE = 1 << 16,    // fixed-point unit gain
    STEP_SHIFT = 16,       // fixed-point source frames per output frame
    WRITE_LEAD_MS = 15,    // write cursor distance ahead of the play cursor
};

class MemSoundBuffer;

// Sample memory and format, shared by a buffer and its duplicates.
struct SoundData {
    long refs;
    unsigned char* bytes;
    DWORD size;
    WAVEFORMATEX format;
};

class MemDirectSound : public IDirectSound {
  public:
    ULONG refs;

    MemDirectSound() : refs(1) {}
    ~MemDirectSound();

    STDMETHOD(QueryInterface)(REFIID, LPVOID FAR* out) {
        *out = 0;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return ++this->refs; }
    STDMETHOD_(ULONG, Release)() {
        ULONG left = --this->refs;
        if (left == 0)
            delete this;
        return left;
    }
    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN);
    STDMETHOD(GetCaps)(LPDSCAPS caps);
    STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* out);
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) { return DS_OK; }
    STDMETHOD(Compact)() { return DS_OK; }
    STDMETHOD(GetSpeakerConfig)(LPDWORD config) {
        *config = DSSPEAKER_STEREO;
        return DS_OK;
    }
    STDMETHOD(SetSpeakerConfig)(DWORD) { return DS_OK; }
#ifdef AOE_LINUX
    STDMETHOD(Initialize)(LPCGUID) { return DSERR_ALREADYINITIALIZED; }  // Wine's dsound.h
#else
    STDMETHOD(Initialize)(LPGUID) { return DSERR_ALREADYINITIALIZED; }   // VC6's dsound.h
#endif
};

class MemSoundBuffer : public IDirectSoundBuffer {
  public:
    ULONG refs;
    DWORD flags;
    SoundData* data;       // 0 for the primary buffer
    // Playback state, guarded by the audio device lock.
    int playing;
    int looping;
    DWORD position;        // byte offset of the current frame
    DWORD fraction;        // position within the frame, of 1 << STEP_SHIFT
    long volume;
    long pan;
    DWORD frequency;       // 0: the format's own rate
    long gain_left;
    long gain_right;
    MemSoundBuffer* next_playing;

    MemSoundBuffer(DWORD flags_in, SoundData* data_in);
    ~MemSoundBuffer();
    void update_gains();
    DWORD frame_bytes() const { return this->data != 0 ? this->data->format.nBlockAlign : 1; }

    STDMETHOD(QueryInterface)(REFIID, LPVOID FAR* out) {
        *out = 0;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return ++this->refs; }
    STDMETHOD_(ULONG, Release)() {
        ULONG left = --this->refs;
        if (left == 0)
            delete this;
        return left;
    }
    STDMETHOD(GetCaps)(LPDSBCAPS caps);
    STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write);
    STDMETHOD(GetFormat)(LPWAVEFORMATEX format, DWORD size, LPDWORD written);
    STDMETHOD(GetVolume)(LPLONG out) {
        *out = this->volume;
        return DS_OK;
    }
    STDMETHOD(GetPan)(LPLONG out) {
        *out = this->pan;
        return DS_OK;
    }
    STDMETHOD(GetFrequency)(LPDWORD out) {
        *out = this->frequency != 0 ? this->frequency : (this->data != 0 ? this->data->format.nSamplesPerSec : MIX_RATE);
        return DS_OK;
    }
    STDMETHOD(GetStatus)(LPDWORD status);
    STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }
    STDMETHOD(Lock)(DWORD offset, DWORD bytes, LPVOID* ptr1, LPDWORD bytes1, LPVOID* ptr2, LPDWORD bytes2, DWORD flags);
    STDMETHOD(Play)(DWORD, DWORD, DWORD flags);
    STDMETHOD(SetCurrentPosition)(DWORD position_in);
    STDMETHOD(SetFormat)(LPCWAVEFORMATEX) { return DS_OK; }
    STDMETHOD(SetVolume)(LONG volume_in);
    STDMETHOD(SetPan)(LONG pan_in);
    STDMETHOD(SetFrequency)(DWORD frequency_in);
    STDMETHOD(Stop)();
    STDMETHOD(Unlock)(LPVOID, DWORD, LPVOID, DWORD) { return DS_OK; }
    STDMETHOD(Restore)() { return DS_OK; }
};

// ---------------------------------------------------------------------------
// The SDL device and mixer

static SDL_AudioDeviceID mix_device = 0;
static MemSoundBuffer* mix_playing = 0;   // singly linked through next_playing
static int mix_devices_open = 0;           // MemDirectSound objects alive

// The two volume lines that stood in for the system mixer's wave-out and
// CD/synthesizer lines: wave scales every DirectSound buffer, music scales
// the music source (mci.cpp). Fixed-point gains, GAIN_ONE at full volume.
static long mix_line_volume[2] = {0, 0};
static long mix_line_gain[2] = {GAIN_ONE, GAIN_ONE};
static MixMusicFill mix_music_fill = 0;

static void mix_lock(void) {
    if (mix_device != 0)
        SDL_LockAudioDevice(mix_device);
}

static void mix_unlock(void) {
    if (mix_device != 0)
        SDL_UnlockAudioDevice(mix_device);
}

// Called with the device locked (or before the device exists).
static void mix_remove(MemSoundBuffer* buffer) {
    MemSoundBuffer** link = &mix_playing;
    while (*link != 0) {
        if (*link == buffer) {
            *link = buffer->next_playing;
            buffer->next_playing = 0;
            return;
        }
        link = &(*link)->next_playing;
    }
}

static int mix_sample(const SoundData* data, DWORD offset, int channel) {
    const WAVEFORMATEX* format = &data->format;
    if (format->nChannels < 2)
        channel = 0;
    if (format->wBitsPerSample == 8)
        return ((int) data->bytes[offset + channel] - 128) << 8;
    return *(const short*) (data->bytes + offset + channel * 2);
}

// Mix one buffer into accum (interleaved stereo). Returns 0 when a one-shot
// buffer reached its end.
static int mix_buffer(MemSoundBuffer* buffer, int* accum, int frames) {
    SoundData* data = buffer->data;
    DWORD frame = buffer->frame_bytes();
    if (data == 0 || data->size < frame || frame == 0)
        return 0;
    DWORD rate = buffer->frequency != 0 ? buffer->frequency : data->format.nSamplesPerSec;
    DWORD step = (DWORD) (((double) rate / MIX_RATE) * (1 << STEP_SHIFT));
    DWORD end = data->size - data->size % frame;

    for (int i = 0; i < frames; ++i) {
        if (buffer->position >= end) {
            if (buffer->looping == 0)
                return 0;
            buffer->position = 0;
        }
        DWORD next = buffer->position + frame;
        if (next >= end)
            next = buffer->looping != 0 ? 0 : buffer->position;
        // Linear interpolation between this frame and the next.
        int t = (int) (buffer->fraction >> (STEP_SHIFT - 8)); // 0..255
        for (int channel = 0; channel < 2; ++channel) {
            int a = mix_sample(data, buffer->position, channel);
            int b = mix_sample(data, next, channel);
            int value = a + (((b - a) * t) >> 8);
            long gain = channel == 0 ? buffer->gain_left : buffer->gain_right;
            accum[i * 2 + channel] += (int) (((long long) value * gain) >> 16);
        }
        buffer->fraction += step;
        buffer->position += (buffer->fraction >> STEP_SHIFT) * frame;
        buffer->fraction &= (1 << STEP_SHIFT) - 1;
    }
    return 1;
}

static void SDLCALL mix_callback(void*, Uint8* stream, int len) {
    int frames = len / 4;
    static int accum[MIX_SAMPLES * 2 * 4];
    static short music[MIX_SAMPLES * 2 * 4];
    if (frames > MIX_SAMPLES * 4)
        frames = MIX_SAMPLES * 4;
    memset(accum, 0, frames * 2 * sizeof(int));

    MemSoundBuffer** link = &mix_playing;
    while (*link != 0) {
        MemSoundBuffer* buffer = *link;
        if (mix_buffer(buffer, accum, frames) == 0) {
            buffer->playing = 0;
            buffer->position = 0;
            buffer->fraction = 0;
            *link = buffer->next_playing;
            buffer->next_playing = 0;
        } else {
            link = &buffer->next_playing;
        }
    }

    long wave_gain = mix_line_gain[SURFACE_SOUND_WAVE];
    if (wave_gain != GAIN_ONE) {
        for (int i = 0; i < frames * 2; ++i)
            accum[i] = (int) (((long long) accum[i] * wave_gain) >> 16);
    }
    if (mix_music_fill != 0 && mix_music_fill(music, frames) != 0) {
        long music_gain = mix_line_gain[SURFACE_SOUND_MUSIC];
        for (int i = 0; i < frames * 2; ++i)
            accum[i] += (int) (((long long) music[i] * music_gain) >> 16);
    }

    short* out = (short*) stream;
    for (int i = 0; i < frames * 2; ++i) {
        int value = accum[i];
        if (value > 32767)
            value = 32767;
        else if (value < -32768)
            value = -32768;
        out[i] = (short) value;
    }
    if (frames * 4 < len)
        memset(stream + frames * 4, 0, len - frames * 4);
}

static int mix_open(void) {
    if (mix_device != 0)
        return 1;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return 0;
    SDL_AudioSpec want;
    SDL_AudioSpec have;
    memset(&want, 0, sizeof(want));
    want.freq = MIX_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = MIX_SAMPLES;
    want.callback = mix_callback;
    // Exactly this format: the callback writes 16-bit stereo at MIX_RATE.
    mix_device = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
    if (mix_device == 0)
        return 0;
    SDL_PauseAudioDevice(mix_device, 0);
    return 1;
}

static void mix_close(void) {
    if (mix_device != 0) {
        SDL_CloseAudioDevice(mix_device);
        mix_device = 0;
    }
    mix_playing = 0;
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void mix_audio_lock(void) {
    mix_lock();
}

void mix_audio_unlock(void) {
    mix_unlock();
}

int mix_audio_open(void) {
    return mix_device != 0;
}

void mix_set_music_source(MixMusicFill fill) {
    mix_lock();
    mix_music_fill = fill;
    mix_unlock();
}

// Volume in the game's mixer units: -10000 (silent) to 0 (full), linear in
// amplitude, as the system mixer's volume controls were.
extern "C" long surface_sound_get_volume(int line) {
    if (line < 0 || line > 1)
        return 0;
    return mix_line_volume[line];
}

extern "C" void surface_sound_set_volume(int line, long volume) {
    if (line < 0 || line > 1)
        return;
    if (volume < -10000)
        volume = -10000;
    if (volume > 0)
        volume = 0;
    long gain = (long) (((long long) (volume + 10000) * GAIN_ONE) / 10000);
    mix_lock();
    mix_line_volume[line] = volume;
    mix_line_gain[line] = gain;
    mix_unlock();
}

// ---------------------------------------------------------------------------
// Buffers

static SoundData* sound_data_create(const WAVEFORMATEX* format, DWORD size) {
    SoundData* data = new SoundData;
    if (data == 0)
        return 0;
    data->refs = 1;
    data->size = size;
    data->bytes = new unsigned char[size > 0 ? size : 1];
    if (data->bytes == 0) {
        delete data;
        return 0;
    }
    memset(data->bytes, format->wBitsPerSample == 8 ? 0x80 : 0, size);
    data->format = *format;
    data->format.cbSize = 0;
    if (data->format.nBlockAlign == 0)
        data->format.nBlockAlign = (WORD) (format->nChannels * format->wBitsPerSample / 8);
    return data;
}

static void sound_data_release(SoundData* data) {
    if (data != 0 && --data->refs == 0) {
        delete[] data->bytes;
        delete data;
    }
}

MemSoundBuffer::MemSoundBuffer(DWORD flags_in, SoundData* data_in) :
    refs(1), flags(flags_in), data(data_in), playing(0), looping(0), position(0), fraction(0), volume(0), pan(0),
    frequency(0), gain_left(GAIN_ONE), gain_right(GAIN_ONE), next_playing(0) {}

MemSoundBuffer::~MemSoundBuffer() {
    mix_lock();
    mix_remove(this);
    mix_unlock();
    sound_data_release(this->data);
}

// DirectSound volume and pan are attenuations in hundredths of a decibel;
// pan attenuates only the opposite channel.
void MemSoundBuffer::update_gains() {
    double base = pow(10.0, this->volume / 2000.0);
    double left = base;
    double right = base;
    if (this->pan > 0)
        left *= pow(10.0, -this->pan / 2000.0);
    else if (this->pan < 0)
        right *= pow(10.0, this->pan / 2000.0);
    long gl = (long) (left * GAIN_ONE + 0.5);
    long gr = (long) (right * GAIN_ONE + 0.5);
    mix_lock();
    this->gain_left = gl;
    this->gain_right = gr;
    mix_unlock();
}

STDMETHODIMP MemSoundBuffer::GetCaps(LPDSBCAPS caps) {
    if (caps == 0)
        return DSERR_INVALIDPARAM;
    caps->dwFlags = this->flags | DSBCAPS_LOCSOFTWARE;
    caps->dwBufferBytes = this->data != 0 ? this->data->size : 0;
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::GetCurrentPosition(LPDWORD play, LPDWORD write) {
    mix_lock();
    DWORD at = this->position;
    mix_unlock();
    if (play != 0)
        *play = at;
    if (write != 0) {
        DWORD lead = 0;
        DWORD size = 1;
        if (this->data != 0 && this->data->size > 0) {
            size = this->data->size;
            lead = this->data->format.nAvgBytesPerSec * WRITE_LEAD_MS / 1000;
            lead -= lead % this->frame_bytes();
        }
        *write = this->playing != 0 ? (at + lead) % size : at;
    }
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::GetFormat(LPWAVEFORMATEX format, DWORD size, LPDWORD written) {
    WAVEFORMATEX own;
    if (this->data != 0) {
        own = this->data->format;
    } else {
        memset(&own, 0, sizeof(own));
        own.wFormatTag = WAVE_FORMAT_PCM;
        own.nChannels = 2;
        own.nSamplesPerSec = MIX_RATE;
        own.wBitsPerSample = 16;
        own.nBlockAlign = 4;
        own.nAvgBytesPerSec = MIX_RATE * 4;
    }
    if (written != 0)
        *written = sizeof(own);
    if (format != 0)
        memcpy(format, &own, size < sizeof(own) ? size : sizeof(own));
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::GetStatus(LPDWORD status) {
    if (status == 0)
        return DSERR_INVALIDPARAM;
    mix_lock();
    DWORD value = 0;
    if (this->playing != 0)
        value = DSBSTATUS_PLAYING | (this->looping != 0 ? DSBSTATUS_LOOPING : 0);
    mix_unlock();
    *status = value;
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::Lock(
    DWORD offset, DWORD bytes, LPVOID* ptr1, LPDWORD bytes1, LPVOID* ptr2, LPDWORD bytes2, DWORD flags
) {
    if (this->data == 0)
        return DSERR_INVALIDCALL;
    DWORD size = this->data->size;
    if ((flags & DSBLOCK_FROMWRITECURSOR) != 0)
        this->GetCurrentPosition(0, &offset);
    if ((flags & DSBLOCK_ENTIREBUFFER) != 0)
        bytes = size;
    if (offset >= size || bytes > size || ptr1 == 0 || bytes1 == 0)
        return DSERR_INVALIDPARAM;
    *ptr1 = this->data->bytes + offset;
    if (offset + bytes <= size) {
        *bytes1 = bytes;
        if (ptr2 != 0)
            *ptr2 = 0;
        if (bytes2 != 0)
            *bytes2 = 0;
    } else {
        *bytes1 = size - offset;
        if (ptr2 != 0)
            *ptr2 = this->data->bytes;
        if (bytes2 != 0)
            *bytes2 = bytes - (size - offset);
    }
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::Play(DWORD, DWORD, DWORD flags) {
    // The primary buffer only exists to be played; nothing is mixed from it.
    if (this->data == 0)
        return DS_OK;
    mix_lock();
    this->looping = (flags & DSBPLAY_LOOPING) != 0;
    if (this->playing == 0) {
        this->playing = 1;
        this->next_playing = mix_playing;
        mix_playing = this;
    }
    mix_unlock();
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::SetCurrentPosition(DWORD position_in) {
    if (this->data == 0)
        return DS_OK;
    mix_lock();
    DWORD frame = this->frame_bytes();
    this->position = position_in < this->data->size ? position_in - position_in % frame : 0;
    this->fraction = 0;
    mix_unlock();
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::SetVolume(LONG volume_in) {
    if (volume_in > DSBVOLUME_MAX || volume_in < DSBVOLUME_MIN)
        return DSERR_INVALIDPARAM;
    this->volume = volume_in;
    this->update_gains();
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::SetPan(LONG pan_in) {
    if (pan_in > DSBPAN_RIGHT || pan_in < DSBPAN_LEFT)
        return DSERR_INVALIDPARAM;
    this->pan = pan_in;
    this->update_gains();
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::SetFrequency(DWORD frequency_in) {
    if (frequency_in != DSBFREQUENCY_ORIGINAL && (frequency_in < DSBFREQUENCY_MIN || frequency_in > DSBFREQUENCY_MAX))
        return DSERR_INVALIDPARAM;
    mix_lock();
    this->frequency = frequency_in;
    mix_unlock();
    return DS_OK;
}

STDMETHODIMP MemSoundBuffer::Stop() {
    mix_lock();
    if (this->playing != 0) {
        this->playing = 0;
        mix_remove(this);
    }
    mix_unlock();
    return DS_OK;
}

// ---------------------------------------------------------------------------
// Device

MemDirectSound::~MemDirectSound() {
    if (--mix_devices_open == 0)
        mix_close();
}

STDMETHODIMP MemDirectSound::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) {
    if (desc == 0 || out == 0)
        return DSERR_INVALIDPARAM;
    *out = 0;
    SoundData* data = 0;
    if ((desc->dwFlags & DSBCAPS_PRIMARYBUFFER) == 0) {
        const WAVEFORMATEX* format = desc->lpwfxFormat;
        if (format == 0 || format->wFormatTag != WAVE_FORMAT_PCM || (format->wBitsPerSample != 8 && format->wBitsPerSample != 16)
            || format->nChannels < 1 || format->nChannels > 2 || format->nSamplesPerSec == 0 || desc->dwBufferBytes == 0)
            return DSERR_INVALIDPARAM;
        data = sound_data_create(format, desc->dwBufferBytes);
        if (data == 0)
            return DSERR_OUTOFMEMORY;
    }
    MemSoundBuffer* buffer = new MemSoundBuffer(desc->dwFlags, data);
    if (buffer == 0) {
        sound_data_release(data);
        return DSERR_OUTOFMEMORY;
    }
    *out = buffer;
    return DS_OK;
}

STDMETHODIMP MemDirectSound::GetCaps(LPDSCAPS caps) {
    if (caps == 0)
        return DSERR_INVALIDPARAM;
    DWORD size = caps->dwSize != 0 && caps->dwSize < sizeof(DSCAPS) ? caps->dwSize : sizeof(DSCAPS);
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT
        | DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT
        | DSCAPS_SECONDARY16BIT;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    return DS_OK;
}

STDMETHODIMP MemDirectSound::DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPDIRECTSOUNDBUFFER* out) {
    MemSoundBuffer* source = (MemSoundBuffer*) original;
    if (source == 0 || out == 0 || source->data == 0)
        return DSERR_INVALIDPARAM;
    ++source->data->refs;
    MemSoundBuffer* copy = new MemSoundBuffer(source->flags, source->data);
    if (copy == 0) {
        sound_data_release(source->data);
        return DSERR_OUTOFMEMORY;
    }
    copy->volume = source->volume;
    copy->pan = source->pan;
    copy->frequency = source->frequency;
    copy->update_gains();
    *out = copy;
    return DS_OK;
}

extern "C" HRESULT WINAPI surface_DirectSoundCreate(LPGUID, LPDIRECTSOUND* out, LPUNKNOWN) {
    if (out == 0)
        return DSERR_INVALIDPARAM;
    *out = 0;
    if (mix_open() == 0)
        return DSERR_GENERIC; // no audio device: the game runs without sound
    MemDirectSound* device = new MemDirectSound();
    if (device == 0)
        return DSERR_OUTOFMEMORY;
    ++mix_devices_open;
    *out = device;
    return DS_OK;
}

