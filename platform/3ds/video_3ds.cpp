// 3DS framebuffer + GPU presentation (replaces posix/sdl/sdlvideo.cpp). Subsystems sheet: video.
// The software renderer draws an 8-bit canvas; Update() maps it through the gamma/flash palette to RGB565,
// the GPU tiles it into a texture (GX display transfer) and citro2d draws it scaled per the Video sheet.
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <string.h>
#include <malloc.h>

#include "doomtype.h"
#include "templates.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "v_palette.h"
#include "stats.h"
#include "hardware.h"
#include "m_argv.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"

EXTERN_CVAR (Float, Gamma)
EXTERN_CVAR (Float, rgamma)
EXTERN_CVAR (Float, ggamma)
EXTERN_CVAR (Float, bgamma)

CUSTOM_CVAR (Float, rgamma, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (screen != NULL) screen->SetGamma (Gamma);
}
CUSTOM_CVAR (Float, ggamma, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (screen != NULL) screen->SetGamma (Gamma);
}
CUSTOM_CVAR (Float, bgamma, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (screen != NULL) screen->SetGamma (Gamma);
}

// ==== GPU side ===================================================================

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Canvases are tiled on the CPU straight into the texture (8x8 tiles, Morton order inside a tile; the image
// fills the last Height memory rows, top row first, and samples upright with subtexture top = 1). The GX
// tiling transfer used before shifted the image by ~8 rows (status bar clipped, seen in a framebuffer dump).
struct GpuSurface
{
	C3D_Tex Tex;
	Tex3DS_SubTexture Sub;
	C2D_Image Image;
	u32 *XOff;
	int TexW, TexH, Width, Height;
	GPU_TEXCOLOR Format;
	bool Valid;
};

static bool GpuUp;
static C3D_RenderTarget *TopTarget, *BottomTarget;
static GpuSurface TopSurf, ViewSurf, BottomSurf;
static int GpuMode = -1;
static bool TopSmooth;

static inline u32 MortonX (u32 x) { return (x & 1) | ((x & 2) << 1) | ((x & 4) << 2); }
static inline u32 MortonY (u32 y) { return ((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3); }

static int Pow2 (int v)
{
	int p = 8;
	while (p < v) p <<= 1;
	return p;
}

static void SurfaceFree (GpuSurface &s)
{
	if (s.Valid)
	{
		C3D_TexDelete(&s.Tex);
		delete[] s.XOff;
		s.Valid = false;
	}
}

static void SurfaceAlloc (GpuSurface &s, int width, int height, bool smooth, GPU_TEXCOLOR fmt = GPU_RGB565)
{
	int tw = Pow2(width), th = Pow2(height);
	if (s.Valid && (s.TexW != tw || s.TexH != th || s.Format != fmt))
		SurfaceFree(s);
	if (!s.Valid)
	{
		C3D_TexInit(&s.Tex, tw, th, fmt);
		memset(s.Tex.data, 0, s.Tex.size);
		s.XOff = new u32[tw];
		for (int x = 0; x < tw; ++x)
			s.XOff[x] = ((x >> 3) << 6) | MortonX(x & 7);
		s.TexW = tw; s.TexH = th; s.Format = fmt;
		s.Valid = true;
	}
	C3D_TexSetFilter(&s.Tex, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
	s.Width = width; s.Height = height;
	s.Sub.width = width;
	s.Sub.height = height;
	s.Sub.left = 0.0f;
	s.Sub.top = 1.0f;		// top > bottom = upright (citro2d reads top < bottom as "rotated 90 degrees")
	s.Sub.right = width / (float)tw;
	s.Sub.bottom = 1.0f - height / (float)th;
	s.Image.tex = &s.Tex;
	s.Image.subtex = &s.Sub;
}

static void SurfaceUpload (GpuSurface &s, const unsigned char *pixels, int pitch, int width, int height, const unsigned short *pal)
{
	if (!s.Valid)
		return;
	width = MIN(width, s.Width);
	height = MIN(height, s.Height);
	u16 *tex = (u16 *)s.Tex.data;
	const u32 tilesPerRow = s.TexW >> 3;
	for (int y = 0; y < height; ++y)
	{
		// memory row 0 = Sub.top (t = 1): the image starts at the first memory row, top row first.
		// Verified with -texprobe dumps (row-index gradient) for 240/256 and 101/128 rows.
		const u32 ym = y;
		u16 *row = tex + ((((ym >> 3) * tilesPerRow) << 6) | MortonY(ym & 7));
		const unsigned char *src = pixels + y * pitch;
		for (int x = 0; x < width; ++x)
			row[s.XOff[x]] = pal[src[x]];
	}
	C3D_TexFlush(&s.Tex);
}

// DEBUG (-texprobe): replace a surface with a probe pattern (R5 = y & 31, G6 >> 3 = 32-px x band), nearest
// filtered, so a framebuffer dump shows exactly which image row lands on each screen row.
static void TexProbe (GpuSurface &s)
{
	static int probe = -1;
	if (probe < 0)
		probe = Args->CheckParm("-texprobe") ? 1 : 0;
	if (!probe || !s.Valid)
		return;
	static unsigned char rows[256 * 1024];
	static unsigned short pal[256];
	for (int i = 0; i < 256; ++i)
		pal[i] = (unsigned short)(((i & 31) << 11) | (((i >> 5) << 3) << 5));
	for (int y = 0; y < s.Height && y < 256; ++y)
		for (int x = 0; x < 1024; ++x)
			rows[y * 1024 + x] = (unsigned char)((y & 31) | (((x >> 5) & 7) << 5));	// x band = G6 >> 3
	SurfaceUpload(s, rows, 1024, s.Width, s.Height, pal);
	C3D_TexSetFilter(&s.Tex, GPU_NEAREST, GPU_NEAREST);
}

// Draw a surface scaled to fill (screenw x screenh).
static void DrawScaled (GpuSurface &s, float screenw, float screenh)
{
	C2D_DrawImageAt(s.Image, 0.0f, 0.0f, 0.5f, NULL, screenw / (float)s.Width, screenh / (float)s.Height);
}

void I3DS_GpuInit()
{
	if (GpuUp)
		return;
	// The startup console left the bottom screen as single-buffered RGB565; citro2d targets expect the
	// default BGR8 double-buffered framebuffers (mismatch = doubled, noisy bottom screen).
	gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
	gfxSetDoubleBuffering(GFX_BOTTOM, true);
	gfxSwapBuffers();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	BottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	SurfaceAlloc(BottomSurf, 320, 240, false);
	GpuUp = true;
}

void I3DS_GpuShutdown()
{
	if (!GpuUp)
		return;
	SurfaceFree(TopSurf);
	SurfaceFree(BottomSurf);
	SurfaceFree(ViewSurf);
	if (TopTarget) { C3D_RenderTargetDelete(TopTarget); TopTarget = NULL; }
	if (BottomTarget) { C3D_RenderTargetDelete(BottomTarget); BottomTarget = NULL; }
	C2D_Fini();
	C3D_Fini();
	gfxSetWide(false);
	gfxExit();
	GpuUp = false;
	GpuMode = -1;
}

bool I3DS_GpuActive()
{
	return GpuUp;
}

int I3DS_CurrentMode()
{
	return GpuMode;
}

void I3DS_GpuSetMode(int mode)
{
	if (!GpuUp || mode < 0)
		return;
	int w, h;
	I3DS_ModeSize(mode, &w, &h);
	// Video sheet 'scale': only a row that names gfxSetWide drives the 800px LCD mode; every other size is
	// stretched or downsampled onto the 400x240 target (always shows the whole picture, on any model)
	const char *scale = bd3ds_video_modes[mode].scale;
	bool wide = scale != NULL && strstr(scale, "gfxSetWide") != NULL;
	if (TopTarget == NULL || mode != GpuMode)
	{
		if (TopTarget)
		{
			C3D_FrameSync();
			C3D_RenderTargetDelete(TopTarget);
		}
		gfxSetWide(wide);
		TopTarget = C3D_RenderTargetCreate(240, wide ? 800 : 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		C3D_RenderTargetSetOutput(TopTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	}
	// sizes that map 1:1 onto the target are sharp, stretched/downsampled sizes are filtered
	TopSmooth = !(h == 240 && w == (wide ? 800 : 400));
	SurfaceAlloc(TopSurf, w, h, TopSmooth);
	GpuMode = mode;
}

void I3DS_GpuUploadTop(const unsigned char *pixels, int pitch, int width, int height, const unsigned short *pal565)
{
	SurfaceUpload(TopSurf, pixels, pitch, width, height, pal565);
}

void I3DS_GpuUploadBottom(const unsigned char *pixels, int pitch, int width, int height, const unsigned short *pal565)
{
	SurfaceUpload(BottomSurf, pixels, pitch, width, height, pal565);
}

// ---- render scale (Menu sheet: "Render scale" -> vid_3dsrenderscale) -------------------------
// Below 1, the 3D view renders into a smaller canvas; the GPU stretches it (smoothly) under the full-size
// HUD layer, whose view rectangle is filled with a transparent key color. HUD, status bar and menus stay sharp.

CUSTOM_CVAR (Float, vid_3dsrenderscale, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	float v = clamp<float>(self, 0.125f, 1.f);
	v = int(v * 8.f + 0.5f) / 8.f;	// 1/8 steps
	if (v != self) self = v;
}

static DSimpleCanvas *ViewCanvas;
static bool ViewThisFrame;
static int ViewX, ViewY, ViewW, ViewH;
static int KeyIndex = -1;

// A palette index whose color duplicates a lower index: truecolor graphics are matched to the first
// (lowest) index, so this one is effectively never drawn by the HUD and can mean "transparent".
static int FindKeyIndex ()
{
	for (int i = 255; i > 0; --i)
		for (int j = 0; j < i; ++j)
			if (GPalette.BaseColors[i].r == GPalette.BaseColors[j].r &&
				GPalette.BaseColors[i].g == GPalette.BaseColors[j].g &&
				GPalette.BaseColors[i].b == GPalette.BaseColors[j].b)
				return i;
	return 247;
}

float I3DS_RenderScale ()
{
	return GpuUp ? (float)vid_3dsrenderscale : 1.f;
}

// Canvas of exactly the (already scaled) view size the renderer computed.
DCanvas *I3DS_ScaledViewCanvas (int w, int h)
{
	w = MAX(w, 8);
	h = MAX(h, 8);
	if (ViewCanvas == NULL || ViewCanvas->GetWidth() != w || ViewCanvas->GetHeight() != h)
	{
		if (ViewCanvas != NULL)
		{
			GC::DelSoftRoot(ViewCanvas);
			ViewCanvas->Destroy();
		}
		ViewCanvas = new DSimpleCanvas(w, h);
		GC::AddSoftRoot(ViewCanvas);	// DObject: must be rooted or the GC frees it
	}
	return ViewCanvas;
}

void I3DS_ScaledViewDone (int x, int y, int w, int h)
{
	if (KeyIndex < 0)
		KeyIndex = FindKeyIndex();
	ViewThisFrame = true;
	if (x != ViewX || y != ViewY || w != ViewW || h != ViewH)
		DPrintf("[3DS] scaled view rect %d,%d %dx%d canvas %dx%d top %dx%d\n", x, y, w, h,
			ViewCanvas->GetWidth(), ViewCanvas->GetHeight(), TopSurf.Width, TopSurf.Height);
	ViewX = x; ViewY = y; ViewW = w; ViewH = h;
	screen->Clear(x, y, x + w, y + h, KeyIndex, 0);
}

void I3DS_GpuPresent()
{
	if (!GpuUp || TopTarget == NULL)
		return;
	int screenw = gfxIsWide() ? 800 : 400;
	I3DS_ProfileDump();
	if (!I3DS_FrameBeginSafe())	// never an unbounded VBlank wait: HOME must reach aptMainLoop (i_3ds.h)
		return;

	C2D_TargetClear(TopTarget, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(TopTarget);
	if (ViewThisFrame && ViewSurf.Valid && TopSurf.Valid)
	{
		float sx = screenw / (float)TopSurf.Width, sy = 240.0f / TopSurf.Height;
		C2D_DrawImageAt(ViewSurf.Image, ViewX * sx, ViewY * sy, 0.4f, NULL,
			ViewW * sx / ViewSurf.Width, ViewH * sy / ViewSurf.Height);
	}
	if (TopSurf.Valid)
		DrawScaled(TopSurf, (float)screenw, 240.0f);

	C2D_TargetClear(BottomTarget, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(BottomTarget);
	if (BottomSurf.Valid && I3DS_BottomCanvasVisible())
		DrawScaled(BottomSurf, 320.0f, 240.0f);
	I3DS_BottomOverlay();

	C3D_FrameEnd(0);
}

// ==== engine side: framebuffer ======================================================

class C3DSFrameBuffer : public DFrameBuffer
{
	DECLARE_CLASS(C3DSFrameBuffer, DFrameBuffer)
public:
	C3DSFrameBuffer (int width, int height);
	~C3DSFrameBuffer ();

	bool Lock (bool buffer);
	void Unlock ();
	bool Relock ();
	bool IsValid ();
	void Update ();
	PalEntry *GetPalette ();
	void GetFlashedPalette (PalEntry pal[256]);
	void UpdatePalette ();
	bool SetGamma (float gamma);
	bool SetFlash (PalEntry rgb, int amount);
	void GetFlash (PalEntry &rgb, int &amount);
	int GetPageCount ();
	bool IsFullscreen ();

private:
	PalEntry SourcePalette[256];
	BYTE GammaTable[3][256];
	unsigned short Pal565[256];
	unsigned short Pal5551[256];	// HUD layer when the render scale is below 1 (alpha bit; key index = 0)
	PalEntry Flash;
	int FlashAmount;
	float Gamma;
	bool UpdatePending;
	bool NeedPalUpdate;
	bool NeedGammaUpdate;

	void UpdateColors ();

	C3DSFrameBuffer () {}
};
IMPLEMENT_CLASS(C3DSFrameBuffer)

C3DSFrameBuffer::C3DSFrameBuffer (int width, int height)
	: DFrameBuffer (width, height)
{
	NeedPalUpdate = false;
	NeedGammaUpdate = false;
	UpdatePending = false;
	FlashAmount = 0;
	Gamma = 1.f;
	for (int i = 0; i < 256; i++)
	{
		GammaTable[0][i] = GammaTable[1][i] = GammaTable[2][i] = i;
	}
	memcpy (SourcePalette, GPalette.BaseColors, sizeof(PalEntry)*256);
	UpdateColors ();
	I3DS_GpuSetMode (I3DS_ModeForSize(width, height));
}

C3DSFrameBuffer::~C3DSFrameBuffer ()
{
}

bool C3DSFrameBuffer::IsValid ()
{
	return DFrameBuffer::IsValid() && I3DS_ModeForSize(Width, Height) >= 0;
}

int C3DSFrameBuffer::GetPageCount ()
{
	return 1;
}

bool C3DSFrameBuffer::IsFullscreen ()
{
	return true;
}

bool C3DSFrameBuffer::Lock (bool buffered)
{
	return DSimpleCanvas::Lock ();
}

bool C3DSFrameBuffer::Relock ()
{
	return DSimpleCanvas::Lock ();
}

void C3DSFrameBuffer::Unlock ()
{
	if (UpdatePending && LockCount == 1)
	{
		Update ();
	}
	else if (--LockCount <= 0)
	{
		Buffer = NULL;
		LockCount = 0;
	}
}

void C3DSFrameBuffer::Update ()
{
	if (LockCount != 1)
	{
		if (LockCount > 0)
		{
			UpdatePending = true;
			--LockCount;
		}
		return;
	}

	DrawRateStuff ();

	Buffer = NULL;
	LockCount = 0;
	UpdatePending = false;

	if (NeedGammaUpdate)
	{
		NeedGammaUpdate = false;
		CalcGamma ((rgamma == 0.f) ? Gamma : (Gamma * rgamma), GammaTable[0]);
		CalcGamma ((ggamma == 0.f) ? Gamma : (Gamma * ggamma), GammaTable[1]);
		CalcGamma ((bgamma == 0.f) ? Gamma : (Gamma * bgamma), GammaTable[2]);
		NeedPalUpdate = true;
	}
	if (NeedPalUpdate)
	{
		NeedPalUpdate = false;
		UpdateColors ();
	}

	if (ViewThisFrame && ViewCanvas != NULL)
	{
		// HUD layer with the view rectangle transparent + the reduced 3D view as its own smooth texture
		Pal5551[KeyIndex] = 0;
		ViewCanvas->Lock();
		{	// profiler: raw reduced view canvas next to the framebuffer dumps (8-bit + RGB565 palette)
			static unsigned n;
			if ((++n % 300) == 0 && Args->CheckParm("-profile3ds"))
			{
				FILE *f = fopen(BD3DS_PATH_APP_DIR "fb_view.raw", "wb");
				if (f)
				{
					u16 hdr[3] = { (u16)ViewCanvas->GetWidth(), (u16)ViewCanvas->GetHeight(), (u16)ViewCanvas->GetPitch() };
					fwrite(hdr, sizeof(hdr), 1, f);
					fwrite(Pal565, 2, 256, f);
					fwrite(ViewCanvas->GetBuffer(), 1, ViewCanvas->GetPitch() * ViewCanvas->GetHeight(), f);
					fclose(f);
				}
			}
		}
		SurfaceAlloc(ViewSurf, ViewCanvas->GetWidth(), ViewCanvas->GetHeight(), true);
		SurfaceUpload(ViewSurf, ViewCanvas->GetBuffer(), ViewCanvas->GetPitch(), ViewCanvas->GetWidth(), ViewCanvas->GetHeight(), Pal565);
		TexProbe(ViewSurf);
		ViewCanvas->Unlock();
		SurfaceAlloc(TopSurf, TopSurf.Width, TopSurf.Height, TopSmooth, GPU_RGBA5551);
		SurfaceUpload(TopSurf, MemBuffer, Pitch, Width, Height, Pal5551);
	}
	else
	{
		SurfaceAlloc(TopSurf, TopSurf.Width, TopSurf.Height, TopSmooth, GPU_RGB565);
		SurfaceUpload(TopSurf, MemBuffer, Pitch, Width, Height, Pal565);
		TexProbe(TopSurf);
	}
	I3DS_BottomRender (Pal565);
	I3DS_GpuPresent ();
	ViewThisFrame = false;
	I3DS_ProfileFrame ();
}

void C3DSFrameBuffer::UpdateColors ()
{
	PalEntry palette[256];
	for (int i = 0; i < 256; ++i)
	{
		palette[i].r = GammaTable[0][SourcePalette[i].r];
		palette[i].g = GammaTable[1][SourcePalette[i].g];
		palette[i].b = GammaTable[2][SourcePalette[i].b];
	}
	if (FlashAmount)
	{
		DoBlending (palette, palette, 256,
			GammaTable[0][Flash.r], GammaTable[1][Flash.g], GammaTable[2][Flash.b], FlashAmount);
	}
	for (int i = 0; i < 256; ++i)
	{
		Pal565[i] = ((palette[i].r >> 3) << 11) | ((palette[i].g >> 2) << 5) | (palette[i].b >> 3);
		Pal5551[i] = ((palette[i].r >> 3) << 11) | ((palette[i].g >> 3) << 6) | ((palette[i].b >> 3) << 1) | 1;
	}
}

PalEntry *C3DSFrameBuffer::GetPalette ()
{
	return SourcePalette;
}

void C3DSFrameBuffer::UpdatePalette ()
{
	NeedPalUpdate = true;
}

bool C3DSFrameBuffer::SetGamma (float gamma)
{
	Gamma = gamma;
	NeedGammaUpdate = true;
	return true;
}

bool C3DSFrameBuffer::SetFlash (PalEntry rgb, int amount)
{
	Flash = rgb;
	FlashAmount = amount;
	NeedPalUpdate = true;
	return true;
}

void C3DSFrameBuffer::GetFlash (PalEntry &rgb, int &amount)
{
	rgb = Flash;
	amount = FlashAmount;
}

void C3DSFrameBuffer::GetFlashedPalette (PalEntry pal[256])
{
	memcpy (pal, SourcePalette, 256*sizeof(PalEntry));
	if (FlashAmount)
	{
		DoBlending (pal, pal, 256, Flash.r, Flash.g, Flash.b, FlashAmount);
	}
}

// ==== IVideo ============================================================================

class C3DSVideo : public IVideo
{
public:
	EDisplayType GetDisplayType () { return DISPLAY_FullscreenOnly; }
	void SetWindowedScale (float scale) {}

	DFrameBuffer *CreateFrameBuffer (int width, int height, bool fs, DFrameBuffer *old)
	{
		PalEntry flashColor = 0;
		int flashAmount = 0;
		if (I3DS_ModeForSize(width, height) < 0)
		{
			I3DS_ModeSize(BD3DS_VIDEO_MODE_DEFAULT, &width, &height);
		}
		if (old != NULL)
		{
			if (old->GetWidth() == width && old->GetHeight() == height)
				return old;
			old->GetFlash (flashColor, flashAmount);
			old->ObjectFlags |= OF_YesReallyDelete;
			if (screen == old) screen = NULL;
			delete old;
		}
		C3DSFrameBuffer *fb = new C3DSFrameBuffer (width, height);
		fb->SetFlash (flashColor, flashAmount);
		return fb;
	}

	void StartModeIterator (int bits, bool fs)
	{
		IteratorMode = 0;
		IteratorBits = bits;
	}

	bool NextMode (int *width, int *height, bool *letterbox)
	{
		if (IteratorBits != 8)
			return false;
		while (IteratorMode < I3DS_ModeCount())
		{
			int m = IteratorMode++;
			if (I3DS_ModeAllowed(m))
			{
				I3DS_ModeSize(m, width, height);
				if (letterbox) *letterbox = false;
				return true;
			}
		}
		return false;
	}

private:
	int IteratorMode;
	int IteratorBits;
};

IVideo *I3DS_CreateVideo()
{
	return new C3DSVideo;
}
