// Hitch profiler (Subsystems sheet: profiler). Off unless -profile3ds is on the command line (args.txt).
// Linker --wrap hooks (Build sheet ld_extra) time zlib inflate (PNG sprites, compressed lumps), tremor ov_read
// (OGG sounds) and fread (SD/romfs I/O); every frame slower than 50 ms is logged with that breakdown.
#include <3ds.h>
#include <stdio.h>
#include <zlib.h>
#include <tremor/ivorbisfile.h>

#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "i_3ds.h"
#include "bd3ds_sheets.h"
#include "c_dispatch.h"

static int Enabled = -1;	// -1 = not decided yet
static FILE *Log;
static u64 Last, InflateT, OggT, ReadT, SeekT;
static unsigned ReadBytes, InflateCalls, OggCalls, SeekCalls;
static bool Dumped;
static int GibTest = -1, LevelFrames;
static double SumMs, MaxMs;
static unsigned StatFrames;

// -gibtest: scripted gore stress (in-level frame number -> console command), markers go to hitches.txt
static const struct { int frame; const char *cmd; } GibScript[] = {
	{ 150, "kill monsters" },
	{ 300, "summon Mancubus; summon Arachnotron; summon Revenant; summon HellKnight; summon Demon" },
	{ 340, "kill monsters" },
	{ 500, "summon Cyberdemon" },
	{ 530, "kill monsters" },
};

#define TICKS_PER_MS (SYSCLOCK_ARM11 / 1000.0)

extern "C"
{
	int __real_inflate(z_streamp strm, int flush);
	int __wrap_inflate(z_streamp strm, int flush)
	{
		u64 t = svcGetSystemTick();
		int r = __real_inflate(strm, flush);
		InflateT += svcGetSystemTick() - t; InflateCalls++;
		return r;
	}

	long __real_ov_read(OggVorbis_File *vf, char *buffer, int length, int *bitstream);
	long __wrap_ov_read(OggVorbis_File *vf, char *buffer, int length, int *bitstream)
	{
		u64 t = svcGetSystemTick();
		long r = __real_ov_read(vf, buffer, length, bitstream);
		OggT += svcGetSystemTick() - t; OggCalls++;
		return r;
	}

	size_t __real_fread(void *ptr, size_t size, size_t n, FILE *f);
	size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
	{
		u64 t = svcGetSystemTick();
		size_t r = __real_fread(ptr, size, n, f);
		ReadT += svcGetSystemTick() - t; ReadBytes += r * size;
		return r;
	}

	int __real_fseek(FILE *f, long offset, int whence);
	int __wrap_fseek(FILE *f, long offset, int whence)
	{
		u64 t = svcGetSystemTick();
		int r = __real_fseek(f, offset, whence);
		SeekT += svcGetSystemTick() - t; SeekCalls++;
		return r;
	}
}

// While profiling, every 300th frame the real top/bottom framebuffers (what the LCDs show, BGR8, rotated)
// are written to <app_dir>/fb_top.raw / fb_bottom.raw (tools/fb2png.py converts them).
static unsigned FrameNo;

void I3DS_DumpScreen(gfxScreen_t scr, const char *path)
{
	u16 w, h;
	u8 *fb = gfxGetFramebuffer(scr, GFX_LEFT, &w, &h);
	FILE *f = fopen(path, "wb");
	if (f == NULL || fb == NULL)
	{
		if (f) fclose(f);
		return;
	}
	u16 hdr[2] = { w, h };
	fwrite(hdr, sizeof(hdr), 1, f);
	fwrite(fb, 3, (size_t)w * h, f);
	fclose(f);
}

void I3DS_ProfileDump()
{
	if (Enabled > 0 && (++FrameNo % 300) == 0)
	{
		Dumped = true;
		I3DS_DumpScreen(GFX_TOP,BD3DS_PATH_APP_DIR "fb_top.raw");
		I3DS_DumpScreen(GFX_BOTTOM, BD3DS_PATH_APP_DIR "fb_bottom.raw");
	}
}

void I3DS_ProfileFrame()
{
	if (Enabled < 0)
	{
		Enabled = Args != NULL && Args->CheckParm("-profile3ds");
		if (Enabled)
			Log = fopen(BD3DS_PATH_APP_DIR "hitches.txt", "w");
		Last = svcGetSystemTick();
	}
	if (!Enabled || Log == NULL)
		return;
	u64 now = svcGetSystemTick();
	double ms = (now - Last) / TICKS_PER_MS;
	Last = now;
	if (ms > 50.0)
	{
		fprintf(Log, "tic %6d frame %6.1f ms | inflate %6.1f ms (%u) | ogg %6.1f ms (%u) | fread %6.1f ms %6u KB | fseek %6.1f ms (%u)%s\n",
			gametic, ms, InflateT / TICKS_PER_MS, InflateCalls, OggT / TICKS_PER_MS, OggCalls,
			ReadT / TICKS_PER_MS, ReadBytes / 1024, SeekT / TICKS_PER_MS, SeekCalls, Dumped ? " (fb dump)" : "");
		fflush(Log);
	}
	if (gamestate == GS_LEVEL && !Dumped)
	{
		SumMs += ms; if (ms > MaxMs) MaxMs = ms;
		if (++StatFrames == 150)
		{
			fprintf(Log, "tic %6d avg %5.1f ms (%4.1f fps) max %6.1f ms over 150 frames\n", gametic,
				SumMs / StatFrames, 1000.0 * StatFrames / SumMs, MaxMs);
			fflush(Log);
			SumMs = MaxMs = 0; StatFrames = 0;
		}
	}
	if (GibTest < 0)
		GibTest = Args->CheckParm("-gibtest") ? 1 : 0;
	if (GibTest && gamestate == GS_LEVEL)
	{
		++LevelFrames;
		for (size_t i = 0; i < countof(GibScript); ++i)
			if (GibScript[i].frame == LevelFrames)
			{
				fprintf(Log, "tic %6d gibtest: %s\n", gametic, GibScript[i].cmd);
				AddCommandString((char *)GibScript[i].cmd);
			}
	}
	Dumped = false;
	InflateT = OggT = ReadT = SeekT = 0;
	ReadBytes = InflateCalls = OggCalls = SeekCalls = 0;
}
