// Shared interface between the 3DS platform modules. Engine-safe: no libctru headers here.
#ifndef __I_3DS_H__
#define __I_3DS_H__

class DCanvas;

// ---- startup console (bottom screen text, before the GPU takes over) -------
void I3DS_ConsoleInit();
bool I3DS_ConsoleActive();
void I3DS_FatalScreen(const char *text);	// shows the error and waits for START

// ---- HOME / sleep safety (i_input_3ds.cpp) ---------------------------------
// The HOME Menu, the power button and closing the lid are only handled inside aptMainLoop(), so the main thread
// must reach it promptly and never block without a bound on a VBlank: C3D_FRAME_SYNCDRAW (C3D_FrameSync) waits for
// a VBlank on both screens with no timeout, and a HOME press could leave the game stuck there (console lock-up).
bool I3DS_FrameBeginSafe();	// C3D_FrameBegin paced to 60 Hz by sleeping; false = skip this frame (HOME pending)
void I3DS_WaitFrame();		// ~1/60 s pause for console loops (instead of gspWaitForVBlank)
void I3DS_PumpApt();		// call often from long loading work: handles HOME / sleep / close (exits on close)
void I3DS_HomeTraceInit();	// first thing in main: home_trace.txt + the hooks that run after citro3d / NDSP
void I3DS_HomeTrace(const char *fmt, ...);	// one line in home_trace.txt, written synchronously
void I3DS_HomeTraceThread();	// call at the start of a thread with >= 8 KB stack: its IPC joins the handover trace
bool I3DS_AptMainLoop(const char *where);	// aptMainLoop + HOME trace + supervisor bookkeeping (where = "game", ...)
void I3DS_StartExitWatchdog(bool relaunch);	// quitting: ends the process if exiting hangs (I_Quit)
void I3DS_SoundQuiesce();	// suspend hook: waits until the music stream thread is idle (ndspsound.cpp)
void I3DS_SoundRelease();	// HOME Menu suspend hook: closes the sound hardware (engine channels evicted)
void I3DS_SoundRestore();	// restore hook: reopens it and restarts the evicted channels

// ---- video modes (Video sheet) ---------------------------------------------
int  I3DS_ModeCount();
bool I3DS_ModeAllowed(int mode);
void I3DS_ModeSize(int mode, int *width, int *height);
int  I3DS_ModeForSize(int width, int height);	// -1 if none
int  I3DS_CurrentMode();

// ---- GPU presentation (video_3ds.cpp, citro2d/citro3d) ---------------------
void I3DS_GpuInit();
void I3DS_GpuShutdown();
bool I3DS_GpuActive();
void I3DS_GpuSetMode(int mode);
// Upload an 8-bit canvas through a 256-entry RGB565 palette into the top/bottom textures.
void I3DS_GpuUploadTop(const unsigned char *pixels, int pitch, int width, int height, const unsigned short *pal565);
void I3DS_GpuUploadBottom(const unsigned char *pixels, int pitch, int width, int height, const unsigned short *pal565);
void I3DS_GpuPresent();

// Render scale hooks for the software renderer (r_swrenderer.cpp patch): scale, canvas for the reduced 3D view,
// then the full-size view rectangle that the GPU composites the stretched view into.
float I3DS_RenderScale();	// vid_3dsrenderscale (1 = off)
DCanvas *I3DS_ScaledViewCanvas(int width, int height);
void I3DS_ScaledViewDone(int x, int y, int w, int h);	// records the view rect; the HUD is keyed transparent there

// Monster frame precache (precache_3ds.cpp, Subsystems sheet: precache), called from the engine's level precache
void I3DS_PrecacheSprites(unsigned char *spritelist);	// marks sprites[] indices (v_video.cpp GetHitlist patch)
void I3DS_PrecacheSounds();	// marks sounds used (s_sound.cpp S_PrecacheLevel patch)

// Mod cache (modcache_3ds.cpp, ModCache sheet): d_main.cpp patch, after R_Init
void I3DS_ModCacheBuild();

// ---- bottom screen (bottom_3ds.cpp) -----------------------------------------
void I3DS_BottomInit();
void I3DS_BottomShutdown();
void I3DS_BottomRender(const unsigned short *pal565);	// automap -> bottom texture
void I3DS_BottomOverlay();							// touch buttons (citro2d), inside the bottom scene
bool I3DS_BottomTouch(int x, int y, bool down);		// true if a touch button consumed it
void I3DS_BottomBacklightRestore();				// undo the Screen off mode (quit, error screen)
bool I3DS_BottomCanvasVisible();					// the current bottom mode shows the automap canvas

// ---- aiming (aim_3ds.cpp, Subsystems sheet: aim) ----------------------------
void I3DS_AimUpdate();							// every input poll: gyro
void I3DS_AimTouch(int dx, int dy);				// stylus drag on an aim area, in pixels
void I3DS_AimAdd(float yawdeg, float pitchdeg);
bool I3DS_TouchAimEnabled();
void I3DS_AimShutdown();
void I3DS_AimSuspend();						// HOME / sleep: gyroscope off (the next update turns it back on)

// ---- input (i_input_3ds.cpp) -------------------------------------------------
void I3DS_PollInput();
bool I3DS_AppRunning();	// false after the HOME menu asked us to close

// ---- hitch profiler (profile_3ds.cpp, enabled by -profile3ds) ----------------
void I3DS_ProfileFrame();
void I3DS_ProfileDump();	// call before C3D_FrameBegin: previous frame is complete

#endif
