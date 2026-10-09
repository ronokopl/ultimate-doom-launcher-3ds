// 3DS replacement for posix/sdl/critsec.h: FCriticalSection on a libctru LightLock.
// Only the synchronization header is pulled in so engine code never sees libctru's KEY_* macros.
#ifndef CRITSEC_H
#define CRITSEC_H

extern "C" {	// libctru sub-headers carry no C linkage of their own (3ds.h adds it)
#include <3ds/types.h>
#include <3ds/synchronization.h>
}

class FCriticalSection
{
public:
	FCriticalSection() { LightLock_Init(&Lock); }
	~FCriticalSection() {}
	void Enter() { LightLock_Lock(&Lock); }
	void Leave() { LightLock_Unlock(&Lock); }
private:
	LightLock Lock;
};

#endif
