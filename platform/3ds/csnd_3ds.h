// Sound output backend for ndspsound.cpp (Subsystems sheet: csnd, dspdump). The snd* calls mirror the libctru
// ndsp* functions ndspsound.cpp uses; they go to the DSP when it starts, otherwise to a software mixer played
// through CSND (no DSP firmware needed).
#pragma once
#include <3ds.h>

Result sndInit();	// extracts the DSP firmware if missing, then NDSP, else CSND
void sndExit();
bool sndUsingCsnd();
const char *sndBackendName();	// "NDSP", "CSND" or "off" (HOME trace)
// HOME / sleep (i_input_3ds.cpp APT hook): while suspended the music stream and the CSND mixer leave the sound
// hardware alone, so nothing touches the DSP while libctru puts it to sleep for the HOME Menu; the CSND output
// channels are stopped (they would loop the last buffer under the HOME Menu) and restarted on return.
void sndSetSuspended(bool suspended);
bool sndSuspended();
void sndEmergencyStop();	// the HOME supervisor ends the process: stop CSND / unload the DSP component first
void sndSetOutputMode(ndspOutputMode mode);
void sndSetOutputCount(int count);
void sndSetMasterVol(float volume);
void sndChnReset(int id);
void sndChnSetInterp(int id, ndspInterpType type);
void sndChnSetRate(int id, float rate);
void sndChnSetFormat(int id, u16 format);
void sndChnSetMix(int id, float mix[12]);
void sndChnSetPaused(int id, bool paused);
bool sndChnIsPlaying(int id);
u32  sndChnGetSamplePos(int id);
void sndChnWaveBufAdd(int id, ndspWaveBuf *buf);
void sndChnWaveBufClear(int id);
