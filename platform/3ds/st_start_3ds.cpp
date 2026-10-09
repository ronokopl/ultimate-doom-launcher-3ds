// 3DS startup screen + console (replaces posix/sdl/st_start.cpp). Subsystems sheet: startup.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "st_start.h"
#include "doomdef.h"
#include "i_system.h"
#include "c_cvars.h"
#include "i_3ds.h"

// Bottom-screen text console until the GPU takes over (I3DS_GpuInit) -----------

static bool ConsoleUp;
static PrintConsole BottomConsole;

void I3DS_ConsoleInit()
{
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, &BottomConsole);
	ConsoleUp = true;
}

bool I3DS_ConsoleActive()
{
	return ConsoleUp && !I3DS_GpuActive();
}

extern FILE *Logfile;

void I3DS_FatalScreen(const char *text)
{
	if (Logfile)
	{
		fprintf (Logfile, "\n**** FATAL (3DS screen):\n%s\n", text);
		fflush (Logfile);
	}
	I3DS_BottomBacklightRestore();
	if (I3DS_GpuActive())
	{
		I3DS_GpuShutdown();	// hand the screens back to the console
		gfxInitDefault();
	}
	consoleInit(GFX_BOTTOM, &BottomConsole);
	ConsoleUp = true;
	printf ("\x1b[2J\x1b[1;1H\x1b[31mFATAL ERROR\x1b[0m\n\n%s\n\nPress START to exit.\n", text);
	while (aptMainLoop())
	{
		I3DS_WaitFrame();	// no unbounded VBlank wait (i_3ds.h)
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
	}
}

FStartupScreen *StartScreen;

// Startup screen --------------------------------------------------------------

class F3DSStartupScreen : public FStartupScreen
{
public:
	F3DSStartupScreen(int max_progress) : FStartupScreen(max_progress) {}
	void Progress()
	{
		if (CurPos < MaxPos)
			++CurPos;
		if (!I3DS_ConsoleActive() || MaxPos <= 0)
			return;
		int notch = CurPos * 30 / MaxPos;
		if (notch != NotchPos)
		{
			NotchPos = notch;
			printf ("\x1b[29;1H[%-30.*s]", notch, "##############################");
			fflush (stdout);
		}
	}
	void NetInit(const char *message, int num_players) {}
	void NetProgress(int count) {}
	void NetMessage(const char *format, ...) {}
	void NetDone() {}
	bool NetLoop(bool (*timer_callback)(void *), void *userdata) { return false; }
};

CUSTOM_CVAR(Int, showendoom, 0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	else if (self > 2) self = 2;
}

void DeleteStartupScreen()
{
	if (StartScreen != NULL)
	{
		delete StartScreen;
		StartScreen = NULL;
	}
}

FStartupScreen *FStartupScreen::CreateInstance(int max_progress)
{
	atterm(DeleteStartupScreen);
	return new F3DSStartupScreen(max_progress);
}

void I_ShutdownJoysticks();

void ST_Endoom()
{
	I_ShutdownJoysticks();
	exit(0);
}
