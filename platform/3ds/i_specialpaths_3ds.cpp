// 3DS user paths (replaces m_specialpaths.cpp). Subsystems sheet: filesystem; paths from the Filesystem sheet.
#include <sys/stat.h>
#include <sys/types.h>

#include "cmdlib.h"
#include "m_misc.h"
#include "version.h"
#include "doomdef.h"
#include "bd3ds_sheets.h"

// sdmc:/3ds/DoomLauncher/ : directory part of the config path
static FString UserDir ()
{
	FString path = BD3DS_PATH_CONFIG;
	long slash = path.LastIndexOf('/');
	return path.Left(slash + 1);
}

FString GetUserFile (const char *file)
{
	FString path = UserDir();
	mkdir (path.Left(path.Len() - 1), 0777);
	path << file;
	return path;
}

FString M_GetCachePath(bool create)
{
	FString path = UserDir() + "cache";
	if (create)
		mkdir (path, 0777);
	return path;
}

FString M_GetAutoexecPath()
{
	return GetUserFile("autoexec.cfg");
}

FString M_GetCajunPath(const char *botfilename)
{
	FString path = GetUserFile(botfilename);
	if (!FileExists(path))
	{
		path = "";
	}
	return path;
}

FString M_GetConfigPath(bool for_reading)
{
	return BD3DS_PATH_CONFIG;
}

FString M_GetScreenshotsPath()
{
	return UserDir() + "screenshots/";
}

FString M_GetSavegamesPath()
{
	return BD3DS_PATH_SAVES;
}
