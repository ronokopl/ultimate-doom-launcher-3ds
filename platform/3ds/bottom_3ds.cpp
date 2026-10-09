// 3DS bottom screen (Subsystems sheet: bottom). Modes = BottomModes sheet, chosen by the bottom_mode cvar
// (3DS Options); every element = a BottomScreen sheet row listing the modes it appears in:
//   automap  the engine draws the live automap into an 8-bit canvas (only in modes with canvas = yes)
//   button   GPU-drawn button from Doom graphics; "slot N" buttons show owned/current state; +commands are held
//   aim      stylus touchpad area (command aim = always, aim_if_enabled = when Touch aiming is on)
//   status   big health / armor / ammo / weapon / keys readouts
//   art      the game's title picture, dimmed 15%
// Graphics are converted once from engine textures to RGBA8 GPU textures (cached by texture).
#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include <stdio.h>
#include <map>

#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "v_video.h"
#include "textures/textures.h"
#include "textures/bitmap.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "am_map.h"
#include "menu/menu.h"
#include "dobject.h"
#include "gi.h"
#include "r_data/sprites.h"
#include "r_state.h"
#include "g_shared/a_pickups.h"
#include "g_shared/a_keys.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"

CVAR (Int, bottom_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static DSimpleCanvas *BottomCanvas;
static int Pressed = -1;		// element held by the stylus
static bool TouchAiming;
static int LastX, LastY;
static int AutomapH = 184;
static int BacklightMode = -1;	// mode whose backlight state is applied
static bool BacklightOff;		// Screen off mode switched the bottom backlight off
static C2D_TextBuf TextBuf;

static int Mode()
{
	int m = bottom_mode;
	return m < 0 || m >= BD3DS_BOTTOM_MODE_COUNT ? 0 : m;
}

static bool InMode(const bd3ds_touch_t &t, int mode)
{
	const char *name = bd3ds_bottom_modes[mode].mode, *p = t.modes;
	size_t n = strlen(name);
	while ((p = strstr(p, name)) != NULL)
	{
		if ((p == t.modes || p[-1] == ' ') && (p[n] == 0 || p[n] == ' '))
			return true;
		p += n;
	}
	return false;
}

static bool Is(const char *a, const char *b) { return a != NULL && strcmp(a, b) == 0; }

// ---- Doom graphics as GPU textures ------------------------------------------------------------

struct DoomTex
{
	C3D_Tex Tex;
	Tex3DS_SubTexture Sub;
	C2D_Image Img;
	int W, H;
};

static std::map<FTexture *, DoomTex *> TexCache;

static inline u32 MortonX (u32 x) { return (x & 1) | ((x & 2) << 1) | ((x & 4) << 2); }
static inline u32 MortonY (u32 y) { return ((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3); }

static int Pow2 (int v)
{
	int p = 8;
	while (p < v) p <<= 1;
	return p;
}

// Engine texture -> RGBA8 GPU texture (same tiling convention as video_3ds.cpp: memory row 0 = Sub.top)
static DoomTex *GpuTex(FTexture *t)
{
	if (t == NULL)
		return NULL;
	std::map<FTexture *, DoomTex *>::iterator it = TexCache.find(t);
	if (it != TexCache.end())
		return it->second;
	DoomTex *out = NULL;
	int w = t->GetWidth(), h = t->GetHeight();
	FBitmap bmp;
	if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && bmp.Create(w, h))
	{
		t->CopyTrueColorPixels(&bmp, 0, 0);
		int tw = Pow2(w), th = Pow2(h);
		out = new DoomTex;
		if (!C3D_TexInit(&out->Tex, tw, th, GPU_RGBA8))
		{
			delete out;
			out = NULL;
		}
		else
		{
			memset(out->Tex.data, 0, out->Tex.size);
			u32 *dst = (u32 *)out->Tex.data;
			const BYTE *src = bmp.GetPixels();	// BGRA
			for (int y = 0; y < h; ++y)
			{
				u32 rowbase = (((y >> 3) * (tw >> 3)) << 6) | MortonY(y & 7);
				for (int x = 0; x < w; ++x)
				{
					const BYTE *p = src + (y * w + x) * 4;
					dst[rowbase | (((x >> 3) << 6) | MortonX(x & 7))] = (p[2] << 24) | (p[1] << 16) | (p[0] << 8) | p[3];
				}
			}
			C3D_TexFlush(&out->Tex);
			C3D_TexSetFilter(&out->Tex, GPU_LINEAR, GPU_NEAREST);
			out->Sub.width = w; out->Sub.height = h;
			out->Sub.left = 0.f; out->Sub.top = 1.f;
			out->Sub.right = w / (float)tw; out->Sub.bottom = 1.f - h / (float)th;
			out->Img.tex = &out->Tex; out->Img.subtex = &out->Sub;
			out->W = w; out->H = h;
		}
	}
	TexCache[t] = out;	// failures are cached too
	return out;
}

static FTexture *TexByName(const char *name)
{
	if (name == NULL || !strcmp(name, "~"))
		return NULL;
	FTextureID id = TexMan.CheckForTexture(name, FTexture::TEX_Any);
	return id.isValid() ? TexMan[id] : NULL;
}

static void FreeGfx()
{
	for (std::map<FTexture *, DoomTex *>::iterator it = TexCache.begin(); it != TexCache.end(); ++it)
		if (it->second != NULL)
		{
			C3D_TexDelete(&it->second->Tex);
			delete it->second;
		}
	TexCache.clear();
}

// Draw a texture scaled to fit (w x h), centred, keeping its aspect.
static void DrawFit(DoomTex *d, float x, float y, float w, float h, float z, const C2D_ImageTint *tint = NULL)
{
	if (d == NULL)
		return;
	float s = MIN(w / d->W, h / d->H);
	C2D_DrawImageAt(d->Img, x + (w - d->W * s) / 2, y + (h - d->H * s) / 2, z, tint, s, s);
}

// ---- text: Doom fonts, system font when the game has none -------------------------------------

// Big status digits (STTNUM0..9, STTPRCNT, STTMINUS) or the HUD font (STCFNnnn), height px, centred in w.
static float DrawDoomText(const char *font, const char *text, float x, float y, float w, float h, float z,
	const C2D_ImageTint *tint = NULL)
{
	bool big = !strcmp(font, "STTNUM");
	DoomTex *g[64];
	int n = 0;
	float total = 0, ref = 0;
	for (const char *c = text; *c && n < 64; ++c)
	{
		char name[16];
		if (big)
			mysnprintf(name, countof(name), *c == '%' ? "STTPRCNT" : *c == '-' ? "STTMINUS" : "STTNUM%c", *c);
		else
			mysnprintf(name, countof(name), "STCFN%03d", toupper(*c));
		g[n] = *c == ' ' ? NULL : GpuTex(TexByName(name));
		if (g[n] != NULL)
			ref = MAX(ref, (float)g[n]->H);
		++n;
	}
	if (ref == 0)
	{	// no Doom font (Heretic, Hexen, ...): system font
		C2D_Text t;
		C2D_TextParse(&t, TextBuf, text);
		C2D_TextOptimize(&t);
		float tw, th, s = h / 30.f;
		C2D_TextGetDimensions(&t, s, s, &tw, &th);
		C2D_DrawText(&t, C2D_WithColor, x + (w - tw) / 2, y, z, s, s, C2D_Color32(220, 30, 20, 255));
		return tw;
	}
	float s = h / ref, space = big ? 0.f : 4.f;
	for (int i = 0; i < n; ++i)
		total += g[i] ? g[i]->W * s + s : space * s;
	if (total > w)
		s *= w / total, total = w;
	float cx = x + (w - total) / 2;
	for (int i = 0; i < n; ++i)
	{
		if (g[i] == NULL) { cx += space * s; continue; }
		C2D_DrawImageAt(g[i]->Img, cx, y + (h - g[i]->H * s), z, tint, s, s);
		cx += g[i]->W * s + s;
	}
	return total;
}

// ---- weapon state ------------------------------------------------------------------------------

static int SlotOf(const bd3ds_touch_t &t)
{
	int slot = 0;
	return (t.command != NULL && sscanf(t.command, "slot %d", &slot) == 1) ? slot : -1;
}

static bool SlotOwned(int slot)
{
	player_t *p = &players[consoleplayer];
	if (p->mo == NULL || slot < 0 || slot >= NUM_WEAPON_SLOTS)
		return false;
	FWeaponSlot &ws = p->weapons.Slots[slot];
	for (int j = 0; j < ws.Size(); ++j)
	{
		const PClass *cls = ws.GetWeapon(j);
		if (cls != NULL && p->mo->FindInventory(cls) != NULL)
			return true;
	}
	return false;
}

static int CurrentSlot()
{
	player_t *p = &players[consoleplayer];
	int slot, index;
	if (p->ReadyWeapon != NULL && p->weapons.LocateWeapon(p->ReadyWeapon->GetClass(), &slot, &index))
		return slot;
	return -1;
}

// ---- backlight ---------------------------------------------------------------------------------

// gsp::Lcd is opened only around the call: the service takes very few sessions and the HOME Menu opens one when it
// takes over. A session kept open for the whole game (opened on the first frame, even in Automap mode) left the
// HOME Menu waiting for it forever - the console lock-up on HOME that only happened in game.
static void SetBacklight(bool on)
{
	if (R_SUCCEEDED(gspLcdInit()))
	{
		on ? GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM) : GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
		gspLcdExit();
	}
	BacklightOff = !on;
}

static void ApplyBacklight(int mode)
{
	if (mode == BacklightMode)
		return;
	bool on = bd3ds_bottom_modes[mode].backlight != 0;
	if (on == BacklightOff)	// only an actual change touches the LCD service (it is on unless we turned it off)
		SetBacklight(on);
	BacklightMode = mode;
}

// ---- screen ------------------------------------------------------------------------------------

// The canvas is a DObject: it must be a GC soft root, or ZDoom's collector frees it under us.
static void EnsureCanvas()
{
	if (BottomCanvas == NULL)
	{
		BottomCanvas = new DSimpleCanvas(320, 240);
		GC::AddSoftRoot(BottomCanvas);
	}
}

void I3DS_BottomInit()
{
	for (int i = 0; i < BD3DS_BOTTOM_COUNT; ++i)
		if (Is(bd3ds_bottom[i].kind, "automap"))
			AutomapH = bd3ds_bottom[i].h;
	if (TextBuf == NULL)
		TextBuf = C2D_TextBufNew(1024);
}

// Screen off mode must never outlive the game (quit, error screen) or show under the HOME Menu (APT suspend hook);
// the next frame applies the mode again.
void I3DS_BottomBacklightRestore()
{
	if (BacklightOff)
		SetBacklight(true);
	BacklightMode = -1;
}

void I3DS_BottomShutdown()
{
	FreeGfx();
	I3DS_BottomBacklightRestore();
	I3DS_AimShutdown();
	if (TextBuf != NULL)
	{
		C2D_TextBufDelete(TextBuf);
		TextBuf = NULL;
	}
	if (BottomCanvas != NULL)
	{
		GC::DelSoftRoot(BottomCanvas);
		BottomCanvas->Destroy();
		BottomCanvas = NULL;
	}
}

bool I3DS_BottomCanvasVisible()
{
	return bd3ds_bottom_modes[Mode()].canvas != 0;
}

void I3DS_BottomRender(const unsigned short *pal565)
{
	ApplyBacklight(Mode());
	if (!I3DS_BottomCanvasVisible())
		return;	// no automap work in the other modes
	EnsureCanvas();
	BottomCanvas->Lock();
	BottomCanvas->Clear(0, 0, 320, 240, 0, 0);
	if (gamestate == GS_LEVEL)
	{
		AM_DrawOnCanvas(BottomCanvas, 320, AutomapH);
		BottomCanvas->Clear(0, AutomapH, 320, 240, 0, 0);
	}
	I3DS_GpuUploadBottom(BottomCanvas->GetBuffer(), BottomCanvas->GetPitch(), 320, 240, pal565);
	BottomCanvas->Unlock();
}

static const u32 COL_LIGHT = C2D_Color32(190, 190, 190, 255), COL_DARK = C2D_Color32(16, 16, 16, 255);

static void Bevel(float x, float y, float w, float h, float z)
{
	C2D_DrawRectSolid(x, y, z, w, 1, COL_LIGHT);
	C2D_DrawRectSolid(x, y, z, 1, h, COL_LIGHT);
	C2D_DrawRectSolid(x, y + h - 1, z, w, 1, COL_DARK);
	C2D_DrawRectSolid(x + w - 1, y, z, 1, h, COL_DARK);
}

static void DrawButton(int i, int current)
{
	const bd3ds_touch_t &t = bd3ds_bottom[i];
	float x = t.x, y = t.y, w = t.w, h = t.h;
	DoomTex *face = GpuTex(TexByName(t.texture));
	if (face != NULL)
		C2D_DrawImageAt(face->Img, x, y, 0.5f, NULL, w / face->W, h / face->H);
	else
		C2D_DrawRectSolid(x, y, 0.5f, w, h, C2D_Color32(70, 70, 70, 255));
	Bevel(x, y, w, h, 0.6f);
	int slot = SlotOf(t);
	if (slot >= 0 && slot == current)
		C2D_DrawRectSolid(x + 1, y + 1, 0.55f, w - 2, h - 2, C2D_Color32(255, 220, 0, 70));
	if (i == Pressed)
		C2D_DrawRectSolid(x + 1, y + 1, 0.55f, w - 2, h - 2, C2D_Color32(255, 0, 0, 110));
	if (t.label == NULL)
		return;
	bool owned = slot < 0 || SlotOwned(slot);
	C2D_ImageTint tint;	// an empty slot drawn with the same font is darkened
	C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, 255), !owned && Is(t.font_owned, t.font_empty) ? 0.65f : 0.0f);
	const char *font = owned ? t.font_owned : t.font_empty;
	bool big = Is(font, "STTNUM");
	float th = h * (big ? 0.45f : 0.28f);
	DrawDoomText(font ? font : "STCFN", t.label, x + 3, y + (h - th) / 2, w - 6, th, 0.7f, &tint);
}

static void DrawPanel(const bd3ds_touch_t &t)
{
	C2D_DrawRectSolid(t.x, t.y, 0.4f, t.w, t.h, C2D_Color32(48, 40, 36, 255));
	Bevel(t.x, t.y, t.w, t.h, 0.45f);
}

static void DrawStatus(const bd3ds_touch_t &t)
{
	player_t *p = &players[consoleplayer];
	if (p->mo == NULL)
		return;
	DrawPanel(t);
	const char *labelfont = t.font_empty ? t.font_empty : "STCFN";
	DrawDoomText(labelfont, t.label, t.x + 4, t.y + 5, t.w - 8, 9, 0.6f);
	char num[16];
	float vx = t.x + 6, vy = t.y + 20, vw = t.w - 12, vh = t.h - 28;
	if (Is(t.element, "st_health"))
	{
		mysnprintf(num, countof(num), "%d%%", MAX(p->health, 0));
		DrawDoomText("STTNUM", num, vx, vy, vw, vh, 0.6f);
	}
	else if (Is(t.element, "st_armor"))
	{
		ABasicArmor *armor = p->mo->FindInventory<ABasicArmor>();
		mysnprintf(num, countof(num), "%d%%", armor != NULL ? armor->Amount : 0);
		DrawDoomText("STTNUM", num, vx, vy, vw, vh, 0.6f);
	}
	else if (Is(t.element, "st_ammo"))
	{
		AWeapon *w = p->ReadyWeapon;
		if (w != NULL && w->Ammo1 != NULL)
		{
			mysnprintf(num, countof(num), "%d", w->Ammo1->Amount);
			DrawDoomText("STTNUM", num, vx, vy, vw, vh * 0.75f, 0.6f);
			mysnprintf(num, countof(num), "OF %d", w->Ammo1->MaxAmount);
			DrawDoomText(labelfont, num, vx, vy + vh * 0.8f, vw, vh * 0.2f, 0.6f);
		}
		else
			DrawDoomText("STTNUM", "-", vx, vy, vw, vh, 0.6f);
	}
	else if (Is(t.element, "st_weapon"))
	{
		AWeapon *w = p->ReadyWeapon;
		if (w == NULL)
			return;
		// the weapon's pickup sprite (first frame of its spawn state), or its inventory icon
		FTexture *pic = NULL;
		FState *st = w->SpawnState;
		if (st != NULL && (unsigned)st->sprite < sprites.Size() && sprites[st->sprite].numframes > st->GetFrame())
		{
			FTextureID id = SpriteFrames[sprites[st->sprite].spriteframes + st->GetFrame()].Texture[0];
			if (id.isValid())
				pic = TexMan[id];
		}
		if (pic == NULL && w->Icon.isValid())
			pic = TexMan[w->Icon];
		DrawFit(GpuTex(pic), vx, vy, vw, vh - 12, 0.6f);
		DrawDoomText(labelfont, w->GetTag(), vx, t.y + t.h - 14, vw, 8, 0.6f);
	}
	else if (Is(t.element, "st_keys"))
	{
		float kx = vx, ky = vy, ks = MIN(vw / 3.f, 22.f);
		for (AInventory *item = p->mo->Inventory; item != NULL; item = item->Inventory)
		{
			if (!item->IsKindOf(RUNTIME_CLASS(AKey)) || !item->Icon.isValid())
				continue;
			DrawFit(GpuTex(TexMan[item->Icon]), kx, ky, ks - 2, ks - 2, 0.6f);
			kx += ks;
			if (kx + ks > vx + vw + 1)
				kx = vx, ky += ks;
			if (ky + ks > vy + vh + 1)
				break;
		}
	}
}

static void DrawAimPad(const bd3ds_touch_t &t)
{
	C2D_DrawRectSolid(t.x, t.y, 0.3f, t.w, t.h, C2D_Color32(22, 16, 14, 255));
	u32 grid = C2D_Color32(60, 30, 24, 255);
	for (int gx = t.x + 20; gx < t.x + t.w; gx += 20)
		C2D_DrawRectSolid(gx, t.y, 0.32f, 1, t.h, grid);
	for (int gy = t.y + 20; gy < t.y + t.h; gy += 20)
		C2D_DrawRectSolid(t.x, gy, 0.32f, t.w, 1, grid);
	float cx = t.x + t.w / 2.f, cy = t.y + t.h / 2.f;
	C2D_DrawRectSolid(cx - 12, cy, 0.34f, 24, 1, C2D_Color32(200, 60, 40, 255));
	C2D_DrawRectSolid(cx, cy - 12, 0.34f, 1, 24, C2D_Color32(200, 60, 40, 255));
	if (t.label != NULL)
		DrawDoomText(t.font_owned ? t.font_owned : "STCFN", t.label, t.x, t.y + t.h - 18, t.w, 9, 0.35f);
}

static void DrawArt(const bd3ds_touch_t &t)
{
	DoomTex *art = GpuTex(TexByName(gameinfo.TitlePage));
	if (art != NULL)	// Doom pictures are 4:3 (320x200 shown at 320x240): stretch to the element
		C2D_DrawImageAt(art->Img, t.x, t.y, 0.3f, NULL, t.w / (float)art->W, t.h / (float)art->H);
	C2D_DrawRectSolid(t.x, t.y, 0.35f, t.w, t.h, C2D_Color32(0, 0, 0, 38));	// 15% darker
}

void I3DS_BottomOverlay()
{
	int mode = Mode();
	if (!bd3ds_bottom_modes[mode].backlight)
		return;	// screen off: nothing to draw
	C2D_TextBufClear(TextBuf);
	bool level = gamestate == GS_LEVEL;
	int current = level ? CurrentSlot() : -1;
	for (int i = 0; i < BD3DS_BOTTOM_COUNT; ++i)
	{
		const bd3ds_touch_t &t = bd3ds_bottom[i];
		if (!InMode(t, mode))
			continue;
		if (Is(t.kind, "art"))
			DrawArt(t);
		else if (!level)
			continue;	// the rest only makes sense in a level
		else if (Is(t.kind, "button"))
			DrawButton(i, current);
		else if (Is(t.kind, "status"))
			DrawStatus(t);
		else if (Is(t.kind, "aim") && Is(t.command, "aim") && t.h < 240)
			DrawAimPad(t);
	}
}

static void Release()
{
	if (Pressed >= 0)
	{
		const char *cmd = bd3ds_bottom[Pressed].command;
		if (cmd != NULL && cmd[0] == '+')
		{
			FString up = cmd;
			up.Truncate(0);
			up << "-" << (cmd + 1);
			AddCommandString(up.LockBuffer());
			up.UnlockBuffer();
		}
	}
	Pressed = -1;
	TouchAiming = false;
}

bool I3DS_BottomTouch(int x, int y, bool down)
{
	if (!down)
	{
		Release();
		return false;
	}
	if (Pressed >= 0)
		return true;
	if (TouchAiming)
	{
		I3DS_AimTouch(x - LastX, y - LastY);
		LastX = x; LastY = y;
		return true;
	}
	if (gamestate != GS_LEVEL || menuactive != MENU_Off)
		return false;
	int mode = Mode(), aim = -1;
	for (int i = 0; i < BD3DS_BOTTOM_COUNT; ++i)
	{
		const bd3ds_touch_t &t = bd3ds_bottom[i];
		if (!InMode(t, mode) || x < t.x || x >= t.x + t.w || y < t.y || y >= t.y + t.h)
			continue;
		if (Is(t.kind, "button") && t.command != NULL)
		{
			Pressed = i;
			AddCommandString((char *)t.command);
			return true;
		}
		if (Is(t.kind, "aim") && (Is(t.command, "aim") || (Is(t.command, "aim_if_enabled") && I3DS_TouchAimEnabled())))
			aim = i;
	}
	if (aim >= 0)
	{
		TouchAiming = true;
		LastX = x; LastY = y;
		return true;
	}
	return false;
}
