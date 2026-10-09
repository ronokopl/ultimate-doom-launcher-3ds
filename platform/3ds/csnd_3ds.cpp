// Sound output backend (Subsystems sheet: csnd, dspdump; Audio sheet: output).
// ndspsound.cpp drives libctru's NDSP through these snd* wrappers. At start the DSP firmware is extracted from
// the console if it is missing (dspdump_3ds.cpp); if NDSP still cannot start, the same channel/wave-buffer model
// is emulated by a software mixer whose stereo output loops through two CSND hardware channels (CSND needs no
// DSP firmware). Mixer: 24 channels, PCM16 mono/stereo wave-buffer queues, linear resampling, per-channel L/R
// gains (mix[0], mix[1]); output ring buffer written half a buffer ahead of the hardware play cursor, which is
// derived from the system tick (the CSND sample clock is SYSCLOCK_ARM11 / 4 divided by the channel timer).
#include <3ds.h>
#include <string.h>
#include <stdio.h>

#include "m_argv.h"
#include "c_console.h"
#include "csnd_3ds.h"
#include "i_3ds.h"

int I3DS_EnsureDspFirmware();

namespace {

const int CHANNELS = 24;
const u32 OUT_RATE = 32728;		// NDSP's native rate
const u32 RING = 4096;			// frames per output channel (~125 ms)

struct SoftChn
{
	bool paused, stereo;
	float rate, mix[2];
	ndspWaveBuf *head, *tail;
	double pos;					// frame position in head
};

bool UseCsnd;
const char *Backend = "off";
volatile bool Suspended;
SoftChn Chn[CHANNELS];
float MasterVol = 1.f;
LightLock Lock;
s16 *Ring[2];
int HwChn[2] = { -1, -1 };
double HwRate;
u64 StartTick;
u32 Written;					// frames written since start (ring position = Written % RING)
Thread Mixer;
volatile bool MixerRun;

void ResetChn(SoftChn &c)
{
	for (ndspWaveBuf *b = c.head; b != NULL; b = b->next)
		b->status = NDSP_WBUF_DONE;
	memset(&c, 0, sizeof(c));
	c.rate = OUT_RATE;
	c.mix[0] = c.mix[1] = 1.f;
}

// Mixes `frames` output frames into the ring at `at`.
void MixInto(u32 at, u32 frames)
{
	static int acc[2][RING];
	memset(acc, 0, sizeof(int) * 2 * frames);
	for (int ch = 0; ch < CHANNELS; ++ch)
	{
		SoftChn &c = Chn[ch];
		if (c.paused || c.head == NULL)
			continue;
		double step = c.rate / OUT_RATE;
		int gl = (int)(c.mix[0] * MasterVol * 256), gr = (int)(c.mix[1] * MasterVol * 256);
		for (u32 f = 0; f < frames && c.head != NULL; ++f)
		{
			ndspWaveBuf *b = c.head;
			b->status = NDSP_WBUF_PLAYING;
			u32 i = (u32)c.pos, n = b->nsamples;
			int frac = (int)((c.pos - i) * 256);
			const s16 *d = b->data_pcm16;
			if (c.stereo)
			{
				u32 j = i + 1 < n ? i + 1 : i;
				int l = d[i * 2] + (((d[j * 2] - d[i * 2]) * frac) >> 8);
				int r = d[i * 2 + 1] + (((d[j * 2 + 1] - d[i * 2 + 1]) * frac) >> 8);
				acc[0][f] += (l * gl) >> 8;
				acc[1][f] += (r * gr) >> 8;
			}
			else
			{
				u32 j = i + 1 < n ? i + 1 : i;
				int s = d[i] + (((d[j] - d[i]) * frac) >> 8);
				acc[0][f] += (s * gl) >> 8;
				acc[1][f] += (s * gr) >> 8;
			}
			c.pos += step;
			if (c.pos >= n)
			{
				if (b->looping)
					c.pos -= n;
				else
				{
					c.pos -= n;
					b->status = NDSP_WBUF_DONE;
					c.head = b->next;
					if (c.head == NULL)
						c.tail = NULL, c.pos = 0;
				}
			}
		}
	}
	for (int side = 0; side < 2; ++side)
	{
		s16 *out = Ring[side];
		for (u32 f = 0; f < frames; ++f)
		{
			int v = acc[side][f];
			out[(at + f) % RING] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
}

void MixerThread(void *)
{
	I3DS_HomeTraceThread();
	while (MixerRun)
	{
		if (sndSuspended())	// HOME / sleep: leave the hardware alone (skips ahead on resume)
		{
			svcSleepThread(5 * 1000000LL);
			continue;
		}
		u64 played = (u64)((svcGetSystemTick() - StartTick) * HwRate / SYSCLOCK_ARM11);
		u32 target = (u32)played + RING / 2;
		if (target > Written)
		{
			u32 frames = target - Written;
			if (frames > RING / 2)	// fell behind (thread starved): skip ahead instead of mixing stale audio
			{
				Written = target - RING / 2;
				frames = RING / 2;
			}
			LightLock_Lock(&Lock);
			u32 at = Written % RING, first = frames < RING - at ? frames : RING - at;
			MixInto(at, first);
			if (frames > first)
				MixInto(0, frames - first);
			LightLock_Unlock(&Lock);
			Written += frames;
			GSPGPU_FlushDataCache(Ring[0], RING * sizeof(s16));
			GSPGPU_FlushDataCache(Ring[1], RING * sizeof(s16));
		}
		svcSleepThread(5 * 1000000LL);
	}
}

// Starts the two looping hardware channels on a silent ring (start, and back from HOME / sleep).
void CsndHwPlay()
{
	memset(Ring[0], 0, RING * sizeof(s16));
	memset(Ring[1], 0, RING * sizeof(s16));
	GSPGPU_FlushDataCache(Ring[0], RING * sizeof(s16));
	GSPGPU_FlushDataCache(Ring[1], RING * sizeof(s16));
	for (int s = 0; s < 2; ++s)
		csndPlaySound(HwChn[s], SOUND_FORMAT_16BIT | SOUND_REPEAT, OUT_RATE, 1.f, s ? 1.f : -1.f,
			Ring[s], Ring[s], RING * sizeof(s16));
	StartTick = svcGetSystemTick();
	Written = 0;
}

void CsndHwStop()
{
	for (int s = 0; s < 2; ++s)
		if (HwChn[s] >= 0)
			CSND_SetPlayState(HwChn[s], 0);
	csndExecCmds(false);
}

bool CsndStart()
{
	if (R_FAILED(csndInit()))
		return false;
	for (int bit = 0, n = 0; bit < 32 && n < 2; ++bit)
		if (csndChannels & BIT(bit))
			HwChn[n++] = bit;
	Ring[0] = (s16 *)linearAlloc(RING * sizeof(s16));
	Ring[1] = (s16 *)linearAlloc(RING * sizeof(s16));
	if (HwChn[1] < 0 || Ring[0] == NULL || Ring[1] == NULL)
	{
		sndExit();
		return false;
	}
	LightLock_Init(&Lock);
	for (int i = 0; i < CHANNELS; ++i)
		ResetChn(Chn[i]);
	u32 timer = CSND_TIMER(OUT_RATE);
	HwRate = (double)0x3FEC3FC / timer;
	CsndHwPlay();
	UseCsnd = true;
	MixerRun = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Mixer = threadCreate(MixerThread, NULL, 16 * 1024, prio - 2, -2, false);
	return Mixer != NULL;
}

} // namespace

Result sndInit()
{
	int firm = I3DS_EnsureDspFirmware();
	if (firm == 2)
		Printf("Audio: DSP firmware extracted from this console to sdmc:/3ds/dspfirm.cdc\n");
	if (Args == NULL || !Args->CheckParm("-csnd"))	// -csnd: force the fallback (testing)
	{
		Result rc = ndspInit();
		if (R_SUCCEEDED(rc))
		{
			Backend = "NDSP";
			return rc;
		}
		Printf("Audio: DSP not available (0x%08lX), trying CSND\n", (unsigned long)rc);
	}
	if (CsndStart())
	{
		Backend = "CSND";
		Printf("Audio: CSND software mixer (no DSP firmware needed)\n");
		return 0;
	}
	Printf("Audio: CSND not available either - sound disabled\n");
	return MAKERESULT(RL_PERMANENT, RS_NOTSUPPORTED, RM_APPLICATION, RD_NOT_FOUND);
}

void sndExit()
{
	Backend = "off";
	if (!UseCsnd)
	{
		ndspExit();
		return;
	}
	if (Mixer != NULL)
	{
		MixerRun = false;
		threadJoin(Mixer, U64_MAX);
		threadFree(Mixer);
		Mixer = NULL;
	}
	for (int s = 0; s < 2; ++s)
		if (HwChn[s] >= 0)
			CSND_SetPlayState(HwChn[s], 0);
	csndExecCmds(true);
	csndExit();
	for (int s = 0; s < 2; ++s)
	{
		if (Ring[s]) linearFree(Ring[s]);
		Ring[s] = NULL;
		HwChn[s] = -1;
	}
	UseCsnd = false;
}

bool sndUsingCsnd() { return UseCsnd; }

const char *sndBackendName() { return Backend; }

void sndSetSuspended(bool suspended)
{
	if (suspended == Suspended)
		return;
	if (suspended)
	{
		Suspended = true;
		if (UseCsnd)
		{
			LightLock_Lock(&Lock);	// the mixer is not inside MixInto
			CsndHwStop();
			LightLock_Unlock(&Lock);
		}
	}
	else
	{
		if (UseCsnd)
		{
			LightLock_Lock(&Lock);
			CsndHwPlay();
			LightLock_Unlock(&Lock);
		}
		Suspended = false;
	}
}

bool sndSuspended() { return Suspended; }

void sndEmergencyStop()
{
	if (UseCsnd)
		CsndHwStop();
	else if (dspIsComponentLoaded())
		DSP_UnloadComponent();
}

void sndSetOutputMode(ndspOutputMode mode) { if (!UseCsnd) ndspSetOutputMode(mode); }
void sndSetOutputCount(int count) { if (!UseCsnd) ndspSetOutputCount(count); }
void sndSetMasterVol(float volume) { if (!UseCsnd) ndspSetMasterVol(volume); else MasterVol = volume; }

void sndChnReset(int id)
{
	if (!UseCsnd) { ndspChnReset(id); return; }
	LightLock_Lock(&Lock);
	ResetChn(Chn[id]);
	LightLock_Unlock(&Lock);
}

void sndChnSetInterp(int id, ndspInterpType type) { if (!UseCsnd) ndspChnSetInterp(id, type); }
void sndChnSetRate(int id, float rate) { if (!UseCsnd) ndspChnSetRate(id, rate); else Chn[id].rate = rate; }

void sndChnSetFormat(int id, u16 format)
{
	if (!UseCsnd) ndspChnSetFormat(id, format);
	else Chn[id].stereo = (format & 3) == 2;	// NDSP_CHANNELS(2)
}

void sndChnSetMix(int id, float mix[12])
{
	if (!UseCsnd) { ndspChnSetMix(id, mix); return; }
	Chn[id].mix[0] = mix[0];
	Chn[id].mix[1] = mix[1];
}

void sndChnSetPaused(int id, bool paused) { if (!UseCsnd) ndspChnSetPaused(id, paused); else Chn[id].paused = paused; }
bool sndChnIsPlaying(int id) { return !UseCsnd ? ndspChnIsPlaying(id) : Chn[id].head != NULL && !Chn[id].paused; }
u32 sndChnGetSamplePos(int id) { return !UseCsnd ? ndspChnGetSamplePos(id) : (u32)Chn[id].pos; }

void sndChnWaveBufAdd(int id, ndspWaveBuf *buf)
{
	if (!UseCsnd) { ndspChnWaveBufAdd(id, buf); return; }
	LightLock_Lock(&Lock);
	buf->next = NULL;
	buf->status = NDSP_WBUF_QUEUED;
	SoftChn &c = Chn[id];
	if (c.tail != NULL)
		c.tail->next = buf;
	else
		c.head = buf, c.pos = 0;
	c.tail = buf;
	LightLock_Unlock(&Lock);
}

void sndChnWaveBufClear(int id)
{
	if (!UseCsnd) { ndspChnWaveBufClear(id); return; }
	LightLock_Lock(&Lock);
	SoftChn &c = Chn[id];
	for (ndspWaveBuf *b = c.head; b != NULL; b = b->next)
		b->status = NDSP_WBUF_DONE;
	c.head = c.tail = NULL;
	c.pos = 0;
	LightLock_Unlock(&Lock);
}
