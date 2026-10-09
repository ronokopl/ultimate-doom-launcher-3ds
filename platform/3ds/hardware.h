// 3DS replacement for posix/hardware.h: same IVideo interface, LightSemaphore instead of POSIX semaphores.
#ifndef __HARDWARE_H__
#define __HARDWARE_H__

#include "i_video.h"
#include "v_video.h"

extern "C" {	// libctru sub-headers carry no C linkage of their own (3ds.h adds it)
#include <3ds/types.h>
#include <3ds/synchronization.h>
}

typedef LightSemaphore Semaphore;
#define SEMAPHORE_WAIT(sem) LightSemaphore_Acquire(&(sem), 1);
#define SEMAPHORE_SIGNAL(sem) LightSemaphore_Release(&(sem), 1);
#define SEMAPHORE_INIT(sem, shared, value) LightSemaphore_Init(&(sem), (value), 0x7fff);

class IVideo
{
 public:
	virtual ~IVideo () {}

	virtual EDisplayType GetDisplayType () = 0;
	virtual void SetWindowedScale (float scale) = 0;

	virtual DFrameBuffer *CreateFrameBuffer (int width, int height, bool fs, DFrameBuffer *old) = 0;

	virtual void StartModeIterator (int bits, bool fs) = 0;
	virtual bool NextMode (int *width, int *height, bool *letterbox) = 0;

	virtual bool SetResolution (int width, int height, int bits);

	virtual void DumpAdapters();
};

void I_InitGraphics ();
void I_ShutdownGraphics ();
void I_CreateRenderer();

extern Semaphore FPSLimitSemaphore;
void I_SetFPSLimit(int limit);

extern IVideo *Video;

#endif	// __HARDWARE_H__
