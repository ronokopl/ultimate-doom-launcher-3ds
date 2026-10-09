// Mod launcher (Subsystems sheet: launcher; layout, labels and buttons: Launcher sheet).
// Runs before the engine on its own citro2d session: pick an IWAD from iwads/, tick mods from mods/ and set
// their load order, save/load named profiles (profiles.ini, [last] = previous launch), then Launch builds the
// engine command line. Mods with a cache pk3 (ModCache sheet) get it loaded right after them.
// Launcher btn_engine: ZScript mods can go to the experimental GZDoom or UZDoom engine CIAs instead (Build
// gz_unique_id / uz_unique_id); the launcher writes their command line to gz_launch / uz_launch and chainloads them.
#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>
#include <string>
#include <vector>

#define BD3DS_NO_ENGINE_KEYS
#include "bd3ds_sheets.h"
#include "launcher_3ds.h"
#include "i_3ds.h"

namespace {

enum Engine { ENGINE_AUTO, ENGINE_ZDOOM, ENGINE_GZDOOM, ENGINE_UZDOOM, ENGINE_COUNT };
const char *const EngineIni[ENGINE_COUNT] = { "auto", "zdoom", "gzdoom", "uzdoom" };	// profiles.ini engine=

struct Profile
{
	std::string name, iwad;
	std::vector<std::string> mods;
	int engine;
};

enum Tab { TAB_IWAD, TAB_MODS, TAB_PROFILES, TAB_OPTIONS, TAB_COUNT };
enum Option { OPT_MODCACHE, OPT_CLEARCACHE, OPT_COUNT };	// Options tab rows (settings.ini)
const int ROW_H = 22;

const u32 COL_BG = C2D_Color32(24, 12, 10, 255), COL_PANEL = C2D_Color32(52, 26, 20, 255),
	COL_HILITE = C2D_Color32(120, 30, 20, 255), COL_TEXT = C2D_Color32(230, 220, 200, 255),
	COL_DIM = C2D_Color32(150, 135, 120, 255), COL_GOLD = C2D_Color32(255, 200, 40, 255),
	COL_BUTTON = C2D_Color32(80, 70, 64, 255), COL_ERR = C2D_Color32(255, 90, 70, 255);

struct Launcher
{
	std::vector<std::string> iwads, mods;	// file names in iwads/ and mods/
	std::string iwad;						// selected IWAD (file name)
	std::vector<std::string> order;			// enabled mods in load order
	std::vector<Profile> profiles;			// named profiles ([last] excluded)
	std::vector<int> modKind;				// I3DS_ModScriptKind per entry of mods
	bool zscriptWarned = false;				// START was pressed once with a ZScript-only mod ticked
	int engine = ENGINE_AUTO;				// Launcher btn_engine
	bool modCache = true;					// Options: mod caching (settings.ini modcache=)
	unsigned cacheKB = 0;					// Options: size of cache/ (refreshed when the tab opens)
	int tab = TAB_IWAD, cursor[TAB_COUNT] = {}, scroll[TAB_COUNT] = {};
	std::string message;
	C3D_RenderTarget *top = NULL, *bottom = NULL;
	C2D_TextBuf buf = NULL;
	bool touching = false, dragged = false;
	int touchStartY = 0, touchScroll = 0;

	// ---- files -------------------------------------------------------------------------------------

	static bool HasExt(const std::string &name, const char *const *exts)
	{
		size_t dot = name.rfind('.');
		if (dot == std::string::npos)
			return false;
		for (; *exts; ++exts)
			if (!strcasecmp(name.c_str() + dot + 1, *exts))
				return true;
		return false;
	}

	static std::vector<std::string> Scan(const char *dir, const char *const *exts)
	{
		std::vector<std::string> out;
		if (DIR *d = opendir(dir))
		{
			while (dirent *e = readdir(d))
				if (e->d_type != DT_DIR && HasExt(e->d_name, exts))
					out.push_back(e->d_name);
			closedir(d);
		}
		std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
		return out;
	}

	static bool IsDeh(const std::string &name)
	{
		static const char *const deh[] = { "deh", "bex", NULL };
		return HasExt(name, deh);
	}

	void ScanFiles()
	{
		static const char *const iwadExt[] = { "wad", "iwad", "ipk3", "pk3", NULL };
		static const char *const modExt[] = { "wad", "pk3", "zip", "pk7", "deh", "bex", NULL };
		mkdir("sdmc:/3ds", 0777);
		std::string base = BD3DS_PATH_IWAD_DIR;
		base = base.substr(0, base.rfind('/', base.size() - 2));
		mkdir(base.c_str(), 0777);
		mkdir(BD3DS_PATH_IWAD_DIR, 0777);
		mkdir(BD3DS_PATH_MOD_DIR, 0777);
		iwads = Scan(BD3DS_PATH_IWAD_DIR, iwadExt);
		mods = Scan(BD3DS_PATH_MOD_DIR, modExt);
		modKind.clear();
		for (size_t i = 0; i < mods.size(); ++i)
			modKind.push_back(IsDeh(mods[i]) ? 0 : I3DS_ModScriptKind((BD3DS_PATH_MOD_DIR + mods[i]).c_str()));
	}

	int Kind(const std::string &mod) const
	{
		std::vector<std::string>::const_iterator it = std::find(mods.begin(), mods.end(), mod);
		return it == mods.end() ? 0 : modKind[it - mods.begin()];
	}

	bool Have(const std::vector<std::string> &v, const std::string &s) const
	{
		return std::find(v.begin(), v.end(), s) != v.end();
	}

	// ---- settings.ini (Options tab, global) --------------------------------------------------------

	void LoadSettings()
	{
		if (FILE *f = fopen(BD3DS_PATH_SETTINGS, "r"))
		{
			char line[128];
			while (fgets(line, sizeof(line), f))
				if (!strncmp(line, "modcache=", 9))
					modCache = line[9] != '0';
			fclose(f);
		}
	}

	void SaveSettings()
	{
		if (FILE *f = fopen(BD3DS_PATH_SETTINGS, "w"))
		{
			fprintf(f, "modcache=%d\n", modCache ? 1 : 0);
			fclose(f);
		}
		else
			message = "Cannot write " BD3DS_PATH_SETTINGS;
	}

	void MeasureCache()
	{
		cacheKB = 0;
		if (DIR *d = opendir(BD3DS_PATH_CACHE_DIR))
		{
			while (dirent *e = readdir(d))
			{
				struct stat st;
				if (e->d_type != DT_DIR && stat((std::string(BD3DS_PATH_CACHE_DIR) + e->d_name).c_str(), &st) == 0)
					cacheKB += (unsigned)(st.st_size / 1024);
			}
			closedir(d);
		}
	}

	void ClearCache()
	{
		std::vector<std::string> files;
		if (DIR *d = opendir(BD3DS_PATH_CACHE_DIR))
		{
			while (dirent *e = readdir(d))
				if (e->d_type != DT_DIR)
					files.push_back(std::string(BD3DS_PATH_CACHE_DIR) + e->d_name);
			closedir(d);
		}
		for (size_t i = 0; i < files.size(); ++i)
			remove(files[i].c_str());
		message = "Deleted " + std::to_string(files.size()) + " cached file(s); mods convert again on their next launch";
		statusOrder.clear();	// cache status of the load order changed
		statusIwad.clear();
		MeasureCache();
	}

	std::vector<std::string> OptionRows() const
	{
		std::vector<std::string> rows(OPT_COUNT);
		rows[OPT_MODCACHE] = std::string("Mod caching: ") + (modCache ? "ON" : "OFF");
		char size[32];
		snprintf(size, sizeof(size), cacheKB >= 1024 ? "%.1f MB" : "%.0f KB", cacheKB >= 1024 ? cacheKB / 1024.0 : (double)cacheKB);
		rows[OPT_CLEARCACHE] = std::string("Delete cached files (") + size + ")";
		return rows;
	}

	// ---- profiles.ini ------------------------------------------------------------------------------

	void LoadProfiles()
	{
		profiles.clear();
		FILE *f = fopen(BD3DS_PATH_PROFILES, "r");
		if (f == NULL)
			return;
		char line[512];
		Profile *cur = NULL;
		std::vector<Profile> all;
		while (fgets(line, sizeof(line), f))
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
				s.pop_back();
			if (s.size() > 2 && s[0] == '[' && s.back() == ']')
			{
				all.push_back(Profile());
				all.back().engine = ENGINE_AUTO;
				cur = &all.back();
				cur->name = s.substr(1, s.size() - 2);
			}
			else if (cur && s.compare(0, 5, "iwad=") == 0)
				cur->iwad = s.substr(5);
			else if (cur && s.compare(0, 4, "mod=") == 0)
				cur->mods.push_back(s.substr(4));
			else if (cur && s.compare(0, 7, "engine=") == 0)
				for (int e = 0; e < ENGINE_COUNT; ++e)
					if (s.substr(7) == EngineIni[e])
						cur->engine = e;
		}
		fclose(f);
		for (size_t i = 0; i < all.size(); ++i)
		{
			if (all[i].name == "last")
				Apply(all[i], false);
			else
				profiles.push_back(all[i]);
		}
	}

	void SaveProfiles()
	{
		FILE *f = fopen(BD3DS_PATH_PROFILES, "w");
		if (f == NULL)
		{
			message = "Cannot write " BD3DS_PATH_PROFILES;
			return;
		}
		std::vector<Profile> all(1, Profile{ "last", iwad, order, engine });
		all.insert(all.end(), profiles.begin(), profiles.end());
		for (size_t i = 0; i < all.size(); ++i)
		{
			fprintf(f, "[%s]\niwad=%s\nengine=%s\n", all[i].name.c_str(), all[i].iwad.c_str(), EngineIni[all[i].engine]);
			for (size_t k = 0; k < all[i].mods.size(); ++k)
				fprintf(f, "mod=%s\n", all[i].mods[k].c_str());
			fprintf(f, "\n");
		}
		fclose(f);
	}

	void Apply(const Profile &p, bool report)
	{
		int missing = 0;
		if (Have(iwads, p.iwad))
			iwad = p.iwad;
		else if (!p.iwad.empty())
			++missing;
		order.clear();
		for (size_t i = 0; i < p.mods.size(); ++i)
		{
			if (Have(mods, p.mods[i]))
				order.push_back(p.mods[i]);
			else
				++missing;
		}
		engine = p.engine;
		if (report)
			message = missing ? "Loaded '" + p.name + "' (" + std::to_string(missing) + " file(s) missing)" : "Loaded profile '" + p.name + "'";
	}

	// ---- list model --------------------------------------------------------------------------------

	// Mods tab: enabled mods in load order first, then the rest alphabetically.
	std::vector<std::string> ModRows() const
	{
		std::vector<std::string> rows(order);
		for (size_t i = 0; i < mods.size(); ++i)
			if (!Have(order, mods[i]))
				rows.push_back(mods[i]);
		return rows;
	}

	int RowCount() const
	{
		return tab == TAB_IWAD ? (int)iwads.size() : tab == TAB_MODS ? (int)mods.size() :
			tab == TAB_OPTIONS ? (int)OPT_COUNT : (int)profiles.size();
	}

	const bd3ds_ui_t &Ui(const char *element) const
	{
		for (unsigned i = 0; i < BD3DS_LAUNCHER_COUNT; ++i)
			if (!strcmp(bd3ds_launcher[i].element, element))
				return bd3ds_launcher[i];
		return bd3ds_launcher[0];
	}

	int VisibleRows() const { return Ui("list").h / ROW_H; }

	void ClampCursor()
	{
		int n = RowCount(), &c = cursor[tab], &s = scroll[tab], vis = VisibleRows();
		c = n ? std::max(0, std::min(c, n - 1)) : 0;
		if (c < s) s = c;
		if (c >= s + vis) s = c - vis + 1;
		s = std::max(0, std::min(s, std::max(0, n - vis)));
	}

	// ---- actions -----------------------------------------------------------------------------------

	void Select()
	{
		int c = cursor[tab];
		if (tab == TAB_IWAD && c < (int)iwads.size())
		{
			iwad = iwads[c];
			message = "IWAD: " + iwad;
		}
		else if (tab == TAB_MODS)
		{
			std::vector<std::string> rows = ModRows();
			if (c < (int)rows.size())
				ToggleMod(rows[c]);
		}
		else if (tab == TAB_PROFILES && c < (int)profiles.size())
			Apply(profiles[c], true);
		else if (tab == TAB_OPTIONS && c == OPT_MODCACHE)
		{
			modCache = !modCache;
			SaveSettings();
			statusOrder.clear();	// the load order's cache status depends on it
			statusIwad.clear();
			message = modCache ? "Mod caching ON: first launch of a mod converts its sprites/sounds once" :
				"Mod caching OFF: no conversion; existing caches are not loaded";
		}
		else if (tab == TAB_OPTIONS && c == OPT_CLEARCACHE)
			ClearCache();
	}

	// Keeps the cursor on the same file after it moves between the enabled and disabled groups.
	void ToggleMod(const std::string &name)
	{
		std::vector<std::string>::iterator it = std::find(order.begin(), order.end(), name);
		if (it != order.end())
			order.erase(it);
		else
			order.push_back(name);
		zscriptWarned = false;
		if (it == order.end() && Kind(name) == 2)
			message = name + ": ZScript mod (GZDoom 3+) - not supported by ZDoom 2.8.1";
		std::vector<std::string> rows = ModRows();
		cursor[TAB_MODS] = std::find(rows.begin(), rows.end(), name) - rows.begin();
	}

	void Move(int dir)
	{
		if (tab != TAB_MODS)
			return;
		std::vector<std::string> rows = ModRows();
		int c = cursor[tab];
		if (c >= (int)order.size())
		{
			message = "Tick a mod first, then move it in the load order";
			return;
		}
		int to = c + dir;
		if (to < 0 || to >= (int)order.size())
			return;
		std::swap(order[c], order[to]);
		cursor[tab] = to;
	}

	void SaveProfile()
	{
		if (iwad.empty())
		{
			message = "Pick an IWAD before saving a profile";
			return;
		}
		SwkbdState kb;
		char name[33] = "";
		swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 32);
		swkbdSetHintText(&kb, "Profile name");
		if (swkbdInputText(&kb, name, sizeof(name)) != SWKBD_BUTTON_CONFIRM || !name[0] || !strcmp(name, "last"))
			return;
		for (char *p = name; *p; ++p)
			if (*p == '[' || *p == ']')
				*p = '_';
		Profile p = { name, iwad, order, engine };
		bool replaced = false;
		for (size_t i = 0; i < profiles.size(); ++i)
			if (profiles[i].name == p.name)
			{
				profiles[i] = p;
				replaced = true;
			}
		if (!replaced)
			profiles.push_back(p);
		SaveProfiles();
		message = (replaced ? "Updated profile '" : "Saved profile '") + p.name + "'";
	}

	void DeleteProfile()
	{
		if (tab != TAB_PROFILES || cursor[tab] >= (int)profiles.size())
			return;
		message = "Deleted profile '" + profiles[cursor[tab]].name + "'";
		profiles.erase(profiles.begin() + cursor[tab]);
		SaveProfiles();
	}

	bool ZScriptTicked() const
	{
		for (size_t i = 0; i < order.size(); ++i)
			if (Kind(order[i]) == 2)
				return true;
		return false;
	}

	// A separate engine CIA runs the selection: GZDoom or UZDoom chosen, or Auto with a ZScript-only mod ticked
	// (GZDoom until UZDoom is proven on hardware).
	bool UseGZDoom() const
	{
		return engine == ENGINE_GZDOOM || engine == ENGINE_UZDOOM || (engine == ENGINE_AUTO && ZScriptTicked());
	}
	bool UseUZDoom() const
	{
		return engine == ENGINE_UZDOOM;
	}

	void CycleEngine()
	{
		engine = (engine + 1) % ENGINE_COUNT;
		zscriptWarned = false;
		message = engine == ENGINE_ZDOOM ? "Engine: ZDoom 2.8.1 (fast, DECORATE/ACS mods)" :
			engine == ENGINE_GZDOOM ? "Engine: GZDoom 4.7.1 - experimental, slow (ZScript mods)" :
			engine == ENGINE_UZDOOM ? "Engine: UZDoom 5.0.3 - experimental (newest ZScript mods)" :
			"Engine: Auto - GZDoom only for ZScript mods, else ZDoom";
	}

	bool CanLaunch()
	{
		if (iwad.empty())
		{
			message = iwads.empty() ? "No IWAD found - see the top screen" : "Pick an IWAD first (IWAD tab)";
			tab = TAB_IWAD;
			return false;
		}
		for (size_t i = 0; i < order.size() && !zscriptWarned && !UseGZDoom(); ++i)
			if (Kind(order[i]) == 2)
			{	// ZScript-only mods need GZDoom 3+; warn once, a second START launches anyway
				message = order[i] + " needs GZDoom (Engine button / ZR). START again to try ZDoom anyway";
				zscriptWarned = true;
				return false;
			}
		return true;
	}

	// Engine arguments for the current selection.
	std::vector<std::string> BuildArgs() const
	{
		std::vector<std::string> args, files(1, BD3DS_PATH_IWAD_DIR + iwad), deh;
		for (size_t i = 0; i < order.size(); ++i)
			if (!IsDeh(order[i]))
				files.push_back(BD3DS_PATH_MOD_DIR + order[i]);
		unsigned palkey = I3DS_PaletteKey(files);
		args.push_back("-iwad");
		args.push_back(files[0]);
		std::vector<std::string> load;
		for (size_t i = 0; i < order.size(); ++i)
		{
			std::string path = BD3DS_PATH_MOD_DIR + order[i];
			if (IsDeh(order[i]))
			{
				deh.push_back(path);
				continue;
			}
			load.push_back(path);
			if (!modCache)
				continue;
			std::string cache = I3DS_ModCacheFile(path.c_str(), I3DS_ModKey(path.c_str(), palkey), "pk3");
			struct stat st;
			if (stat(cache.c_str(), &st) == 0)
				load.push_back(cache);
		}
		if (!load.empty())
		{
			args.push_back("-file");
			args.insert(args.end(), load.begin(), load.end());
		}
		if (!deh.empty())
		{
			args.push_back("-deh");
			args.insert(args.end(), deh.begin(), deh.end());
		}
		if (!modCache)
			args.push_back("-nomodcache");	// modcache_3ds.cpp: no conversion on this launch
		return args;
	}

	// GZDoom engine command line: plain files, no ZDoom cache pk3s.
	std::vector<std::string> BuildGZArgs() const
	{
		std::vector<std::string> args, load, deh;
		args.push_back("-iwad");
		args.push_back(BD3DS_PATH_IWAD_DIR + iwad);
		for (size_t i = 0; i < order.size(); ++i)
			(IsDeh(order[i]) ? deh : load).push_back(BD3DS_PATH_MOD_DIR + order[i]);
		if (!load.empty())
		{
			args.push_back("-file");
			args.insert(args.end(), load.begin(), load.end());
		}
		if (!deh.empty())
		{
			args.push_back("-deh");
			args.insert(args.end(), deh.begin(), deh.end());
		}
		return args;
	}

	// Writes the engine's launch file and asks APT to start its CIA (GZDoom or UZDoom) when this app exits. False
	// (with a message) when the engine is not installed or the launch file cannot be written.
	bool StartGZDoom()
	{
		const bool uz = UseUZDoom();
		const char *launchPath = uz ? BD3DS_PATH_UZ_LAUNCH : BD3DS_PATH_GZ_LAUNCH;
		const u64 tid = 0x0004000000000000ULL | ((u64)(uz ? BD3DS_UZ_UNIQUE_ID : BD3DS_GZ_UNIQUE_ID) << 8);
		if (R_SUCCEEDED(amInit()))	// a 3DSX may not get the AM service: then just try the chainload
		{
			AM_TitleEntry info;
			Result r = AM_GetTitleInfo(MEDIATYPE_SD, 1, (u64 *)&tid, &info);
			amExit();
			if (R_FAILED(r))
			{
				message = uz ? "UZDoom engine not installed: install udl-uzdoom.cia" : "GZDoom engine not installed: install udl-gzdoom.cia";
				return false;
			}
		}
		std::string dir = launchPath;
		mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0777);
		FILE *f = fopen(launchPath, "w");
		if (f == NULL)
		{
			message = std::string("Cannot write ") + launchPath;
			return false;
		}
		std::vector<std::string> args = BuildGZArgs();
		for (size_t i = 0; i < args.size(); ++i)
			fprintf(f, "%s\n", args[i].c_str());
		fclose(f);
		aptSetChainloader(tid, MEDIATYPE_SD);
		I3DS_HomeTrace(uz ? "launcher: the app closes and APT starts the UZDoom engine CIA" : "launcher: the app closes and APT starts the GZDoom engine CIA");
		return true;
	}

	// ---- drawing -----------------------------------------------------------------------------------

	void Text(const std::string &s, float x, float y, u32 color, float scale = 0.5f, bool center = false, float w = 0)
	{
		C2D_Text t;
		C2D_TextParse(&t, buf, s.c_str());
		C2D_TextOptimize(&t);
		float tw, th;
		C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
		if (center)
			x += (w - tw) / 2;
		C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
	}

	static std::string SizeText(const std::string &path)
	{
		struct stat st;
		char s[32];
		if (stat(path.c_str(), &st) != 0)
			return "";
		snprintf(s, sizeof(s), st.st_size >= 1 << 20 ? "%.1f MB" : "%.0f KB", st.st_size / (st.st_size >= 1 << 20 ? 1048576.0 : 1024.0));
		return s;
	}

	// Per-mod status for the top screen ("cached", "dehacked" or the file size). Cache keys read the files,
	// so this is recomputed only when the selection changes.
	std::vector<std::string> status, statusOrder;
	std::string statusIwad;
	int statusEngine = -1;

	void RefreshStatus()
	{
		if (order == statusOrder && iwad == statusIwad && engine == statusEngine)
			return;
		statusOrder = order;
		statusIwad = iwad;
		statusEngine = engine;
		std::vector<std::string> args = BuildArgs();
		status.assign(order.size(), "");
		for (size_t i = 0; i < order.size(); ++i)
		{
			std::string path = BD3DS_PATH_MOD_DIR + order[i];
			std::vector<std::string>::iterator it = std::find(args.begin(), args.end(), path);
			bool cached = it != args.end() && it + 1 != args.end() &&
				(it + 1)->compare(0, strlen(BD3DS_PATH_CACHE_DIR), BD3DS_PATH_CACHE_DIR) == 0;
			status[i] = IsDeh(order[i]) ? "dehacked" : Kind(order[i]) == 2 && !UseGZDoom() ? "needs GZDoom" :
				cached && !UseGZDoom() && modCache ? "cached" : SizeText(path);
		}
	}

	void DrawTop()
	{
		C2D_TargetClear(top, COL_BG);
		C2D_SceneBegin(top);
		const bd3ds_ui_t &title = Ui("title"), &sum = Ui("summary"), &hints = Ui("hints");
		C2D_DrawRectSolid(title.x, title.y, 0.4f, title.w, title.h, COL_HILITE);
		Text(title.label, title.x + 8, title.y + 3, COL_GOLD, 0.6f);
		const bd3ds_ui_t &credit = Ui("credit");	// right-aligned in its box
		{
			C2D_Text t;
			C2D_TextParse(&t, buf, credit.label);
			C2D_TextOptimize(&t);
			float tw, th;
			C2D_TextGetDimensions(&t, 0.45f, 0.45f, &tw, &th);
			C2D_DrawText(&t, C2D_WithColor, credit.x + credit.w - tw, credit.y + (credit.h - th) / 2, 0.5f, 0.45f, 0.45f, COL_TEXT);
		}
		float y = sum.y + 4;
		if (iwads.empty())
		{
			Text("No IWAD found. Copy your Doom game files to:", 10, y, COL_ERR); y += 20;
			Text(BD3DS_PATH_IWAD_DIR, 20, y, COL_GOLD); y += 26;
			Text("(doom.wad, doom2.wad, plutonia.wad, tnt.wad, heretic.wad,", 10, y, COL_TEXT); y += 18;
			Text(" hexen.wad, freedoom1.wad, freedoom2.wad, ...)", 10, y, COL_TEXT); y += 26;
			Text("Mods (.pk3 .wad .zip .deh) go to:", 10, y, COL_TEXT); y += 20;
			Text(BD3DS_PATH_MOD_DIR, 20, y, COL_GOLD);
		}
		else
		{
			Text("IWAD", 10, y, COL_DIM);
			Text(iwad.empty() ? "(none selected)" : iwad + "   " + SizeText(BD3DS_PATH_IWAD_DIR + iwad), 60, y, iwad.empty() ? COL_ERR : COL_TEXT);
			y += 22;
			Text(order.empty() ? "No mods - plain game" : "Load order", 10, y, COL_DIM);
			Text(UseUZDoom() ? "UZDoom 5.0.3 (experimental)" : UseGZDoom() ? "GZDoom 4.7.1 (experimental)" : "ZDoom 2.8.1", 230, y,
				UseGZDoom() ? COL_GOLD : COL_DIM, 0.45f);
			y += 20;
			RefreshStatus();
			for (size_t i = 0; i < order.size() && y < sum.y + sum.h - 18; ++i, y += 18)
			{
				Text(std::to_string(i + 1) + ". " + order[i], 20, y, COL_TEXT, 0.48f);
				Text(status[i], 290, y, status[i] == "cached" ? COL_GOLD : status[i] == "needs GZDoom" ? COL_ERR : COL_DIM, 0.42f);
			}
		}
		if (!message.empty())
			Text(message, 10, sum.y + sum.h - 16, COL_GOLD, 0.45f);
		C2D_DrawRectSolid(hints.x, hints.y, 0.4f, hints.w, hints.h, COL_PANEL);
		Text(tab == TAB_MODS ? "A tick  X/Y order  ZR engine  SELECT save  START launch" :
			 tab == TAB_PROFILES ? "A load  B delete  SELECT save  START launch  L/R tab" :
			 tab == TAB_OPTIONS ? "A change / run the option  START launch  L/R tab" :
			 "A pick IWAD  SELECT save profile  START launch  L/R tab", hints.x + 6, hints.y + 4, COL_DIM, 0.45f);
	}

	void DrawButton(const bd3ds_ui_t &u, bool active, bool enabled, const char *label = NULL)
	{
		C2D_DrawRectSolid(u.x + 1, u.y + 1, 0.4f, u.w - 2, u.h - 2, active ? COL_HILITE : COL_BUTTON);
		C2D_DrawRectSolid(u.x + 1, u.y + 1, 0.45f, u.w - 2, 1, C2D_Color32(170, 160, 150, 255));	// bevel
		C2D_DrawRectSolid(u.x + 1, u.y + u.h - 2, 0.45f, u.w - 2, 1, C2D_Color32(20, 10, 8, 255));
		Text(label ? label : u.label, u.x, u.y + (u.h - 15) / 2.0f, enabled ? (active ? COL_GOLD : COL_TEXT) : COL_DIM, 0.5f, true, u.w);
	}

	void DrawBottom()
	{
		C2D_TargetClear(bottom, COL_BG);
		C2D_SceneBegin(bottom);
		static const char *const tabs[TAB_COUNT] = { "tab_iwad", "tab_mods", "tab_profiles", "tab_options" };
		for (int t = 0; t < TAB_COUNT; ++t)
			DrawButton(Ui(tabs[t]), t == tab, true);
		const bd3ds_ui_t &list = Ui("list");
		C2D_DrawRectSolid(list.x, list.y, 0.3f, list.w, list.h, COL_PANEL);
		std::vector<std::string> rows = tab == TAB_IWAD ? iwads : tab == TAB_MODS ? ModRows() :
			tab == TAB_OPTIONS ? OptionRows() : std::vector<std::string>();
		if (tab == TAB_PROFILES)
			for (size_t i = 0; i < profiles.size(); ++i)
				rows.push_back(profiles[i].name + "  (" + profiles[i].iwad + (profiles[i].mods.empty() ? "" : " + " + std::to_string(profiles[i].mods.size()) + " mods") + ")");
		if (rows.empty())
			Text(tab == TAB_PROFILES ? "No profiles yet: SELECT / Save stores one" : "Folder is empty", list.x + 8, list.y + 8, COL_DIM);
		for (int r = 0; r < VisibleRows() && scroll[tab] + r < (int)rows.size(); ++r)
		{
			int i = scroll[tab] + r;
			float y = list.y + r * ROW_H;
			if (i == cursor[tab])
				C2D_DrawRectSolid(list.x, y, 0.35f, list.w, ROW_H, COL_HILITE);
			std::string mark;
			u32 col = COL_TEXT;
			if (tab == TAB_IWAD)
				mark = rows[i] == iwad ? "> " : "   ";
			else if (tab == TAB_MODS)
			{
				std::vector<std::string>::iterator it = std::find(order.begin(), order.end(), rows[i]);
				mark = it != order.end() ? std::to_string(it - order.begin() + 1) + ". " : "-  ";
				col = it != order.end() ? COL_TEXT : COL_DIM;
				if (Kind(rows[i]) == 2)	// ZScript only: will not run
					Text("[ZScript]", list.x + list.w - 62, y + 4, COL_ERR, 0.42f);
			}
			Text(mark + rows[i], list.x + 6, y + 3, i == cursor[tab] ? COL_GOLD : col, 0.5f);
		}
		if ((int)rows.size() > VisibleRows())	// scroll bar
		{
			float h = list.h * VisibleRows() / (float)rows.size();
			float y = list.y + (list.h - h) * scroll[tab] / (float)(rows.size() - VisibleRows());
			C2D_DrawRectSolid(list.x + list.w - 4, y, 0.4f, 3, h, COL_DIM);
		}
		DrawButton(Ui("btn_up"), false, tab == TAB_MODS);
		DrawButton(Ui("btn_down"), false, tab == TAB_MODS);
		DrawButton(Ui("btn_save"), false, true);
		DrawButton(Ui("btn_delete"), false, tab == TAB_PROFILES);
		{
			const bd3ds_ui_t &u = Ui("btn_engine");
			std::string labels = u.label;	// "Auto|ZDoom|GZDoom|UZDoom"
			size_t a = 0;
			for (int e = 0; e < engine && a != std::string::npos; ++e)
				a = labels.find('|', a) + 1;
			DrawButton(u, engine != ENGINE_AUTO, true, labels.substr(a, labels.find('|', a) - a).c_str());
		}
		DrawButton(Ui("btn_launch"), true, true);
	}

	// ---- input -------------------------------------------------------------------------------------

	void SetTab(int t)
	{
		tab = t;
		message.clear();
		if (tab == TAB_OPTIONS)
			MeasureCache();
	}

	// Returns true when Launch was requested.
	bool Action(const char *action)
	{
		message.clear();
		if (!strcmp(action, "select")) Select();
		else if (!strcmp(action, "move_up")) Move(-1);
		else if (!strcmp(action, "move_down")) Move(1);
		else if (!strcmp(action, "save_profile")) SaveProfile();
		else if (!strcmp(action, "delete_profile")) DeleteProfile();
		else if (!strcmp(action, "engine")) CycleEngine();
		else if (!strcmp(action, "launch")) return CanLaunch();
		ClampCursor();
		return false;
	}

	bool Input()
	{
		u32 down = hidKeysDown(), rep = hidKeysDownRepeat(), held = hidKeysHeld();
		if (rep & KEY_UP) { --cursor[tab]; ClampCursor(); }
		if (rep & KEY_DOWN) { ++cursor[tab]; ClampCursor(); }
		if (rep & KEY_LEFT) { cursor[tab] -= VisibleRows(); ClampCursor(); }
		if (rep & KEY_RIGHT) { cursor[tab] += VisibleRows(); ClampCursor(); }
		if (down & Ui("tab_iwad").key) SetTab((tab + TAB_COUNT - 1) % TAB_COUNT);
		if (down & Ui("tab_mods").key) SetTab((tab + 1) % TAB_COUNT);
		for (unsigned i = 0; i < BD3DS_LAUNCHER_COUNT; ++i)
		{
			const bd3ds_ui_t &u = bd3ds_launcher[i];
			if (u.key && (down & u.key) && strcmp(u.action, "tab") != 0)
			{
				if (!strcmp(u.action, "select") && tab == TAB_MODS)	// toggle the row under the cursor
				{
					std::vector<std::string> rows = ModRows();
					if (cursor[tab] < (int)rows.size())
						ToggleMod(rows[cursor[tab]]);
					ClampCursor();
				}
				else if (Action(u.action))
					return true;
			}
		}

		touchPosition tp;
		hidTouchRead(&tp);
		const bd3ds_ui_t &list = Ui("list");
		if (down & KEY_TOUCH)
		{
			touching = true;
			dragged = false;
			touchStartY = tp.py;
			touchScroll = scroll[tab];
		}
		else if ((held & KEY_TOUCH) && touching && tp.py >= list.y && tp.py < list.y + list.h)
		{
			int rows = (touchStartY - tp.py) / ROW_H;
			if (rows != 0 || dragged)
			{
				dragged = true;
				scroll[tab] = std::max(0, std::min(touchScroll + rows, std::max(0, RowCount() - VisibleRows())));
			}
		}
		else if (!(held & KEY_TOUCH) && touching)
		{
			touching = false;
			if (dragged)
				return false;
			int x = lastTouch.px, y = lastTouch.py;
			for (unsigned i = 0; i < BD3DS_LAUNCHER_COUNT; ++i)
			{
				const bd3ds_ui_t &u = bd3ds_launcher[i];
				if (strcmp(u.screen, "bottom") || x < u.x || x >= u.x + u.w || y < u.y || y >= u.y + u.h)
					continue;
				if (!strcmp(u.action, "tab"))
				{
					SetTab(!strcmp(u.element, "tab_iwad") ? TAB_IWAD : !strcmp(u.element, "tab_mods") ? TAB_MODS :
						!strcmp(u.element, "tab_options") ? TAB_OPTIONS : TAB_PROFILES);
				}
				else if (!strcmp(u.kind, "list"))
				{
					int row = scroll[tab] + (y - list.y) / ROW_H;
					if (row < RowCount())
					{
						cursor[tab] = row;
						if (tab == TAB_MODS)
							ToggleMod(ModRows()[row]);
						else
							Action("select");
					}
				}
				else if (Action(u.action))
					return true;
				break;
			}
		}
		if (held & KEY_TOUCH)
			lastTouch = tp;
		return false;
	}

	touchPosition lastTouch = {};
};

} // namespace

bool I3DS_RunLauncher(std::vector<std::string> &args)
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	hidSetRepeatParameters(18, 5);
	Launcher *l = new Launcher;
	l->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	l->bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	l->buf = C2D_TextBufNew(8192);
	l->ScanFiles();
	l->LoadSettings();
	l->LoadProfiles();
	if (l->iwad.empty() && l->iwads.size() == 1)
		l->iwad = l->iwads[0];
	l->ClampCursor();
	// Filesystem sheet: autolaunch - a profile name (or "last") starts it without the launcher screens. The
	// launcher is still drawn for a moment and dumped to fb_top/fb_bottom.raw (tools/fb2png.py) for testing.
	std::string autoname;
	if (FILE *f = fopen(BD3DS_PATH_AUTOLAUNCH, "r"))
	{
		char name[64] = "";
		if (fgets(name, sizeof(name), f))
			autoname = std::string(name).substr(0, strcspn(name, "\r\n"));
		fclose(f);
	}
	bool launch = false;
	for (int frame = 0; I3DS_AptMainLoop("launcher menu"); ++frame)
	{
		hidScanInput();
		if (!autoname.empty() && frame == 30)
		{
			I3DS_DumpScreen(GFX_TOP, BD3DS_PATH_APP_DIR "fb_top.raw");
			I3DS_DumpScreen(GFX_BOTTOM, BD3DS_PATH_APP_DIR "fb_bottom.raw");
			for (size_t i = 0; i < l->profiles.size(); ++i)
				if (l->profiles[i].name == autoname)
					l->Apply(l->profiles[i], false);
			launch = l->CanLaunch() && (!l->UseGZDoom() || l->StartGZDoom());
			if (launch)
				break;
			autoname.clear();
		}
		if (l->Input())
		{
			if (!l->UseGZDoom() || l->StartGZDoom())
			{
				launch = true;
				break;
			}
		}
		C2D_TextBufClear(l->buf);
		if (!I3DS_FrameBeginSafe())	// no unbounded VBlank wait: HOME must reach aptMainLoop (i_3ds.h)
			continue;
		l->DrawTop();
		l->DrawBottom();
		C3D_FrameEnd(0);
	}
	bool gz = launch && l->UseGZDoom();
	if (launch)
	{
		l->SaveProfiles();	// [last]
		args = l->BuildArgs();
	}
	C2D_TextBufDelete(l->buf);
	delete l;
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return launch && !gz;	// GZDoom: the app exits and APT starts the engine CIA
}
