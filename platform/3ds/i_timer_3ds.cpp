// 3DS tic timer (replaces posix/sdl/i_timer.cpp). Subsystems sheet: timer. Polled on osGetTime(), no thread.
#include <3ds.h>
#include <assert.h>

#include "basictypes.h"
#include "basicinlines.h"
#include "doomdef.h"
#include "i_system.h"
#include "templates.h"

static u64 BaseTime;
static DWORD TicStart;
static int TicFrozen;

unsigned int I_MSTime (void)
{
	return (unsigned int)(osGetTime() - BaseTime);
}

unsigned int I_FPSTime()
{
	return (unsigned int)osGetTime();
}

static int I_GetTimePolled (bool saveMS)
{
	if (TicFrozen != 0)
	{
		return TicFrozen;
	}
	DWORD tm = (DWORD)osGetTime();
	if (saveMS)
	{
		TicStart = tm;
	}
	return Scale(tm - (DWORD)BaseTime, TICRATE, 1000);
}

static int I_WaitForTicPolled (int prevtic)
{
	int time;
	assert (TicFrozen == 0);
	while ((time = I_GetTimePolled(false)) <= prevtic)
	{
		svcSleepThread (500000);	// 0.5 ms: let the sound/APT threads run
	}
	return time;
}

static void I_FreezeTimePolled (bool frozen)
{
	if (frozen)
	{
		assert(TicFrozen == 0);
		TicFrozen = I_GetTimePolled(false);
	}
	else
	{
		assert(TicFrozen != 0);
		int froze = TicFrozen;
		TicFrozen = 0;
		int now = I_GetTimePolled(false);
		BaseTime += (now - froze) * 1000 / TICRATE;
	}
}

fixed_t I_GetTimeFrac (uint32 *ms)
{
	DWORD now = (DWORD)osGetTime();
	if (ms) *ms = TicStart + (1000 / TICRATE);
	if (TicStart == 0)
	{
		return FRACUNIT;
	}
	return clamp<fixed_t> ((now - TicStart)*FRACUNIT*TICRATE/1000, 0, FRACUNIT);
}

void I_InitTimer ()
{
	BaseTime = osGetTime();
	I_GetTime = I_GetTimePolled;
	I_WaitForTic = I_WaitForTicPolled;
	I_FreezeTime = I_FreezeTimePolled;
}

void I_ShutdownTimer ()
{
}
