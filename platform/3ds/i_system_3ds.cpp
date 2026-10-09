// 3DS system services (replaces posix/sdl/i_system.cpp). Subsystems sheet: system.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "doomerrors.h"
#include "doomtype.h"
#include "doomstat.h"
#include "version.h"
#include "c_cvars.h"
#include "c_console.h"
#include "d_main.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "gameconfigfile.h"
#include "x86.h"
#include "doomdef.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"

EXTERN_CVAR (String, language)

extern "C"
{
	double		SecondsPerCycle = 1e-8;
	double		CyclesPerSecond = 1e8;
}

DWORD LanguageIDs[4];

int (*I_GetTime) (bool saveMS);
int (*I_WaitForTic) (int);
void (*I_FreezeTime) (bool frozen);

void I_Tactile (int /*on*/, int /*off*/, int /*total*/)
{
}

ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void)
{
	return &emptycmd;
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

void I_WaitVBL (int count)
{
	// Used for delays only (not real vblank sync).
	svcSleepThread ((s64)1000000000 * count / 70);
}

void SetLanguageIDs ()
{
	size_t langlen = strlen(language);

	DWORD lang = (langlen < 2 || langlen > 3) ?
		MAKE_ID('e','n','u','\0') :
		MAKE_ID(language[0],language[1],language[2],'\0');

	LanguageIDs[3] = LanguageIDs[2] = LanguageIDs[1] = LanguageIDs[0] = lang;
}

void I_InitTimer ();
void I_ShutdownTimer ();

void I_Init (void)
{
	CheckCPUID (&CPU);
	DumpCPUInfo (&CPU);

	atterm (I_ShutdownSound);
	I_InitSound ();
	I_InitTimer ();
}

static int has_exited;

void I_Quit (void)
{
	has_exited = 1;
	// Subsystems sheet: main - quitting the game (or an error screen) goes back to the launcher. Only for a
	// CIA (the Homebrew Launcher has its own menu) and not when the HOME menu is closing the app.
	bool relaunch = I3DS_AppRunning() && !envIsHomebrew();
	if (relaunch)
		aptSetChainloaderToSelf();
	I3DS_StartExitWatchdog(relaunch);
	I3DS_BottomBacklightRestore();

	if (demorecording)
		G_CheckDemoStatus();

	C_DeinitConsole();

	I_ShutdownTimer();
}

extern FILE *Logfile;
bool gameisdead;

void STACK_ARGS I_FatalError (const char *error, ...)
{
	static bool alreadyThrown = false;
	gameisdead = true;

	if (!alreadyThrown)
	{
		alreadyThrown = true;
		char errortext[MAX_ERRORTEXT];
		va_list argptr;
		va_start (argptr, error);
		vsnprintf (errortext, MAX_ERRORTEXT, error, argptr);
		va_end (argptr);

		if (Logfile)
		{
			fprintf (Logfile, "\n**** DIED WITH FATAL ERROR:\n%s\n", errortext);
			fflush (Logfile);
		}
		I3DS_FatalScreen (errortext);
		exit (-1);
	}

	if (!has_exited)
	{
		has_exited = 1;
		exit(-1);
	}
}

void STACK_ARGS I_Error (const char *error, ...)
{
	va_list argptr;
	char errortext[MAX_ERRORTEXT];

	va_start (argptr, error);
	vsnprintf (errortext, MAX_ERRORTEXT, error, argptr);
	va_end (argptr);

	throw CRecoverableError (errortext);
}

void I_SetIWADInfo ()
{
}

// Console text goes to the bottom-screen console only until the GPU takes over.
void I_PrintStr (const char *cp)
{
	if (!I3DS_ConsoleActive())
		return;
	char *copy = new char[strlen(cp)+1];
	const char *srcp = cp;
	char *dstp = copy;
	while (*srcp != 0)
	{
		if (*srcp!=0x1c && *srcp!=0x1d && *srcp!=0x1e && *srcp!=0x1f)
		{
			*dstp++=*srcp++;
		}
		else
		{
			if (srcp[1]!=0) srcp+=2;
			else break;
		}
	}
	*dstp=0;
	fputs (copy, stdout);
	delete [] copy;
}

// IWAD picker on the bottom-screen console (Filesystem sheet: iwad_picker). D-pad + A.
int I_PickIWad (WadStuff *wads, int numwads, bool showwin, int defaultiwad)
{
	if (!showwin || numwads <= 1)
	{
		return defaultiwad;
	}
	int sel = (defaultiwad >= 0 && defaultiwad < numwads) ? defaultiwad : 0;
	for (;;)
	{
		printf ("\x1b[2J\x1b[1;1H" BD3DS_TITLE "\n\nSelect a game (D-Pad, A to start):\n\n");
		for (int i = 0; i < numwads; ++i)
		{
			printf ("%s %s\n", i == sel ? " >" : "  ", wads[i].Name.GetChars());
		}
		u32 down = 0;
		while (aptMainLoop() && down == 0)
		{
			I3DS_WaitFrame();	// no unbounded VBlank wait (i_3ds.h)
			hidScanInput();
			down = hidKeysDown();
		}
		if (!aptMainLoop())
			return -1;
		if (down & KEY_DUP) sel = (sel + numwads - 1) % numwads;
		if (down & KEY_DDOWN) sel = (sel + 1) % numwads;
		if (down & KEY_A) { printf ("\x1b[2J\x1b[1;1H"); return sel; }
		if (down & KEY_START) return -1;
	}
}

bool I_WriteIniFailed ()
{
	printf ("The config file %s could not be saved:\n%s\n", GameConfig->GetPathName(), strerror(errno));
	return false;
}

// ---- file finding (no scandir/fnmatch in newlib) ---------------------------

static bool WildMatch (const char *pat, const char *str)
{
	while (*pat)
	{
		if (*pat == '*')
		{
			while (*pat == '*') pat++;
			if (!*pat) return true;
			for (; *str; str++)
				if (WildMatch(pat, str)) return true;
			return false;
		}
		if (!*str) return false;
		if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*str)) return false;
		pat++; str++;
	}
	return *str == 0;
}

static FString FindDir;

void *I_FindFirst (const char *filespec, findstate_t *fileinfo)
{
	const char *pattern;
	const char *slash = strrchr (filespec, '/');
	if (slash)
	{
		pattern = slash+1;
		FindDir = FString(filespec, slash-filespec+1);
	}
	else
	{
		pattern = filespec;
		FindDir = "./";
	}
	fileinfo->current = 0;
	fileinfo->count = 0;
	fileinfo->namelist = NULL;

	DIR *d = opendir (FindDir.GetChars());
	if (d == NULL)
		return (void*)-1;
	int cap = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL)
	{
		if (!WildMatch(pattern, ent->d_name))
			continue;
		if (fileinfo->count == cap)
		{
			cap = cap ? cap * 2 : 16;
			fileinfo->namelist = (struct dirent **)realloc(fileinfo->namelist, cap * sizeof(struct dirent *));
		}
		struct dirent *copy = (struct dirent *)malloc(sizeof(struct dirent));
		memcpy (copy, ent, sizeof(struct dirent));
		fileinfo->namelist[fileinfo->count++] = copy;
	}
	closedir (d);
	if (fileinfo->count > 0)
		return fileinfo;
	free (fileinfo->namelist);
	fileinfo->namelist = NULL;
	return (void*)-1;
}

int I_FindNext (void *handle, findstate_t *fileinfo)
{
	findstate_t *state = (findstate_t *)handle;
	if (state->current < fileinfo->count)
	{
		return ++state->current < fileinfo->count ? 0 : -1;
	}
	return -1;
}

int I_FindClose (void *handle)
{
	findstate_t *state = (findstate_t *)handle;
	if (handle != (void*)-1 && state->count > 0)
	{
		for (int i = 0; i < state->count; ++i)
			free (state->namelist[i]);
		state->count = 0;
		free (state->namelist);
		state->namelist = NULL;
	}
	return 0;
}

int I_FindAttr (findstate_t *fileinfo)
{
	dirent *ent = fileinfo->namelist[fileinfo->current];
	struct stat buf;
	FString path = FindDir + ent->d_name;
	if (stat(path.GetChars(), &buf) == 0)
	{
		return S_ISDIR(buf.st_mode) ? FA_DIREC : 0;
	}
	return 0;
}

unsigned int I_MakeRNGSeed()
{
	return (unsigned int)(osGetTime() ^ svcGetSystemTick());
}

TArray<FString> I_GetGogPaths()
{
	return TArray<FString>();
}
