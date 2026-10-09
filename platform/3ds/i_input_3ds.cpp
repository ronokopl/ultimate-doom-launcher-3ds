// 3DS buttons + touch -> ZDoom events (replaces posix/sdl/i_input.cpp). Subsystems sheet: input.
// Button table = generated from the Controls sheet (bd3ds_sheets.h). Keys are KEY_PAD_*, so the engine's
// bind system/controls menu remaps them and the menu navigates with A/B/D-pad.
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "c_bind.h"
#include "d_event.h"
#include "d_gui.h"
#include "m_joy.h"
#include "i_input.h"
#include "bd3ds_sheets.h"
#include "i_3ds.h"
#include "csnd_3ds.h"
#include "m_argv.h"

CVAR (Bool,  use_mouse,		false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Bool,  m_noprescale,	false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Bool,	 m_filter,		false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
// Menu sheet (3DS Options): L <-> ZL and R <-> ZR before the buttons become engine keys, so a mod that fires on
// the bumpers instead of the triggers needs one switch instead of four rebinds. The gyro button stays physical.
CVAR (Bool,  in_3dsswapshoulders, BD3DS_DEFAULT_IN_3DSSWAPSHOULDERS, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

static u32 SwapShoulders(u32 keys)
{
	const u32 pairs[2][2] = { { KEY_L, KEY_ZL }, { KEY_R, KEY_ZR } };
	for (int i = 0; i < 2; ++i)
	{
		u32 a = pairs[i][0], b = pairs[i][1], both = keys & (a | b);
		if (both != 0 && both != (a | b))
			keys ^= a | b;
	}
	return keys;
}

void I_ProcessJoysticks();

static u32 PrevHeld;
static bool TouchWasDown;
static bool DefaultsBound;

// Controls sheet defaults go in once, as engine default bindings (users can still rebind).
static void BindDefaults()
{
	for (unsigned i = 0; i < BD3DS_BUTTON_COUNT; ++i)
	{
		const bd3ds_button_t &b = bd3ds_buttons[i];
		if (b.bind != NULL && Bindings.GetBinding(b.zdoom_key).IsEmpty())
			Bindings.SetBind(b.zdoom_key, b.bind);
	}
	DefaultsBound = true;
}

bool I3DS_AppRunning()
{
	return aptMainLoop();
}

// ---- HOME / sleep safety ----------------------------------------------------------------------------------

static u64 LastFrameMs;

bool I3DS_FrameBeginSafe()
{
	u64 now = osGetTime();
	if (now - LastFrameMs < 16)	// 60 Hz pacing by sleeping, not by waiting for VBlank events
		svcSleepThread((s64)(16 - (now - LastFrameMs)) * 1000000);
	for (u64 start = osGetTime(); !C3D_FrameBegin(C3D_FRAME_NONBLOCK); svcSleepThread(1000000))
		if (aptShouldJumpToHome() || aptShouldClose() || osGetTime() - start > 250)
			return false;	// GPU still busy: skip this frame so the caller gets back to aptMainLoop()
	LastFrameMs = osGetTime();
	return true;
}

void I3DS_WaitFrame()
{
	svcSleepThread(16 * 1000000LL);
}

static bool AptLoop();

void I3DS_PumpApt()
{
	static u64 last;
	u64 now = osGetTime();
	if (now - last < 50)
		return;
	last = now;
	svcSleepThread(100000);	// the APT event thread runs below the main thread's priority: let it see the button
	if (!AptLoop())
		exit(0);
}

// HOME trace (Filesystem app_dir: home_trace.txt): each step of a HOME / sleep transition, written synchronously
// (O_SYNC + fsync) as it happens, so a lock-up leaves the last step that was reached on the SD card. Every app
// start appends a header (sessions are kept: a later launch must not erase the trace of a lock-up); past 32 KB
// the file moves to home_trace_prev.txt. Times are seconds since the app started.
#define HOME_TRACE_PATH BD3DS_PATH_APP_DIR "home_trace.txt"
static u64 TraceStartMs;
static LightLock TraceLock;	// one writer at a time: an O_APPEND write is "get size, write there" on the SD card
static __thread bool InTrace;		// this thread is writing the trace: its own file IPC is not traced
static __thread bool IpcTraceThread;	// threads with room on the stack for the IPC trace (main + ours, not libctru's)

void I3DS_HomeTrace(const char *fmt, ...)
{
	bool was = InTrace;
	InTrace = true;
	char line[224];
	u64 t = osGetTime() - TraceStartMs;
	int n = snprintf(line, sizeof(line), "%5lu.%03lu ", (unsigned long)(t / 1000), (unsigned long)(t % 1000));
	va_list ap;
	va_start(ap, fmt);
	int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	n += m > 0 ? m : 0;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	LightLock_Lock(&TraceLock);
	int fd = open(HOME_TRACE_PATH, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666);
	if (fd >= 0)
	{
		write(fd, line, n);
		fsync(fd);
		close(fd);
	}
	LightLock_Unlock(&TraceLock);
	InTrace = was;
}

void I3DS_HomeTraceThread()
{
	IpcTraceThread = true;
}

// IPC trace: while a HOME / close handover runs (or the -hometest one), every IPC request a traced thread sends
// (APT, GSP, DSP, FS, ...: command header + session) is written before it goes out (Build sheet ld_extra:
// --wrap=svcSendSyncRequest), so the last line names the request a lock-up waits on. Polling loops are folded
// by command header ("x N" at powers of two; APT opens a new session per command). The thread's IPC command
// buffer and static buffer descriptors are saved around the trace write, which does file IPC of its own.
static volatile bool IpcTrace;
static int IpcLines;
static u32 IpcLastHeader, IpcRepeats;

extern "C" Result __real_svcSendSyncRequest(Handle session);
extern "C" Result __wrap_svcSendSyncRequest(Handle session)
{
	if (!IpcTrace || InTrace || !IpcTraceThread)
		return __real_svcSendSyncRequest(session);
	u32 *cmd = getThreadCommandBuffer();
	u32 header = cmd[0];
	if (header == 0x00050100)	// srv GetServiceHandle: every APT command opens its own session first
		return __real_svcSendSyncRequest(session);
	bool repeat = header == IpcLastHeader;
	IpcRepeats = repeat ? IpcRepeats + 1 : 1;
	IpcLastHeader = header;
	if ((!repeat || (IpcRepeats & (IpcRepeats - 1)) == 0) && IpcLines < 400)
	{
		++IpcLines;
		u32 saved[64 + 32];
		memcpy(saved, cmd, 64 * sizeof(u32));
		memcpy(saved + 64, getThreadStaticBuffers(), 32 * sizeof(u32));
		const char *service = session == *gspGetSessionHandle() ? "gsp" : session == *fsGetSessionHandle() ? "fs" : "";
		u32 tid = 0;
		svcGetThreadId(&tid, CUR_THREAD_HANDLE);
		if (repeat)
			I3DS_HomeTrace("  ipc %08lX %s x%lu", (unsigned long)header, service, (unsigned long)IpcRepeats);
		else
			I3DS_HomeTrace("ipc %08lX %s session %lX thread %lu%s", (unsigned long)header, service,
				(unsigned long)session, (unsigned long)tid, threadGetCurrent() == NULL ? " (main)" : "");
		memcpy(cmd, saved, 64 * sizeof(u32));
		memcpy(getThreadStaticBuffers(), saved + 64, 32 * sizeof(u32));
	}
	return __real_svcSendSyncRequest(session);
}

static void IpcTraceBegin()
{
	IpcLines = 0;
	IpcLastHeader = 0;
	IpcRepeats = 0;
	IpcTrace = true;
}

static volatile bool InAptLoop, Sleeping;
static volatile u64 InAptSince;
static aptHookCookie FirstHook, LastHook;
static dspHookCookie FirstDspHook, LastDspHook;

// Hooks registered later run earlier: FirstHook (registered after the GPU and sound started) runs before citro3d's
// GPU wait and NDSP; LastHook (registered at startup) runs after them, which brackets those steps in the trace.
static void FirstHookFn(APT_HookType hook, void *)
{
	switch (hook)
	{
	case APTHOOK_ONSUSPEND:	// HOME Menu (or an applet): the sound hardware closes completely, not just sleeps
		I3DS_HomeTrace("suspend: hooks start (gyro off, sound closed)");
		sndSetSuspended(true);
		I3DS_SoundQuiesce();
		I3DS_SoundRelease();
		I3DS_AimSuspend();
		I3DS_BottomBacklightRestore();	// Screen off mode: the HOME Menu gets a lit bottom screen
		break;
	case APTHOOK_ONSLEEP:
		I3DS_HomeTrace("sleep: hooks start");
		Sleeping = true;
		sndSetSuspended(true);
		I3DS_SoundQuiesce();
		I3DS_AimSuspend();
		break;
	case APTHOOK_ONRESTORE:
		I3DS_HomeTrace("restore: hooks start");
		I3DS_SoundRestore();
		sndSetSuspended(false);
		break;
	case APTHOOK_ONWAKEUP:
		I3DS_HomeTrace("wakeup: hooks start");
		sndSetSuspended(false);
		Sleeping = false;
		break;
	default:
		break;
	}
}

static void LastHookFn(APT_HookType hook, void *)
{
	if (hook == APTHOOK_ONSUSPEND) I3DS_HomeTrace("suspend: hooks done (GPU idle) -> screen capture, DSP sleep, jump");
	else if (hook == APTHOOK_ONRESTORE) I3DS_HomeTrace("restore: done, back in the app");
	else if (hook == APTHOOK_ONSLEEP) I3DS_HomeTrace("sleep: hooks done");
	else if (hook == APTHOOK_ONWAKEUP) I3DS_HomeTrace("wakeup: done");
	else if (hook == APTHOOK_ONEXIT) I3DS_HomeTrace("exit: APT hooks (app closing)");
}

static void FirstDspHookFn(DSP_HookType hook)
{
	if (hook == DSPHOOK_ONSLEEP) I3DS_HomeTrace("dsp: sleep start");
	else if (hook == DSPHOOK_ONWAKEUP) I3DS_HomeTrace("dsp: wakeup start");
	else if (hook == DSPHOOK_ONCANCEL) I3DS_HomeTrace("dsp: cancel start");
}

static void LastDspHookFn(DSP_HookType hook)
{
	if (hook == DSPHOOK_ONSLEEP) I3DS_HomeTrace("dsp: sleep done -> release GPU, jump to HOME");
	else if (hook == DSPHOOK_ONWAKEUP) I3DS_HomeTrace("dsp: wakeup done");
	else if (hook == DSPHOOK_ONCANCEL) I3DS_HomeTrace("dsp: cancel done");
}

void I3DS_HomeTraceInit()
{
	TraceStartMs = osGetTime();
	LightLock_Init(&TraceLock);
	IpcTraceThread = true;	// main thread (1 MB stack)
	struct stat st;
	if (stat(HOME_TRACE_PATH, &st) == 0 && st.st_size > 32 * 1024)
	{
		remove(BD3DS_PATH_APP_DIR "home_trace_prev.txt");
		rename(HOME_TRACE_PATH, BD3DS_PATH_APP_DIR "home_trace_prev.txt");
	}
	time_t now = time(NULL);
	char when[32] = "";
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", gmtime(&now));	// the 3DS clock is local time
	I3DS_HomeTrace("==== %s started %s", BD3DS_TITLE, when);
	aptHook(&LastHook, LastHookFn, NULL);
	dspHook(&LastDspHook, LastDspHookFn);
}

// Last resort if the game still misses a HOME press: after the grace time the process ends and the console goes
// back to the HOME Menu instead of needing a hard power-off. Two cases: the press is never handled (the main loop
// does not reach aptMainLoop), or the handover inside aptMainLoop stalls while the app is still active (a finished
// handover clears 'active': the supervisor then only reports how long the app has been in the HOME Menu, which
// tells a lock-up of the HOME Menu itself from one in the game). The sound hardware is released before ending.
static void SupervisorExit(const char *why)
{
	I3DS_HomeTrace("supervisor: %s, closing", why);
	sndEmergencyStop();
	svcExitProcess();
}

static void HomeSupervisor(void *)
{
	I3DS_HomeTraceThread();
	static const int awayMarks[] = { 1, 5, 15, 30, 60, 120 };
	u64 pendingSince = 0, awaySince = 0;
	int traced = 0, awayTraced = 0;
	for (;;)
	{
		bool active = aptIsActive();
		bool pending = active && (aptShouldJumpToHome() || aptShouldClose());
		u64 now = osGetTime();
		if (!pending)
			pendingSince = 0;
		else if (pendingSince == 0)
			pendingSince = now;
		else if (now - pendingSince > 12000)
			SupervisorExit("HOME press unanswered for 12 s");
		bool stalled = InAptLoop && active && !Sleeping;
		if (!stalled)
			traced = 0;
		else if (now - InAptSince > 2000 * (traced + 1) && traced < 5)
		{
			I3DS_HomeTrace("supervisor: still in the HOME handover after %lu ms", (unsigned long)(now - InAptSince));
			++traced;
		}
		if (stalled && now - InAptSince > 10000)
			SupervisorExit("HOME handover stalled for 10 s");
		bool away = InAptLoop && !active && !Sleeping;
		if (!away)
			awaySince = 0, awayTraced = 0;
		else if (awaySince == 0)
			awaySince = now;
		else if (awayTraced < (int)(sizeof(awayMarks) / sizeof(awayMarks[0])) && now - awaySince >= (u64)awayMarks[awayTraced] * 1000)
			I3DS_HomeTrace("supervisor: handover finished, the HOME Menu has had the screens for %d s", awayMarks[awayTraced++]);
		svcSleepThread(100 * 1000000LL);
	}
}

// Quitting: the exit handlers and libctru's relaunch of the launcher (aptExit chainload) run after this. If the
// process has not ended 8 s later it ends itself, so the console falls back to the HOME Menu instead of a black screen.
static void ExitWatchdog(void *)
{
	svcSleepThread(8000 * 1000000LL);
	I3DS_HomeTrace("quit: exit did not finish within 8 s, ending the process");
	svcExitProcess();
}

void I3DS_StartExitWatchdog(bool relaunch)
{
	I3DS_HomeTrace(relaunch ? "quit: exiting, then the launcher is relaunched" : "quit: exiting");
	if (threadCreate(ExitWatchdog, NULL, 4096, 0x18, 2, true) == NULL)
		threadCreate(ExitWatchdog, NULL, 4096, 0x18, -2, true);
}

// aptMainLoop with the HOME trace and the supervisor's bookkeeping (game loop, launcher menu)
bool I3DS_AptMainLoop(const char *where)
{
	bool home = aptShouldJumpToHome(), close = aptShouldClose();
	if (home || close)
	{
		I3DS_HomeTrace("%s (%s): entering aptMainLoop", home ? "HOME pressed" : "close requested", where);
		IpcTraceBegin();
	}
	InAptSince = osGetTime();
	InAptLoop = true;
	bool running = aptMainLoop();
	InAptLoop = false;
	if (home || close)
	{
		IpcTrace = false;
		I3DS_HomeTrace(running ? "left aptMainLoop: running again" : "left aptMainLoop: the app closes");
	}
	return running;
}

static bool AptLoop()
{
	static u64 testAt = Args->CheckParm("-hometest") ? osGetTime() + 30000 : 0;
	if (testAt && osGetTime() >= testAt)
	{	// emulator test (args.txt -hometest): the HOME handover 30 s in; Azahar has no HOME Menu, so it stalls in
		// libctru and the trace + supervisor must take over
		testAt = 0;
		I3DS_HomeTrace("hometest: aptJumpToHomeMenu");
		IpcTraceBegin();
		InAptSince = osGetTime();
		InAptLoop = true;
		aptJumpToHomeMenu();
		InAptLoop = false;
		IpcTrace = false;
	}
	static u64 soundAt = Args->CheckParm("-soundcycletest") ? osGetTime() + 30000 : 0;
	if (soundAt && osGetTime() >= soundAt)
	{	// emulator test (args.txt -soundcycletest): the HOME Menu's sound close / reopen every 20 s, without the
		// HOME Menu (Azahar cannot run it), so the restore path (music position, evicted sounds) can be checked
		soundAt = osGetTime() + 20000;
		I3DS_HomeTrace("soundcycletest: close");
		sndSetSuspended(true);
		I3DS_SoundQuiesce();
		I3DS_SoundRelease();
		svcSleepThread(1000 * 1000000LL);
		I3DS_SoundRestore();
		sndSetSuspended(false);
	}
	return I3DS_AptMainLoop("game");
}

static void StartHomeSupervisor()
{
	static bool started;
	if (started)
		return;
	started = true;
	aptHook(&FirstHook, FirstHookFn, NULL);	// after the GPU (citro3d) and NDSP hooks: runs before them
	dspHook(&FirstDspHook, FirstDspHookFn);
	// on the app core, not core 2: nothing of ours runs on core 2 while the HOME Menu is up (the sound refill
	// thread is stopped too); it sleeps between checks, so the HOME Menu loses no CPU time to it
	const char *core = threadCreate(HomeSupervisor, NULL, 8 * 1024, 0x2f, -2, true) != NULL ? "core 0" : "NOT STARTED";
	int mode = I3DS_CurrentMode(), w = 0, h = 0;
	if (mode >= 0)
		I3DS_ModeSize(mode, &w, &h);
	I3DS_HomeTrace("game running: audio %s, video %dx%d%s, linear memory free %lu KB, supervisor %s", sndBackendName(),
		w, h, gfxIsWide() ? " (800px LCD mode)" : "", (unsigned long)(linearSpaceFree() / 1024), core);
}

void I3DS_PollInput()
{
	StartHomeSupervisor();
	static bool sleepAllowed;
	if (!sleepAllowed)	// the game loop runs: sleep is handled from now on (disallowed while loading, i_main_3ds.cpp)
	{
		sleepAllowed = true;
		aptSetSleepAllowed(true);
	}
	svcSleepThread(200000);	// slow frames never sleep: give the APT event thread its turn every tic
	if (!AptLoop())
	{
		exit(0);	// HOME menu "close": run the normal exit path (config save etc.)
	}
	if (!DefaultsBound)
		BindDefaults();

	hidScanInput();
	u32 held = hidKeysHeld();
	if (in_3dsswapshoulders)
		held = SwapShoulders(held);	// switching it while a button is held sends the matching key up/down
	u32 changed = held ^ PrevHeld;
	for (unsigned i = 0; i < BD3DS_BUTTON_COUNT; ++i)
	{
		const bd3ds_button_t &b = bd3ds_buttons[i];
		if (changed & b.ctru_mask)
		{
			event_t ev = { 0 };
			ev.type = (held & b.ctru_mask) ? EV_KeyDown : EV_KeyUp;
			ev.data1 = b.zdoom_key;
			D_PostEvent(&ev);
		}
	}
	PrevHeld = held;

	bool touching = (held & KEY_TOUCH) != 0;
	if (touching || TouchWasDown)
	{
		touchPosition tp;
		hidTouchRead(&tp);
		I3DS_BottomTouch(tp.px, tp.py, touching);
	}
	TouchWasDown = touching;

	I3DS_AimUpdate();
	I_ProcessJoysticks();
}

void I_SetMouseCapture()
{
}

void I_ReleaseMouseCapture()
{
}

void I_PutInClipboard (const char *str)
{
}

FString I_GetFromClipboard (bool use_primary_selection)
{
	return "";
}

void I_GetEvent ()
{
	I3DS_PollInput();
}

void I_StartTic ()
{
	I3DS_PollInput();
}

void I_StartFrame ()
{
}
