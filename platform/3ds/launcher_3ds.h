// Launcher + mod cache interfaces for the platform module (C++11, before/around the engine).
#pragma once
#include <string>
#include <vector>

// launcher_3ds.cpp: runs before the engine. Fills args with the engine command line for the chosen IWAD,
// mods (each followed by its cache pk3 when one exists) and profile extras. False = the app is closing.
bool I3DS_RunLauncher(std::vector<std::string> &args);

// profile_3ds.cpp: raw framebuffer dump (u16 w, h + BGR8; tools/fb2png.py converts it)
#include <3ds.h>
void I3DS_DumpScreen(gfxScreen_t screen, const char *path);

// modcache_3ds.cpp: cache naming shared by the launcher and the engine-time builder.
unsigned I3DS_PaletteKey(const std::vector<std::string> &files);	// crc of the last PLAYPAL in load order
unsigned I3DS_ModKey(const char *path, unsigned palkey);
std::string I3DS_ModCacheFile(const char *modpath, unsigned key, const char *ext);	// cache/<mod>-<key>.<ext>
int I3DS_ModScriptKind(const char *path);	// 0 DECORATE/ACS, 1 ZScript + DECORATE, 2 ZScript only (needs GZDoom 3+)
