// 3DS graphics init + video modes (replaces posix/sdl/hardware.cpp). Subsystems sheet: hardware.
// Modes come from the Video sheet (bd3ds_video_modes); the Menu sheet's vid_3dsmode cvar picks one.
#include <3ds.h>
#include <string.h>

#include "doomtype.h"
#include "doomdef.h"
#include "hardware.h"
#include "i_video.h"
#include "i_system.h"
#include "c_console.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_text.h"
#include "doomstat.h"
#include "m_argv.h"
#include "r_renderer.h"
#include "r_swrenderer.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"

IVideo *I3DS_CreateVideo();

EXTERN_CVAR (Bool, ticker)
EXTERN_CVAR (Int, vid_defwidth)
EXTERN_CVAR (Int, vid_defheight)

IVideo *Video;

// ---- Video sheet modes + hardware gate ---------------------------------------

static int SystemModel = -1;

static int Model()
{
	if (SystemModel < 0)
	{
		u8 m = CFG_MODEL_N3DS;
		if (R_SUCCEEDED(cfguInit()))
		{
			CFGU_GetSystemModel(&m);
			cfguExit();
		}
		SystemModel = m;
	}
	return SystemModel;
}

int I3DS_ModeCount()
{
	return BD3DS_VIDEO_MODE_COUNT;
}

bool I3DS_ModeAllowed(int mode)
{
	if (mode < 0 || mode >= BD3DS_VIDEO_MODE_COUNT)
		return false;
	const char *gate = bd3ds_video_modes[mode].gate;
	if (gate != NULL && strstr(gate, "CFG_MODEL_2DS") != NULL && Model() == CFG_MODEL_2DS)
		return false;	// the original 2DS cannot drive the 800px wide mode
	return true;
}

void I3DS_ModeSize(int mode, int *width, int *height)
{
	*width = bd3ds_video_modes[mode].width;
	*height = bd3ds_video_modes[mode].height;
}

int I3DS_ModeForSize(int width, int height)
{
	for (int i = 0; i < BD3DS_VIDEO_MODE_COUNT; ++i)
	{
		if (bd3ds_video_modes[i].width == width && bd3ds_video_modes[i].height == height)
			return i;
	}
	return -1;
}

extern int NewWidth, NewHeight, NewBits, DisplayBits;

// Menu sheet: "Screen resolution" -> vid_3dsmode (index into the Video sheet)
CUSTOM_CVAR (Int, vid_3dsmode, BD3DS_VIDEO_MODE_DEFAULT, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL)
{
	if (!I3DS_ModeAllowed(self))
	{
		self = BD3DS_VIDEO_MODE_DEFAULT;
		return;
	}
	if (screen != NULL)
	{
		I3DS_ModeSize(self, &NewWidth, &NewHeight);
		NewBits = 8;
		setmodeneeded = true;
	}
}

// ---- engine entry points -------------------------------------------------------

void I_ShutdownGraphics ()
{
	if (screen)
	{
		DFrameBuffer *s = screen;
		screen = NULL;
		s->ObjectFlags |= OF_YesReallyDelete;
		delete s;
	}
	if (Video)
		delete Video, Video = NULL;
	I3DS_BottomShutdown();
	I3DS_GpuShutdown();
}

void I_InitGraphics ()
{
	UCVarValue val;
	val.Bool = !!Args->CheckParm ("-devparm");
	ticker.SetGenericRepDefault (val, CVAR_Bool);

	int mode = I3DS_ModeAllowed(vid_3dsmode) ? (int)vid_3dsmode : BD3DS_VIDEO_MODE_DEFAULT;
	int w, h;
	I3DS_ModeSize(mode, &w, &h);
	vid_defwidth = w;	// V_Init2 starts from these
	vid_defheight = h;

	I3DS_GpuInit();
	I3DS_BottomInit();
	Video = I3DS_CreateVideo();
	if (Video == NULL)
		I_FatalError ("Failed to initialize display");
	atterm (I_ShutdownGraphics);
}

static void I_DeleteRenderer()
{
	if (Renderer != NULL) delete Renderer;
}

void I_CreateRenderer()
{
	if (Renderer == NULL)
	{
		Renderer = new FSoftwareRenderer;
		atterm(I_DeleteRenderer);
	}
}

DFrameBuffer *I_SetMode (int &width, int &height, DFrameBuffer *old)
{
	return Video->CreateFrameBuffer (width, height, true, old);
}

bool I_CheckResolution (int width, int height, int bits)
{
	int twidth, theight;
	Video->StartModeIterator (bits, true);
	while (Video->NextMode (&twidth, &theight, NULL))
	{
		if (width == twidth && height == theight)
			return true;
	}
	return false;
}

// Only the Video sheet modes exist: any other request (e.g. the engine's 640x480 vid_defwidth default)
// resolves to the mode chosen in the menu (vid_3dsmode), not to the nearest size.
void I_ClosestResolution (int *width, int *height, int bits)
{
	int mode = I3DS_ModeForSize(*width, *height);
	if (mode >= 0 && I3DS_ModeAllowed(mode))
		return;
	mode = I3DS_ModeAllowed(vid_3dsmode) ? (int)vid_3dsmode : BD3DS_VIDEO_MODE_DEFAULT;
	I3DS_ModeSize(mode, width, height);
}

// The GPU frame pacing (I3DS_FrameBeginSafe, 60 Hz) already caps presentation at the display rate.
Semaphore FPSLimitSemaphore;

void I_SetFPSLimit(int limit)
{
}

CUSTOM_CVAR (Int, vid_maxfps, 60, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (vid_maxfps < TICRATE && vid_maxfps != 0)
		vid_maxfps = TICRATE;
	else if (vid_maxfps > 60)
		vid_maxfps = 60;
}

CUSTOM_CVAR (Bool, fullscreen, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
}

CUSTOM_CVAR (Float, vid_winscale, 1.f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
}

CCMD (vid_listmodes)
{
	for (int i = 0; i < BD3DS_VIDEO_MODE_COUNT; ++i)
	{
		bool thisMode = (bd3ds_video_modes[i].width == DisplayWidth && bd3ds_video_modes[i].height == DisplayHeight);
		Printf (thisMode ? PRINT_BOLD : PRINT_HIGH, "%d: %s%s\n", i, bd3ds_video_modes[i].label,
			I3DS_ModeAllowed(i) ? "" : " (not supported on this system)");
	}
}

CCMD (vid_currentmode)
{
	Printf ("%dx%dx%d\n", DisplayWidth, DisplayHeight, DisplayBits);
}
