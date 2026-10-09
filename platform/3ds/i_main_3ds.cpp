// 3DS entry point (replaces posix/sdl/i_main.cpp). Subsystems sheet: main.
#include <3ds.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "doomerrors.h"
#include "m_argv.h"
#include "d_main.h"
#include "i_system.h"
#include "c_console.h"
#include "version.h"
#include "cmdlib.h"
#include "doomdef.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"
#include "c_cvars.h"
#include "launcher_3ds.h"

EXTERN_CVAR (Int, snd_mididevice)
EXTERN_CVAR (Bool, use_joystick)

// Build sheet: stack_kb. libctru's default main stack (32 KB) overflowed during Brutal Doom gib chains.
extern "C" { u32 __stacksize__ = BD3DS_STACK_KB * 1024; }

#define MAX_TERMS 64

DArgs *Args;

static void (*TermFuncs[MAX_TERMS]) ();
static const char *TermNames[MAX_TERMS];
static int NumTerms;

void addterm (void (*func) (), const char *name)
{
	for (int i = 0; i < NumTerms; ++i)
	{
		if (TermFuncs[i] == func)
			return;
	}
	if (NumTerms == MAX_TERMS)
	{
		func ();
		I_FatalError ("Too many exit functions registered.\nIncrease MAX_TERMS in i_main_3ds.cpp");
	}
	TermNames[NumTerms] = name;
	TermFuncs[NumTerms++] = func;
}

void popterm ()
{
	if (NumTerms)
		NumTerms--;
}

void STACK_ARGS call_terms ()
{
	while (NumTerms > 0)
	{
		TermFuncs[--NumTerms] ();
	}
}

static void STACK_ARGS NewFailure ()
{
	I_FatalError ("Failed to allocate memory from system heap");
}

void I_StartupJoysticks();
void I_ShutdownJoysticks();

// Config/save directories from the Filesystem sheet (sdmc:/3ds/DoomLauncher/...)
static void MakeUserDirs()
{
	mkdir("sdmc:/3ds", 0777);
	FString base = BD3DS_PATH_SAVES;	// ".../save/"
	FString parent = base.Left(base.Len() - 1);
	long slash = parent.LastIndexOf('/');
	if (slash > 0)
		mkdir(parent.Left(slash), 0777);
	mkdir(parent, 0777);
}

int main (int argc, char **argv)
{
	I3DS_HomeTraceInit();	// HOME / sleep transitions -> home_trace.txt (i_input_3ds.cpp)
	osSetSpeedupEnable(true);	// Build sheet: 804MHz + L2 on New 3DS
	romfsInit();
	std::vector<std::string> launch;
	if (!I3DS_RunLauncher(launch))
	{
		romfsExit();
		return 0;	// closed from the HOME menu
	}
	// No sleep while the engine loads: libctru finishes a sleep only inside aptMainLoop, which loading does not call,
	// so a closed lid left a black screen and dead audio. The first input poll of the game loop allows it again
	// (and sleeps then if the lid is still closed).
	aptSetSleepAllowed(false);
	I3DS_HomeTrace("launcher done: loading the game (ZDoom engine)");
	I3DS_ConsoleInit();
	MakeUserDirs();

	printf(GAMENAME " %s - " BD3DS_TITLE "\n", GetVersionString());

	std::set_new_handler (NewFailure);
	setlocale (LC_ALL, "C");

	try
	{
		// Launcher sheet: the launcher builds the command line (-iwad, -file mod [cache] ..., -deh)
		static char a0[] = "doomlauncher3ds";
		char *fakeargv[] = { a0, NULL };
		Args = new DArgs(1, fakeargv);
		for (size_t i = 0; i < launch.size(); ++i)
			Args->AppendArg(launch[i].c_str());
		// Filesystem sheet: args_file - optional extra command line (debugging, e.g. +logfile, +map)
		FILE *af = fopen(BD3DS_PATH_ARGS_FILE, "r");
		if (af != NULL)
		{
			char word[256];
			while (fscanf(af, "%255s", word) == 1)
				Args->AppendArg(word);
			fclose(af);
		}

		// Audio sheet: music_midi = OPL emulation. Engine default (-1, "sound system") needs FMOD.
		{
			UCVarValue v;
			v.Int = -3;	// MDEV_OPL
			snd_mididevice.SetGenericRepDefault(v, CVAR_Int);
		}
		// Controls sheet: CIRCLEPAD/CSTICK drive the engine through its joystick path, which is off by default.
		{
			UCVarValue v;
			v.Bool = true;
			use_joystick.SetGenericRepDefault(v, CVAR_Bool);
		}

		atexit (call_terms);
		atterm (I_Quit);

		progdir = "romfs:/";

		I_StartupJoysticks();
		C_InitConsole (80*8, 25*8, false);
		D_DoomMain ();
	}
	catch (class CDoomError &error)
	{
		I_ShutdownJoysticks();
		I3DS_FatalScreen(error.GetMessage() ? error.GetMessage() : "Unknown error");
		exit (-1);
	}
	catch (...)
	{
		call_terms ();
		throw;
	}
	return 0;
}
