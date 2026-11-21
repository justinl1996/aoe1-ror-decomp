// In-memory DirectDraw for the SDL build (workflow/docs/sdl_port_tasks.md,
// Task 1).
//
// The game draws 8-bit indexed pixels into DirectDraw surfaces it locks, and
// uses a small subset of DirectDraw: creating surfaces, a palette and a
// clipper, Lock/Unlock, Blt/BltFast with fills and source color keys, and
// GetDC for GDI text. This file implements those interfaces (as declared by
// the VC98 ddraw.h) over plain memory, so the game's call sites stay as they
// are. Only surface_DirectDrawCreate replaces DirectDrawCreate.
//
// Every surface's pixels live in an 8-bit top-down DIB section, so GetDC
// hands out a memory DC that draws straight into the pixels the game locks.
//
// The display mode is never changed. SetDisplayMode only records the mode,
// and the primary surface is a memory buffer of that size. Writes to the
// primary surface are shown with surface_present_* (port/sdl/surface.cpp).

#include <windows.h>
#include <ddraw.h>
#include <string.h>

#include "../../include/surface.h"


class MemDirectDraw;
class MemSurface;

// Value-initializing helper: the VC6 headers predate ZeroMemory on structs
// with constructors, and these are plain C structs.
#define MEM_CLEAR(p, n) memset((p), 0, (n))

class MemPalette : public IDirectDrawPalette {
  public:
    ULONG refs;
    PALETTEENTRY entries[256];
    MemDirectDraw* owner;

    MemPalette(MemDirectDraw* owner_in, LPPALETTEENTRY initial) : refs(1), owner(owner_in) {
        if (initial != 0)
            memcpy(this->entries, initial, sizeof(this->entries));
        else
            MEM_CLEAR(this->entries, sizeof(this->entries));
    }

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
    STDMETHOD(GetCaps)(LPDWORD caps) {
        *caps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
        return DD_OK;
    }
    STDMETHOD(GetEntries)(DWORD, DWORD start, DWORD count, LPPALETTEENTRY out) {
        if (start >= 256 || count > 256 - start)
            return DDERR_INVALIDPARAMS;
        memcpy(out, this->entries + start, count * sizeof(PALETTEENTRY));
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(SetEntries)(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in);
};

class MemClipper : public IDirectDrawClipper {
  public:
    ULONG refs;
    HWND wnd;

    MemClipper() : refs(1), wnd(0) {}

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
    STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD) { return DDERR_NOCLIPLIST; }
    STDMETHOD(GetHWnd)(HWND FAR* out) {
        *out = this->wnd;
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsClipListChanged)(BOOL FAR* changed) {
        *changed = FALSE;
        return DD_OK;
    }
    STDMETHOD(SetClipList)(LPRGNDATA, DWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(SetHWnd)(DWORD, HWND wnd_in) {
        this->wnd = wnd_in;
        return DD_OK;
    }
};

class MemSurface : public IDirectDrawSurface {
  public:
    ULONG refs;
    MemDirectDraw* owner;
    long width;
    long height;
    long pitch;
    DWORD caps;
    HBITMAP dib;
    HDC dc;
    HGDIOBJ old_bitmap;
    unsigned char* bits;
    int has_src_key;
    unsigned char src_key;
    int locked;
    int dc_out;
    MemPalette* palette;
    MemClipper* clipper;

    MemSurface(MemDirectDraw* owner_in, long width_in, long height_in, DWORD caps_in);
    ~MemSurface();
    int valid() const { return this->bits != 0; }
    int is_primary() const { return (this->caps & DDSCAPS_PRIMARYSURFACE) != 0; }
    void fill_desc(LPDDSURFACEDESC desc);
    void refresh_color_table();
    void changed(const RECT* rect);

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
    STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
    STDMETHOD(AddOverlayDirtyRect)(LPRECT) { return DDERR_UNSUPPORTED; }
    STDMETHOD(Blt)(LPRECT dest_rect, LPDIRECTDRAWSURFACE src, LPRECT src_rect, DWORD flags, LPDDBLTFX fx);
    STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src, LPRECT src_rect, DWORD trans);
    STDMETHOD(DeleteAttachedSurface)(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID, LPDDENUMSURFACESCALLBACK) { return DD_OK; }
    STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return DD_OK; }
    STDMETHOD(Flip)(LPDIRECTDRAWSURFACE, DWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetAttachedSurface)(LPDDSCAPS, LPDIRECTDRAWSURFACE FAR*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetBltStatus)(DWORD) { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDSCAPS out) {
        out->dwCaps = this->caps;
        return DD_OK;
    }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER FAR* out) {
        *out = this->clipper;
        if (this->clipper == 0)
            return DDERR_NOCLIPPERATTACHED;
        this->clipper->AddRef();
        return DD_OK;
    }
    STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) {
        if ((flags & DDCKEY_SRCBLT) == 0 || this->has_src_key == 0)
            return DDERR_NOCOLORKEY;
        key->dwColorSpaceLowValue = this->src_key;
        key->dwColorSpaceHighValue = this->src_key;
        return DD_OK;
    }
    STDMETHOD(GetDC)(HDC FAR* out);
    STDMETHOD(GetFlipStatus)(DWORD) { return DD_OK; }
    STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE FAR* out) {
        *out = this->palette;
        if (this->palette == 0)
            return DDERR_NOPALETTEATTACHED;
        this->palette->AddRef();
        return DD_OK;
    }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT format);
    STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC desc) {
        if (desc == 0)
            return DDERR_INVALIDPARAMS;
        this->fill_desc(desc);
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsLost)() { return DD_OK; }
    STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC desc, DWORD flags, HANDLE);
    STDMETHOD(ReleaseDC)(HDC dc_in);
    STDMETHOD(Restore)() { return DD_OK; }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER clipper_in);
    STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key);
    STDMETHOD(SetOverlayPosition)(LONG, LONG) { return DDERR_UNSUPPORTED; }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE palette_in);
    STDMETHOD(Unlock)(LPVOID);
    STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return DDERR_UNSUPPORTED; }
    STDMETHOD(UpdateOverlayDisplay)(DWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE) { return DDERR_UNSUPPORTED; }
};

class MemDirectDraw : public IDirectDraw {
  public:
    ULONG refs;
    HWND wnd;
    int exclusive;
    long mode_width;
    long mode_height;
    MemSurface* primary;

    MemDirectDraw() : refs(1), wnd(0), exclusive(0), primary(0) {
        this->mode_width = GetSystemMetrics(SM_CXSCREEN);
        this->mode_height = GetSystemMetrics(SM_CYSCREEN);
    }

    void fill_mode_desc(LPDDSURFACEDESC desc, long width, long height, DWORD bits);

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
    STDMETHOD(Compact)() { return DD_OK; }
    STDMETHOD(CreateClipper)(DWORD, LPDIRECTDRAWCLIPPER FAR* out, IUnknown FAR*) {
        *out = new MemClipper();
        return *out != 0 ? DD_OK : DDERR_OUTOFMEMORY;
    }
    STDMETHOD(CreatePalette)(DWORD, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE FAR* out, IUnknown FAR*) {
        *out = new MemPalette(this, entries);
        return *out != 0 ? DD_OK : DDERR_OUTOFMEMORY;
    }
    STDMETHOD(CreateSurface)(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE FAR* out, IUnknown FAR*);
    STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE FAR*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(EnumDisplayModes)(DWORD, LPDDSURFACEDESC, LPVOID context, LPDDENUMMODESCALLBACK callback);
    STDMETHOD(EnumSurfaces)(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) { return DDERR_UNSUPPORTED; }
    STDMETHOD(FlipToGDISurface)() { return DD_OK; }
    STDMETHOD(GetCaps)(LPDDCAPS driver, LPDDCAPS hel);
    STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC desc) {
        if (desc == 0)
            return DDERR_INVALIDPARAMS;
        this->fill_mode_desc(desc, this->mode_width, this->mode_height, 8);
        return DD_OK;
    }
    STDMETHOD(GetFourCCCodes)(LPDWORD count, LPDWORD) {
        *count = 0;
        return DD_OK;
    }
    STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE FAR*) { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetMonitorFrequency)(LPDWORD frequency) {
        *frequency = 60;
        return DD_OK;
    }
    STDMETHOD(GetScanLine)(LPDWORD) { return DDERR_UNSUPPORTED; }
    STDMETHOD(GetVerticalBlankStatus)(LPBOOL in_blank) {
        *in_blank = FALSE;
        return DD_OK;
    }
    STDMETHOD(Initialize)(GUID FAR*) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(RestoreDisplayMode)() { return DD_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND wnd_in, DWORD flags) {
        this->wnd = wnd_in;
        this->exclusive = (flags & DDSCL_EXCLUSIVE) != 0;
        return DD_OK;
    }
    STDMETHOD(SetDisplayMode)(DWORD width, DWORD height, DWORD bits) {
        // Only 8-bit modes exist here: the game renders nothing else.
        if (bits != 8 || width == 0 || height == 0)
            return DDERR_INVALIDMODE;
        this->mode_width = width;
        this->mode_height = height;
        return DD_OK;
    }
    STDMETHOD(WaitForVerticalBlank)(DWORD, HANDLE) { return DD_OK; }
};

// ---------------------------------------------------------------------------
// Palette

STDMETHODIMP MemPalette::SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in) {
    if (start >= 256 || count > 256 - start || in == 0)
        return DDERR_INVALIDPARAMS;
    memcpy(this->entries + start, in, count * sizeof(PALETTEENTRY));
    // On a real 8-bit display a palette change recolors the screen at once.
    MemSurface* primary = this->owner != 0 ? this->owner->primary : 0;
    if (primary != 0 && primary->palette == this) {
        primary->refresh_color_table();
        surface_present_palette(this->entries);
        primary->changed(0);
    }
    return DD_OK;
}

// ---------------------------------------------------------------------------
// Surface

MemSurface::MemSurface(MemDirectDraw* owner_in, long width_in, long height_in, DWORD caps_in) :
    refs(1), owner(owner_in), width(width_in), height(height_in), pitch((width_in + 3) & ~3), caps(caps_in), dib(0), dc(0),
    old_bitmap(0), bits(0), has_src_key(0), src_key(0), locked(0), dc_out(0), palette(0), clipper(0) {
    struct {
        BITMAPINFOHEADER header;
        RGBQUAD colors[256];
    } info;
    void* pixels = 0;

    MEM_CLEAR(&info, sizeof(info));
    info.header.biSize = sizeof(info.header);
    info.header.biWidth = width_in;
    info.header.biHeight = -height_in; // top-down, like a locked DirectDraw surface
    info.header.biPlanes = 1;
    info.header.biBitCount = 8;
    info.header.biCompression = BI_RGB;
    info.header.biClrUsed = 256;

    this->dc = CreateCompatibleDC(0);
    if (this->dc == 0)
        return;
    this->dib = CreateDIBSection(this->dc, (BITMAPINFO*) &info, DIB_RGB_COLORS, &pixels, 0, 0);
    if (this->dib == 0 || pixels == 0)
        return;
    this->old_bitmap = SelectObject(this->dc, this->dib);
    this->bits = (unsigned char*) pixels;
    memset(this->bits, 0, this->pitch * this->height);
}

MemSurface::~MemSurface() {
    if (this->owner != 0 && this->owner->primary == this)
        this->owner->primary = 0;
    if (this->palette != 0)
        this->palette->Release();
    if (this->clipper != 0)
        this->clipper->Release();
    if (this->dc != 0) {
        if (this->old_bitmap != 0)
            SelectObject(this->dc, this->old_bitmap);
        DeleteDC(this->dc);
    }
    if (this->dib != 0)
        DeleteObject(this->dib);
}

void MemSurface::fill_desc(LPDDSURFACEDESC desc) {
    DWORD size = desc->dwSize != 0 ? desc->dwSize : sizeof(DDSURFACEDESC);
    if (size > sizeof(DDSURFACEDESC))
        size = sizeof(DDSURFACEDESC);
    MEM_CLEAR(desc, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    desc->dwHeight = this->height;
    desc->dwWidth = this->width;
    desc->lPitch = this->pitch;
    desc->ddsCaps.dwCaps = this->caps;
    if (this->has_src_key != 0) {
        desc->dwFlags |= DDSD_CKSRCBLT;
        desc->ddckCKSrcBlt.dwColorSpaceLowValue = this->src_key;
        desc->ddckCKSrcBlt.dwColorSpaceHighValue = this->src_key;
    }
    this->GetPixelFormat(&desc->ddpfPixelFormat);
}

STDMETHODIMP MemSurface::GetPixelFormat(LPDDPIXELFORMAT format) {
    MEM_CLEAR(format, sizeof(DDPIXELFORMAT));
    format->dwSize = sizeof(DDPIXELFORMAT);
    format->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    format->dwRGBBitCount = 8;
    return DD_OK;
}

// GDI matches colors against the DIB color table. DirectDraw uses the
// surface's palette, or the primary surface's palette when the surface has
// none, so do the same.
void MemSurface::refresh_color_table() {
    RGBQUAD colors[256];
    MemPalette* source = this->palette;
    if (source == 0 && this->owner != 0 && this->owner->primary != 0)
        source = this->owner->primary->palette;
    if (source == 0 || this->dc == 0)
        return;
    for (int i = 0; i < 256; ++i) {
        colors[i].rgbRed = source->entries[i].peRed;
        colors[i].rgbGreen = source->entries[i].peGreen;
        colors[i].rgbBlue = source->entries[i].peBlue;
        colors[i].rgbReserved = 0;
    }
    SetDIBColorTable(this->dc, 0, 256, colors);
}

// A write to the primary surface is visible at once on a real display.
void MemSurface::changed(const RECT* rect) {
    if (!this->is_primary() || this->owner == 0)
        return;
    RECT whole;
    if (rect == 0) {
        whole.left = 0;
        whole.top = 0;
        whole.right = this->width;
        whole.bottom = this->height;
        rect = &whole;
    }
    surface_present_primary(this->bits, this->pitch, this->width, this->height, rect, this->dc);
}

STDMETHODIMP MemSurface::Lock(LPRECT rect, LPDDSURFACEDESC desc, DWORD, HANDLE) {
    if (desc == 0)
        return DDERR_INVALIDPARAMS;
    if (this->dc_out != 0)
        return DDERR_SURFACEBUSY;
    GdiFlush(); // finish any GDI drawing into the pixels first
    this->fill_desc(desc);
    desc->dwFlags |= DDSD_LPSURFACE;
    unsigned char* start = this->bits;
    if (rect != 0) {
        if (rect->left < 0 || rect->top < 0 || rect->right > this->width || rect->bottom > this->height
            || rect->left > rect->right || rect->top > rect->bottom)
            return DDERR_INVALIDRECT;
        start += rect->top * this->pitch + rect->left;
    }
    desc->lpSurface = start;
    this->locked = 1;
    return DD_OK;
}

STDMETHODIMP MemSurface::Unlock(LPVOID) {
    if (this->locked == 0)
        return DDERR_NOTLOCKED;
    this->locked = 0;
    this->changed(0);
    return DD_OK;
}

STDMETHODIMP MemSurface::GetDC(HDC FAR* out) {
    if (out == 0)
        return DDERR_INVALIDPARAMS;
    if (this->dc_out != 0)
        return DDERR_DCALREADYCREATED;
    if (this->locked != 0)
        return DDERR_SURFACEBUSY;
    if (this->dc == 0)
        return DDERR_CANTCREATEDC;
    this->refresh_color_table();
    this->dc_out = 1;
    *out = this->dc;
    return DD_OK;
}

STDMETHODIMP MemSurface::ReleaseDC(HDC) {
    if (this->dc_out == 0)
        return DDERR_GENERIC;
    GdiFlush();
    this->dc_out = 0;
    this->changed(0);
    return DD_OK;
}

STDMETHODIMP MemSurface::SetClipper(LPDIRECTDRAWCLIPPER clipper_in) {
    MemClipper* next = (MemClipper*) clipper_in;
    if (next != 0)
        next->AddRef();
    if (this->clipper != 0)
        this->clipper->Release();
    this->clipper = next;
    return DD_OK;
}

STDMETHODIMP MemSurface::SetPalette(LPDIRECTDRAWPALETTE palette_in) {
    MemPalette* next = (MemPalette*) palette_in;
    if (next != 0)
        next->AddRef();
    if (this->palette != 0)
        this->palette->Release();
    this->palette = next;
    this->refresh_color_table();
    if (this->is_primary() && next != 0) {
        surface_present_palette(next->entries);
        this->changed(0);
    }
    return DD_OK;
}

STDMETHODIMP MemSurface::SetColorKey(DWORD flags, LPDDCOLORKEY key) {
    // Only source blit keys affect drawing; overlays do not exist here.
    if ((flags & DDCKEY_SRCBLT) != 0) {
        this->has_src_key = key != 0;
        if (key != 0)
            this->src_key = (unsigned char) key->dwColorSpaceLowValue;
    }
    return DD_OK;
}

// Copy (or stretch) a rectangle, optionally skipping source pixels equal to
// the key. Rectangles are already clipped. Copies within one surface may
// overlap (the scrolling view does that in video memory), so rows run in the
// direction that never reads a row already written.
static void mem_copy_rect(
    MemSurface* dest, const RECT* d, MemSurface* src, const RECT* s, int use_key, unsigned char key
) {
    long dw = d->right - d->left;
    long dh = d->bottom - d->top;
    long sw = s->right - s->left;
    long sh = s->bottom - s->top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    if (dw == sw && dh == sh) {
        int upward = dest == src && d->top > s->top;
        for (long i = 0; i < dh; ++i) {
            long row = upward ? dh - 1 - i : i;
            unsigned char* out = dest->bits + (d->top + row) * dest->pitch + d->left;
            const unsigned char* in = src->bits + (s->top + row) * src->pitch + s->left;
            if (use_key == 0) {
                memmove(out, in, dw);
            } else if (dest == src && out > in) {
                for (long x = dw - 1; x >= 0; --x)
                    if (in[x] != key)
                        out[x] = in[x];
            } else {
                for (long x = 0; x < dw; ++x)
                    if (in[x] != key)
                        out[x] = in[x];
            }
        }
        return;
    }

    // Stretching (nearest neighbour). No call site is known to stretch.
    for (long y = 0; y < dh; ++y) {
        const unsigned char* in = src->bits + (s->top + y * sh / dh) * src->pitch + s->left;
        unsigned char* out = dest->bits + (d->top + y) * dest->pitch + d->left;
        for (long x = 0; x < dw; ++x) {
            unsigned char c = in[x * sw / dw];
            if (use_key == 0 || c != key)
                out[x] = c;
        }
    }
}

static int mem_rect_inside(const RECT* r, long width, long height) {
    return r->left >= 0 && r->top >= 0 && r->right <= width && r->bottom <= height && r->left <= r->right
        && r->top <= r->bottom;
}

STDMETHODIMP MemSurface::Blt(LPRECT dest_rect, LPDIRECTDRAWSURFACE src_in, LPRECT src_rect, DWORD flags, LPDDBLTFX fx) {
    RECT d;
    if (dest_rect != 0) {
        d = *dest_rect;
    } else {
        d.left = 0;
        d.top = 0;
        d.right = this->width;
        d.bottom = this->height;
    }
    if (d.left > d.right || d.top > d.bottom)
        return DDERR_INVALIDRECT;
    if (this->dc_out != 0)
        GdiFlush();

    if ((flags & DDBLT_COLORFILL) != 0) {
        if (fx == 0)
            return DDERR_INVALIDPARAMS;
        if (this->clipper != 0) {
            if (d.left < 0)
                d.left = 0;
            if (d.top < 0)
                d.top = 0;
            if (d.right > this->width)
                d.right = this->width;
            if (d.bottom > this->height)
                d.bottom = this->height;
        } else if (!mem_rect_inside(&d, this->width, this->height)) {
            return DDERR_INVALIDRECT;
        }
        unsigned char color = (unsigned char) fx->dwFillColor;
        for (long y = d.top; y < d.bottom; ++y)
            memset(this->bits + y * this->pitch + d.left, color, d.right - d.left);
        this->changed(&d);
        return DD_OK;
    }

    MemSurface* src = (MemSurface*) src_in;
    if (src == 0)
        return DDERR_INVALIDPARAMS;
    RECT s;
    if (src_rect != 0) {
        s = *src_rect;
    } else {
        s.left = 0;
        s.top = 0;
        s.right = src->width;
        s.bottom = src->height;
    }
    if (!mem_rect_inside(&s, src->width, src->height))
        return DDERR_INVALIDRECT;

    // With a clipper attached DirectDraw clips the destination (and so the
    // source, in proportion); without one, a rectangle off the surface fails.
    if (this->clipper != 0) {
        long dw = d.right - d.left;
        long dh = d.bottom - d.top;
        long sw = s.right - s.left;
        long sh = s.bottom - s.top;
        if (dw <= 0 || dh <= 0)
            return DD_OK;
        if (d.left < 0) {
            s.left += (0 - d.left) * sw / dw;
            d.left = 0;
        }
        if (d.top < 0) {
            s.top += (0 - d.top) * sh / dh;
            d.top = 0;
        }
        if (d.right > this->width) {
            s.right -= (d.right - this->width) * sw / dw;
            d.right = this->width;
        }
        if (d.bottom > this->height) {
            s.bottom -= (d.bottom - this->height) * sh / dh;
            d.bottom = this->height;
        }
    } else if (!mem_rect_inside(&d, this->width, this->height)) {
        return DDERR_INVALIDRECT;
    }

    int use_key = (flags & DDBLT_KEYSRC) != 0 && src->has_src_key != 0;
    mem_copy_rect(this, &d, src, &s, use_key, src->src_key);
    this->changed(&d);
    return DD_OK;
}

STDMETHODIMP MemSurface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE src_in, LPRECT src_rect, DWORD trans) {
    MemSurface* src = (MemSurface*) src_in;
    if (src == 0)
        return DDERR_INVALIDPARAMS;
    // BltFast never clips; DirectDraw refuses it on a surface with a clipper.
    if (this->clipper != 0)
        return DDERR_BLTFASTCANTCLIP;
    RECT s;
    if (src_rect != 0) {
        s = *src_rect;
    } else {
        s.left = 0;
        s.top = 0;
        s.right = src->width;
        s.bottom = src->height;
    }
    RECT d;
    d.left = (long) x;
    d.top = (long) y;
    d.right = d.left + (s.right - s.left);
    d.bottom = d.top + (s.bottom - s.top);
    if (!mem_rect_inside(&s, src->width, src->height) || !mem_rect_inside(&d, this->width, this->height))
        return DDERR_INVALIDRECT;
    if (this->dc_out != 0)
        GdiFlush();

    int use_key = (trans & DDBLTFAST_SRCCOLORKEY) != 0 && src->has_src_key != 0;
    mem_copy_rect(this, &d, src, &s, use_key, src->src_key);
    this->changed(&d);
    return DD_OK;
}

// ---------------------------------------------------------------------------
// DirectDraw object

void MemDirectDraw::fill_mode_desc(LPDDSURFACEDESC desc, long width, long height, DWORD bits) {
    DWORD size = desc->dwSize != 0 ? desc->dwSize : sizeof(DDSURFACEDESC);
    if (size > sizeof(DDSURFACEDESC))
        size = sizeof(DDSURFACEDESC);
    MEM_CLEAR(desc, size);
    desc->dwSize = size;
    desc->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    desc->dwWidth = width;
    desc->dwHeight = height;
    desc->lPitch = width * (bits / 8);
    desc->dwRefreshRate = 60;
    desc->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc->ddpfPixelFormat.dwFlags = bits == 8 ? (DDPF_RGB | DDPF_PALETTEINDEXED8) : DDPF_RGB;
    desc->ddpfPixelFormat.dwRGBBitCount = bits;
    if (bits == 16) {
        desc->ddpfPixelFormat.dwRBitMask = 0xF800;
        desc->ddpfPixelFormat.dwGBitMask = 0x07E0;
        desc->ddpfPixelFormat.dwBBitMask = 0x001F;
    }
}

STDMETHODIMP MemDirectDraw::CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE FAR* out, IUnknown FAR*) {
    if (desc == 0 || out == 0)
        return DDERR_INVALIDPARAMS;
    *out = 0;
    DWORD requested = (desc->dwFlags & DDSD_CAPS) != 0 ? desc->ddsCaps.dwCaps : 0;
    long width;
    long height;
    DWORD caps;

    if ((requested & DDSCAPS_PRIMARYSURFACE) != 0) {
        if (this->primary != 0)
            return DDERR_PRIMARYSURFACEALREADYEXISTS;
        // In exclusive mode the primary surface is the (recorded) display
        // mode. Otherwise it is the desktop, and the game blits to it in
        // screen coordinates.
        if (this->exclusive != 0) {
            width = this->mode_width;
            height = this->mode_height;
        } else {
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
        }
        caps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    } else {
        if ((desc->dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) != (DDSD_WIDTH | DDSD_HEIGHT) || desc->dwWidth == 0
            || desc->dwHeight == 0)
            return DDERR_INVALIDPARAMS;
        width = desc->dwWidth;
        height = desc->dwHeight;
        caps = requested;
        if ((caps & DDSCAPS_SYSTEMMEMORY) == 0)
            caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    }

    MemSurface* surface = new MemSurface(this, width, height, caps);
    if (surface == 0)
        return DDERR_OUTOFMEMORY;
    if (!surface->valid()) {
        surface->Release();
        return DDERR_OUTOFMEMORY;
    }
    if (surface->is_primary()) {
        this->primary = surface;
        surface_present_set_target(this->wnd, this->exclusive, width, height);
    }
    *out = surface;
    return DD_OK;
}

STDMETHODIMP MemDirectDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID context, LPDDENUMMODESCALLBACK callback) {
    static const long sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};
    static const DWORD depths[] = {8, 16};
    DDSURFACEDESC desc;

    if (callback == 0)
        return DDERR_INVALIDPARAMS;
    for (int d = 0; d < 2; ++d) {
        for (int i = 0; i < 4; ++i) {
            desc.dwSize = sizeof(desc);
            this->fill_mode_desc(&desc, sizes[i][0], sizes[i][1], depths[d]);
            if (callback(&desc, context) == DDENUMRET_CANCEL)
                return DD_OK;
        }
    }
    return DD_OK;
}

STDMETHODIMP MemDirectDraw::GetCaps(LPDDCAPS driver, LPDDCAPS hel) {
    LPDDCAPS both[2] = {driver, hel};
    for (int i = 0; i < 2; ++i) {
        LPDDCAPS caps = both[i];
        if (caps == 0)
            continue;
        DWORD size = caps->dwSize != 0 ? caps->dwSize : sizeof(DDCAPS);
        if (size > sizeof(DDCAPS))
            size = sizeof(DDCAPS);
        MEM_CLEAR(caps, size);
        caps->dwSize = size;
        caps->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY | DDCAPS_PALETTE;
        caps->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        caps->dwPalCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
    }
    return DD_OK;
}

extern "C" HRESULT WINAPI surface_DirectDrawCreate(GUID FAR*, LPDIRECTDRAW FAR* out, IUnknown FAR*) {
    if (out == 0)
        return DDERR_INVALIDPARAMS;
    *out = new MemDirectDraw();
    return *out != 0 ? DD_OK : DDERR_OUTOFMEMORY;
}

