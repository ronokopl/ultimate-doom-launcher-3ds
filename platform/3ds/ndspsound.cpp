// 3DS sound backend on NDSP (replaces FMOD/OpenAL). Subsystems sheet: sound; Audio sheet: mixer, sfx_*, music_*.
// - sfx: decoded to 16-bit mono in linear RAM (22050Hz when the source is >= 32kHz), one NDSP voice each.
// - 3D: rolloff via S_GetRolloff + stereo pan from the listener angle (computed every UpdateSounds).
// - streams (OPL MIDI/MUS music, OGG music): ring of NDSP wave buffers refilled by a thread (core 2 on New 3DS).
#include <3ds.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "doomtype.h"
#include "doomdef.h"
#include "templates.h"
#include "c_cvars.h"
#include "i_sound.h"
#include "i_soundinternal.h"
#include "files.h"
#include "v_text.h"
#include "s_sound.h"
#define BD3DS_NO_ENGINE_KEYS
#include "bd3ds_sheets.h"
#include "csnd_3ds.h"
#include "i_3ds.h"

EXTERN_CVAR (Bool, snd_pitched)

#define SFX_VOICES		22		// Audio sheet: mixer = 24 ndsp channels -> 22 sfx + 2 streams
#define STREAM_VOICE0	22
#define STREAM_VOICES	2
#define STREAM_BUFS		4

SoundDecoder *I3DS_CreateVorbisDecoder();
SoundDecoder *I3DS_CreateWavDecoder();

struct NDSPSample
{
	s16 *PCM;
	u32 Frames;
	int Rate;
};

struct NDSPVoice
{
	FISoundChannel *Chan;
	NDSPSample *Sample;
	ndspWaveBuf Wave;
	float Volume;
	bool Is3D, Area, Loop, Paused, Active;
	FVector3 Pos;
	int Pitch;
};

class NDSPSoundRenderer;
static NDSPSoundRenderer *TheRenderer;
static LightLock StreamLock;
static bool StreamLockReady;

// HOME / sleep (APT suspend hook, right after sndSetSuspended(true)): waits for a music stream update that is
// already running on the stream thread. The thread checks the suspended state under the same lock, so after this
// nothing of ours touches the DSP while libctru puts it to sleep (the IPC trace showed a DSP call after the flag).
void I3DS_SoundQuiesce()
{
	if (StreamLockReady)
	{
		LightLock_Lock(&StreamLock);
		LightLock_Unlock(&StreamLock);
	}
}

// ---------------------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------------------

class NDSPStream : public SoundStream
{
public:
	NDSPStream(int voice) : Voice(voice), Callback(NULL), UserData(NULL), Decoder(NULL), Reader(NULL),
		Playing(false), Paused(false), Ended(false), Looping(false), Volume(1.f), Frames(0), Position(0)
	{
		memset(Bufs, 0, sizeof(Bufs));
		memset(Data, 0, sizeof(Data));
	}
	~NDSPStream();

	bool InitCallback(SoundStreamCallback cb, int buffbytes, int flags, int rate, void *user)
	{
		Callback = cb; UserData = user; Rate = rate;
		Stereo = !(flags & Mono);
		InFormat = (flags & Float) ? 2 : (flags & Bits32) ? 3 : (flags & Bits8) ? 1 : 0;
		int insize = (InFormat == 1) ? 1 : (InFormat == 0) ? 2 : 4;
		Frames = buffbytes / (insize * (Stereo ? 2 : 1));
		if (Frames < 256) Frames = 256;
		Scratch.Resize(Frames * insize * (Stereo ? 2 : 1));
		return AllocBuffers();
	}

	bool InitDecoder(FileReader *reader, SoundDecoder *decoder, bool loop)
	{
		Reader = reader; Decoder = decoder; Looping = loop;
		ChannelConfig chans; SampleType type;
		decoder->getInfo(&Rate, &chans, &type);
		Stereo = chans == ChannelConfig_Stereo;
		InFormat = type == SampleType_UInt8 ? 1 : 0;
		Frames = 4096;
		Scratch.Resize(Frames * 4);
		return AllocBuffers();
	}

	bool Play(bool looping, float volume)
	{
		Looping = Looping || looping;
		Volume = volume;
		sndChnReset(Voice);
		sndChnSetInterp(Voice, NDSP_INTERP_LINEAR);
		sndChnSetRate(Voice, (float)Rate);
		sndChnSetFormat(Voice, Stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		ApplyVolume();
		Ended = false;
		for (int i = 0; i < STREAM_BUFS; ++i)
		{
			Fill(i);
		}
		Playing = true;
		return true;
	}
	void Stop()
	{
		LightLock_Lock(&StreamLock);
		Playing = false;
		sndChnWaveBufClear(Voice);
		LightLock_Unlock(&StreamLock);
	}
	void SetVolume(float volume) { Volume = volume; ApplyVolume(); }
	bool SetPaused(bool paused) { Paused = paused; sndChnSetPaused(Voice, paused); return true; }
	// HOME Menu: the sound hardware closes under the stream and reopens later; the source position is kept
	void Detach()
	{
		sndChnWaveBufClear(Voice);
		memset(Bufs, 0, sizeof(Bufs));
	}
	void Resume()
	{
		sndChnReset(Voice);
		sndChnSetInterp(Voice, NDSP_INTERP_LINEAR);
		sndChnSetRate(Voice, (float)Rate);
		sndChnSetFormat(Voice, Stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
		ApplyVolume();
		sndChnSetPaused(Voice, Paused);
		for (int i = 0; i < STREAM_BUFS && Playing; ++i)
			Fill(i);
	}
	unsigned int GetPosition() { return Rate ? (unsigned int)((unsigned long long)Position * 1000 / Rate) : 0; }
	bool IsEnded() { return Ended; }
	bool SetPosition(unsigned int ms) { return Decoder != NULL && Decoder->seek(ms); }

	// called from the stream thread
	void Service()
	{
		if (!Playing || Paused)
			return;
		for (int i = 0; i < STREAM_BUFS; ++i)
		{
			if (Bufs[i].status == NDSP_WBUF_DONE)
				Fill(i);
		}
	}

	void ApplyVolume();

private:
	bool AllocBuffers()
	{
		for (int i = 0; i < STREAM_BUFS; ++i)
		{
			Data[i] = (s16 *)linearAlloc(Frames * 2 * sizeof(s16));
			if (Data[i] == NULL) return false;
			memset(Data[i], 0, Frames * 2 * sizeof(s16));
		}
		return true;
	}

	void Fill(int i)
	{
		s16 *out = Data[i];
		int chans = Stereo ? 2 : 1;
		int samples = Frames * chans;
		bool ok = true;
		if (Callback != NULL)
		{
			int insize = (InFormat == 1) ? 1 : (InFormat == 0) ? 2 : 4;
			ok = Callback(this, &Scratch[0], samples * insize, UserData);
			Convert(&Scratch[0], out, samples);
		}
		else if (Decoder != NULL)
		{
			int insize = InFormat == 1 ? 1 : 2;
			size_t want = samples * insize, got = Decoder->read(&Scratch[0], want);
			if (got < want && Looping)
			{
				Decoder->seek(0);
				got += Decoder->read(&Scratch[0] + got, want - got);
			}
			memset(&Scratch[0] + got, InFormat == 1 ? 0x80 : 0, want - got);
			Convert(&Scratch[0], out, samples);
			if (got == 0 && !Looping) ok = false;
		}
		if (!ok)
		{
			Ended = true;
			Playing = false;
			return;
		}
		Position += Frames;
		DSP_FlushDataCache(out, samples * sizeof(s16));
		memset(&Bufs[i], 0, sizeof(ndspWaveBuf));
		Bufs[i].data_vaddr = out;
		Bufs[i].nsamples = Frames;
		sndChnWaveBufAdd(Voice, &Bufs[i]);
	}

	void Convert(const char *in, s16 *out, int samples)
	{
		switch (InFormat)
		{
		case 0: memcpy(out, in, samples * 2); break;
		case 1: for (int s = 0; s < samples; ++s) out[s] = (s16)(((int)(BYTE)in[s] - 128) << 8); break;
		case 2:
			for (int s = 0; s < samples; ++s)
			{
				float f = ((const float *)in)[s] * 32767.f;
				out[s] = (s16)clamp(f, -32768.f, 32767.f);
			}
			break;
		case 3: for (int s = 0; s < samples; ++s) out[s] = (s16)(((const int *)in)[s] >> 16); break;
		}
	}

	int Voice;
	SoundStreamCallback Callback;
	void *UserData;
	SoundDecoder *Decoder;
	FileReader *Reader;
	bool Playing, Paused, Ended, Looping, Stereo;
	float Volume;
	int Rate, InFormat;
	int Frames;
	unsigned int Position;
	ndspWaveBuf Bufs[STREAM_BUFS];
	s16 *Data[STREAM_BUFS];
	TArray<char> Scratch;
};

// ---------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------

class NDSPSoundRenderer : public SoundRenderer
{
public:
	NDSPSoundRenderer() : Valid(false), Released(false), SfxVolume(1.f), MusicVolume(1.f), ThreadRun(false), Worker(NULL)
	{
		memset(Voices, 0, sizeof(Voices));
		memset(Streams, 0, sizeof(Streams));
		LightLock_Init(&StreamLock);
		StreamLockReady = true;
		Result rc = sndInit();
		FILE *diag = fopen(BD3DS_PATH_APP_DIR "ndsp.txt", "w");	// survives even before the engine log exists
		if (diag) { fprintf(diag, "ndspInit rc=0x%08lX\n", (unsigned long)rc); fclose(diag); }
		if (R_FAILED(rc))
		{
			Printf (TEXTCOLOR_RED "No sound output (DSP and CSND both unavailable, 0x%08lX). Sound disabled.\n", (unsigned long)rc);
			return;
		}
		OutputSetup();
		Valid = true;
		Printf ("%s: %d sfx voices + %d stream voices\n", sndUsingCsnd() ? "CSND" : "NDSP", SFX_VOICES, STREAM_VOICES);
		TheRenderer = this;
		StartWorker();
	}
	~NDSPSoundRenderer()
	{
		StopWorker();
		if (Valid && !Released)
		{
			for (int i = 0; i < SFX_VOICES; ++i)
				sndChnReset(i);
			sndExit();
		}
		TheRenderer = NULL;
	}

	// HOME Menu (APT suspend hook): the sound hardware is closed completely - no DSP session, component or refill
	// thread of ours is left, as on the launcher screen, where HOME works. The handover trace showed the game's DSP
	// sleep and the jump completing and then the HOME Menu itself hanging. The engine evicted its channels first
	// (I3DS_SoundRelease); streams keep their position and samples stay loaded.
	void ReleaseHardware()
	{
		if (!Valid || Released)
			return;
		StopWorker();
		for (int i = 0; i < SFX_VOICES; ++i)
			if (Voices[i].Active)
				StopVoice(i, true);
		for (int i = 0; i < STREAM_VOICES; ++i)
			if (Streams[i])
				Streams[i]->Detach();
		sndExit();
		Released = true;
	}
	bool RestoreHardware()
	{
		if (!Released)
			return Valid;
		Released = false;
		if (R_FAILED(sndInit()))
		{
			Valid = false;	// stays silent; the engine keeps running
			return false;
		}
		OutputSetup();
		for (int i = 0; i < STREAM_VOICES; ++i)
			if (Streams[i])
				Streams[i]->Resume();
		StartWorker();
		return true;
	}

	bool IsValid() { return Valid; }

	void SetSfxVolume(float volume) { SfxVolume = volume; }
	void SetMusicVolume(float volume)
	{
		MusicVolume = volume;
		for (int i = 0; i < STREAM_VOICES; ++i)
			if (Streams[i]) Streams[i]->ApplyVolume();
	}
	float MusicVol() const { return MusicVolume; }

	// ---- samples -------------------------------------------------------------------

	SoundHandle LoadSound(BYTE *sfxdata, int length)
	{
		SoundHandle retval = { NULL };
		MemoryReader reader((const char *)sfxdata, length);
		SoundDecoder *decoder = CreateDecoder(&reader);
		if (decoder == NULL)
			return retval;
		int rate; ChannelConfig chans; SampleType type;
		decoder->getInfo(&rate, &chans, &type);
		int channels = chans == ChannelConfig_Stereo ? 2 : 1;
		int bits = type == SampleType_UInt8 ? 8 : 16;
		// A known length is read into one exact buffer: readAll doubles its buffer, which for a long stereo sound
		// needs a 12 MB block next to the old one.
		TArray<char> data;
		size_t frames = decoder->getSampleLength();
		if (frames > 0)
		{
			data.Resize((unsigned)(frames * channels * (bits / 8)));
			size_t got = 0, n;
			while (got < data.Size() && (n = decoder->read(&data[got], data.Size() - got)) > 0)
				got += n;
			data.Resize((unsigned)got);
		}
		else
			data = decoder->readAll();
		delete decoder;
		return MakeSample(data.Size() ? (const BYTE *)&data[0] : NULL, data.Size(), rate, channels, bits);
	}

	SoundHandle LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
	{
		return MakeSample(sfxdata, length, frequency, channels, bits);
	}

	void UnloadSound(SoundHandle sfx)
	{
		NDSPSample *s = (NDSPSample *)sfx.data;
		if (s == NULL) return;
		for (int i = 0; i < SFX_VOICES; ++i)
			if (Voices[i].Active && Voices[i].Sample == s)
				StopVoice(i, true);
		linearFree(s->PCM);
		delete s;
	}

	unsigned int GetMSLength(SoundHandle sfx)
	{
		NDSPSample *s = (NDSPSample *)sfx.data;
		return s ? (unsigned int)((unsigned long long)s->Frames * 1000 / s->Rate) : 0;
	}
	unsigned int GetSampleLength(SoundHandle sfx)
	{
		NDSPSample *s = (NDSPSample *)sfx.data;
		return s ? s->Frames : 0;
	}
	float GetOutputRate() { return 32728.f; }	// NDSP native mixing rate

	// ---- streams --------------------------------------------------------------------

	SoundStream *CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
	{
		int slot = FreeStreamSlot();
		if (slot < 0)
		{
			Printf (TEXTCOLOR_RED "NDSP: no free stream voice\n");
			return NULL;
		}
		NDSPStream *s = new NDSPStream(STREAM_VOICE0 + slot);
		if (!s->InitCallback(callback, buffbytes, flags, samplerate, userdata))
		{
			Printf (TEXTCOLOR_RED "NDSP: stream buffers failed (%d bytes, flags %d, %d Hz)\n", buffbytes, flags, samplerate);
			delete s;
			return NULL;
		}
		Register(slot, s);
		return s;
	}

	SoundStream *OpenStream(FileReader *reader, int flags)
	{
		int slot = FreeStreamSlot();
		if (slot < 0) { delete reader; return NULL; }
		SoundDecoder *decoder = CreateDecoder(reader);
		if (decoder == NULL) { delete reader; return NULL; }
		NDSPStream *s = new NDSPStream(STREAM_VOICE0 + slot);
		if (!s->InitDecoder(reader, decoder, !!(flags & SoundStream::Loop)))
		{
			delete s;
			return NULL;
		}
		Register(slot, s);
		return s;
	}

	void StreamDeleted(NDSPStream *s)
	{
		LightLock_Lock(&StreamLock);
		for (int i = 0; i < STREAM_VOICES; ++i)
			if (Streams[i] == s) Streams[i] = NULL;
		LightLock_Unlock(&StreamLock);
	}

	// ---- channels -------------------------------------------------------------------

	FISoundChannel *StartSound(SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan)
	{
		int v = StartVoice(sfx, vol, pitch, chanflags);
		if (v < 0) return NULL;
		Voices[v].Is3D = false;
		ApplyMix(v, vol * SfxVolume, 0.f);
		return Bind(v, reuse_chan, NULL, 1.f, false);
	}

	FISoundChannel *StartSound3D(SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale,
		int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan)
	{
		int v = StartVoice(sfx, vol, pitch, chanflags);
		if (v < 0) return NULL;
		Voices[v].Is3D = true;
		Voices[v].Area = !!(chanflags & SNDF_AREA);
		Voices[v].Pos = pos;
		FISoundChannel *chan = Bind(v, reuse_chan, rolloff, distscale, true);
		Spatialize(v, listener);
		return chan;
	}

	void StopChannel(FISoundChannel *chan)
	{
		int v = VoiceOf(chan);
		if (v >= 0) StopVoice(v, true);
	}

	void ChannelVolume(FISoundChannel *chan, float volume)
	{
		int v = VoiceOf(chan);
		if (v < 0) return;
		Voices[v].Volume = volume;
		if (!Voices[v].Is3D) ApplyMix(v, volume * SfxVolume, 0.f);
	}

	void MarkStartTime(FISoundChannel *chan)
	{
		chan->StartTime.AsOne = osGetTime();
	}

	unsigned int GetPosition(FISoundChannel *chan)
	{
		int v = VoiceOf(chan);
		return v >= 0 ? sndChnGetSamplePos(v) : 0;
	}

	float GetAudibility(FISoundChannel *chan)
	{
		int v = VoiceOf(chan);
		return v >= 0 ? Voices[v].Volume : 0.f;
	}

	void Sync(bool sync) {}

	void SetSfxPaused(bool paused, int slot)
	{
		for (int i = 0; i < SFX_VOICES; ++i)
			if (Voices[i].Active) sndChnSetPaused(i, paused);
	}

	void SetInactive(EInactiveState inactive)
	{
		bool pause = inactive != INACTIVE_Active;
		for (int i = 0; i < SFX_VOICES; ++i)
			if (Voices[i].Active) sndChnSetPaused(i, pause);
	}

	void UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel)
	{
		int v = VoiceOf(chan);
		if (v < 0) return;
		Voices[v].Pos = pos;
		Voices[v].Area = areasound;
		Spatialize(v, listener);
	}

	void UpdateListener(SoundListener *listener)
	{
		Listener = *listener;
	}

	void UpdateSounds()
	{
		for (int i = 0; i < SFX_VOICES; ++i)
		{
			NDSPVoice &vo = Voices[i];
			if (!vo.Active) continue;
			if (!vo.Loop && !vo.Paused && !sndChnIsPlaying(i) && vo.Wave.status == NDSP_WBUF_DONE)
			{
				StopVoice(i, true);
			}
			else if (vo.Is3D && vo.Chan != NULL)
			{
				Spatialize(i, &Listener);
			}
		}
	}

	void PrintStatus() { Printf("NDSP: %d sfx voices, %d stream voices\n", SFX_VOICES, STREAM_VOICES); }
	void PrintDriversList() { Printf("NDSP (3DS DSP)\n"); }

	SoundDecoder *CreateDecoder(FileReader *reader) { return SoundRenderer::CreateDecoder(reader); }

private:
	void OutputSetup()
	{
		sndSetOutputMode(NDSP_OUTPUT_STEREO);
		sndSetOutputCount(1);
		sndSetMasterVol(1.0f);
	}
	void StartWorker()
	{
		ThreadRun = true;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		bool n3ds = false;
		APT_CheckNew3DS(&n3ds);
		Worker = threadCreate(StreamThread, this, 32 * 1024, prio - 1, n3ds ? 2 : -2, false);
		if (Worker == NULL)
			Worker = threadCreate(StreamThread, this, 32 * 1024, prio - 1, -2, false);
	}
	void StopWorker()
	{
		if (Worker)
		{
			ThreadRun = false;
			threadJoin(Worker, U64_MAX);
			threadFree(Worker);
			Worker = NULL;
		}
	}

	static void StreamThread(void *arg)
	{
		NDSPSoundRenderer *self = (NDSPSoundRenderer *)arg;
		I3DS_HomeTraceThread();
		while (self->ThreadRun)
		{
			if (sndSuspended())	// HOME / sleep: no DSP access while libctru puts it to sleep
			{
				svcSleepThread(4 * 1000000LL);
				continue;
			}
			LightLock_Lock(&StreamLock);
			if (!sndSuspended())	// checked again under the lock: I3DS_SoundQuiesce
				for (int i = 0; i < STREAM_VOICES; ++i)
					if (self->Streams[i]) self->Streams[i]->Service();
			LightLock_Unlock(&StreamLock);
			svcSleepThread(4 * 1000000LL);
		}
	}

	int FreeStreamSlot()
	{
		for (int i = 0; i < STREAM_VOICES; ++i)
			if (Streams[i] == NULL) return i;
		return -1;
	}
	void Register(int slot, NDSPStream *s)
	{
		LightLock_Lock(&StreamLock);
		Streams[slot] = s;
		LightLock_Unlock(&StreamLock);
	}

	SoundHandle MakeSample(const BYTE *data, int length, int rate, int channels, int bits)
	{
		SoundHandle retval = { NULL };
		int bytesper = bits / 8;
		if (data == NULL || rate <= 0 || channels < 1 || length < bytesper * channels)
			return retval;
		u32 frames = length / (bytesper * channels);
		// Audio sheet sfx target: 16-bit mono, halved to ~22kHz when the source rate is >= 32kHz.
		int step = rate >= 32000 ? 2 : 1;
		u32 outframes = frames / step;
		if (outframes == 0) return retval;
		s16 *pcm = (s16 *)linearAlloc(outframes * sizeof(s16));
		if (pcm == NULL) return retval;
		for (u32 f = 0; f < outframes; ++f)
		{
			int acc = 0;
			for (int k = 0; k < step; ++k)
			{
				const BYTE *p = data + ((f * step + k) * channels) * bytesper;
				for (int c = 0; c < channels; ++c)
				{
					int s = bits == 8 ? ((int)p[c] - 128) << 8 : (int)(SWORD)(p[c * 2] | (p[c * 2 + 1] << 8));
					acc += s;
				}
			}
			pcm[f] = (s16)(acc / (step * channels));
		}
		DSP_FlushDataCache(pcm, outframes * sizeof(s16));
		NDSPSample *s = new NDSPSample;
		s->PCM = pcm;
		s->Frames = outframes;
		s->Rate = rate / step;
		retval.data = s;
		return retval;
	}

	int StartVoice(SoundHandle sfx, float vol, int pitch, int chanflags)
	{
		NDSPSample *s = (NDSPSample *)sfx.data;
		if (!Valid || s == NULL) return -1;
		int v = -1;
		for (int i = 0; i < SFX_VOICES; ++i)
			if (!Voices[i].Active) { v = i; break; }
		if (v < 0)
		{	// steal the quietest voice
			float lowest = 1e9f;
			for (int i = 0; i < SFX_VOICES; ++i)
				if (Voices[i].Volume < lowest) { lowest = Voices[i].Volume; v = i; }
			if (v < 0 || lowest > vol) return -1;
			StopVoice(v, true);
		}
		NDSPVoice &vo = Voices[v];
		vo.Sample = s;
		vo.Volume = vol;
		vo.Loop = !!(chanflags & SNDF_LOOP);
		vo.Paused = false;
		vo.Pitch = pitch;
		vo.Active = true;
		sndChnReset(v);
		sndChnSetInterp(v, NDSP_INTERP_LINEAR);
		float pitchmul = snd_pitched ? pitch / 128.f : 1.f;
		sndChnSetRate(v, s->Rate * pitchmul);
		sndChnSetFormat(v, NDSP_FORMAT_MONO_PCM16);
		memset(&vo.Wave, 0, sizeof(ndspWaveBuf));
		vo.Wave.data_vaddr = s->PCM;
		vo.Wave.nsamples = s->Frames;
		vo.Wave.looping = vo.Loop;
		sndChnWaveBufAdd(v, &vo.Wave);
		return v;
	}

	FISoundChannel *Bind(int v, FISoundChannel *reuse, FRolloffInfo *rolloff, float distscale, bool manual)
	{
		FISoundChannel *chan = reuse;
		if (chan == NULL) chan = S_GetChannel((void *)(intptr_t)(v + 1));
		else chan->SysChannel = (void *)(intptr_t)(v + 1);
		if (rolloff) chan->Rolloff = *rolloff;
		else
		{
			chan->Rolloff.RolloffType = ROLLOFF_Log;
			chan->Rolloff.RolloffFactor = 0.f;
			chan->Rolloff.MinDistance = 1.f;
		}
		chan->DistanceScale = distscale;
		chan->ManualRolloff = manual;
		Voices[v].Chan = chan;
		return chan;
	}

	int VoiceOf(FISoundChannel *chan)
	{
		if (chan == NULL || chan->SysChannel == NULL) return -1;
		int v = (int)(intptr_t)chan->SysChannel - 1;
		return (v >= 0 && v < SFX_VOICES && Voices[v].Active && Voices[v].Chan == chan) ? v : -1;
	}

	void StopVoice(int v, bool notify)
	{
		NDSPVoice &vo = Voices[v];
		sndChnWaveBufClear(v);
		vo.Active = false;
		FISoundChannel *chan = vo.Chan;
		vo.Chan = NULL;
		vo.Sample = NULL;
		vo.Volume = 0.f;
		if (chan != NULL)
		{
			chan->SysChannel = NULL;
			if (notify) S_ChannelEnded(chan);
		}
	}

	void ApplyMix(int v, float vol, float pan)
	{
		float mix[12];
		memset(mix, 0, sizeof(mix));
		vol = clamp(vol, 0.f, 1.f);
		pan = clamp(pan, -1.f, 1.f);
		mix[0] = vol * (pan > 0 ? 1.f - pan : 1.f);	// left
		mix[1] = vol * (pan < 0 ? 1.f + pan : 1.f);	// right
		sndChnSetMix(v, mix);
	}

	// Doom-style positional audio: FMOD coordinates (x, height, y); listener angle in radians.
	void Spatialize(int v, SoundListener *listener)
	{
		NDSPVoice &vo = Voices[v];
		FISoundChannel *chan = vo.Chan;
		if (chan == NULL || listener == NULL || !listener->valid)
		{
			ApplyMix(v, vo.Volume * SfxVolume, 0.f);
			return;
		}
		float dx = vo.Pos.X - listener->position.X;
		float dy = vo.Pos.Y - listener->position.Y;
		float dz = vo.Pos.Z - listener->position.Z;
		float dist = sqrtf(dx*dx + dy*dy + dz*dz);
		float gain = S_GetRolloff(&chan->Rolloff, dist * chan->DistanceScale, true);
		float pan = 0.f;
		if (!vo.Area && dist > 1.f)
		{
			float rel = atan2f(dz, dx) - listener->angle;
			pan = -sinf(rel) * 0.75f;
		}
		ApplyMix(v, vo.Volume * gain * SfxVolume, pan);
	}

	bool Valid, Released;
	float SfxVolume, MusicVolume;
	volatile bool ThreadRun;
	Thread Worker;
	NDSPVoice Voices[SFX_VOICES];
	NDSPStream *Streams[STREAM_VOICES];
	SoundListener Listener;
};

NDSPStream::~NDSPStream()
{
	Stop();
	if (TheRenderer) TheRenderer->StreamDeleted(this);	// the refill thread must not see us any more
	for (int i = 0; i < STREAM_BUFS; ++i)
		if (Data[i]) linearFree(Data[i]);
	if (Decoder) delete Decoder;
	if (Reader) delete Reader;
}

void NDSPStream::ApplyVolume()
{
	float mix[12];
	memset(mix, 0, sizeof(mix));
	float v = Volume * (TheRenderer ? TheRenderer->MusicVol() : 1.f);
	mix[0] = mix[1] = v;
	sndChnSetMix(Voice, mix);
}

// OpenAL is not part of the 3DS build; the sound options menu still asks about it.
struct FOptionValues;
bool IsOpenALPresent() { return false; }
void I_BuildALDeviceList(FOptionValues *opt) {}

SoundRenderer *I3DS_CreateSoundRenderer()
{
	return new NDSPSoundRenderer;
}

// APT hooks (i_input_3ds.cpp): HOME Menu suspend / restore around the whole sound system
void I3DS_SoundRelease()
{
	if (TheRenderer == NULL)
		return;
	S_EvictAllChannels();	// the engine remembers playing sounds and restarts them on return
	TheRenderer->ReleaseHardware();
	I3DS_HomeTrace("sound: closed for the HOME Menu (no DSP session left)");
}

void I3DS_SoundRestore()
{
	if (TheRenderer == NULL)
		return;
	bool ok = TheRenderer->RestoreHardware();
	I3DS_HomeTrace(ok ? "sound: reopened" : "sound: reopen failed, sound stays off");
	if (ok)
		S_RestoreEvictedChannels();
}
